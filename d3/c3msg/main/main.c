/*
 * c3msg — ESP32-C3 自写固件 M1
 *
 * 目标：横向 160x80 UI，与 Cardputer 通过 ESP-NOW 双向收发文字消息。
 * 不碰 Meshtastic 的 NVS / 分区表 / bootloader，只占用 app 分区(0x10000)。
 *
 * 屏幕：ST7735S 80x160（物理），逻辑横屏 160x80，MADCTL=0x00，INVON
 *       565 必须按 BGR 打包（面板按 BGR 解释），否则红蓝反。
 * 帧格式：ZD v1，24B 头 + payload<=226，小端，CRC16-CCITT(0x1021, init 0xFFFF)
 *       覆盖 [0..21] + payload。
 * 无线：ESP-NOW，channel 1，广播地址 FF:FF:FF:FF:FF:FF。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "display.h"
#include "monitor.h"
#include "mirror.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "hal/usb_serial_jtag_ll.h"

#include "ascii12.h"
#include "cn_index.h"
#include "lora.h"
#include "ble_mesh.h"

/*
 * 非阻塞日志：USB-Serial/JTAG 没被主机读取时，默认控制台会一直等 FIFO 空间，
 * 把调用任务（包括 WiFi 任务）永久阻塞 —— 表现为「USB 卡住后设备失聪」。
 * 这里改成直接写硬件 FIFO：最多等约 3ms，写不进去就丢弃，绝不无限阻塞。
 */
static void nblog(const char *fmt, ...)
{
    char b[200];
    int n = snprintf(b, sizeof(b), "[%6d] ", (int)(esp_timer_get_time() / 1000));
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof(b) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    if (n < 0) return;
    b[n++] = '\n';

    int off = 0, budget = 30;                       /* 30 * 100us = 3ms 上限 */
    while (off < n && budget-- > 0) {
        if (!usb_serial_jtag_ll_txfifo_writable()) {
            esp_rom_delay_us(100);
            continue;
        }
        int w = usb_serial_jtag_ll_write_txfifo((const uint8_t *)b + off, (uint32_t)(n - off));
        if (w <= 0) break;
        usb_serial_jtag_ll_txfifo_flush();
        off += w;
    }
}

#undef ESP_LOGI
#undef ESP_LOGW
#undef ESP_LOGE
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); nblog("I " fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); nblog("W " fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); nblog("E " fmt, ##__VA_ARGS__); } while (0)

/* 无人值守自检：1 = 上电后每 20s 自动发一条文本（仅用于验证双向链路） */
#ifndef C3MSG_BOOT_TEST
#define C3MSG_BOOT_TEST 0
#endif

/* M2 验收自检：1 = 上电后自动发「重复帧 / 长文本 / 多条消息」用于实测
 * 去重窗口、长文本折行与列表滚动（仅验证时打开，正常固件保持 0） */
#ifndef C3MSG_M2_TEST
#define C3MSG_M2_TEST 0
#endif

static const char *TAG = "c3msg";

/* ==================== 引脚（实机已核实） ==================== */
#define PIN_LORA_CS    8
#define PIN_LORA_RST   5
#define PIN_LORA_BUSY  4
#define PIN_LORA_DIO1  3
#define PIN_LORA_RXEN  12
#define PIN_BTN        9
#define PIN_BATT       2

/* 屏幕几何/旋转/颜色/初始化全部由 display.c 的面板 profile 决定 */

/* ==================== 屏幕 ==================== */
static inline void put_px(int x, int y, color_t c)
{
    display_put(x, y, c);          /* 逻辑坐标，旋转/偏移由 display.c 处理 */
}

static void fill_rect(int x, int y, int w, int h, color_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_px(x + i, y + j, c);
}

static void hline(int x, int y, int w, color_t c)
{
    for (int i = 0; i < w; i++) put_px(x + i, y, c);
}

/* ==================== 字库 ==================== */
extern const uint8_t cn12_bin_start[] asm("_binary_cn12_bin_start");

static const uint8_t *cn_glyph(uint32_t cp)
{
    int lo = 0, hi = (int)(sizeof(cn_cp) / sizeof(cn_cp[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cn_cp[mid] == cp) return cn12_bin_start + (size_t)cn_idx[mid] * 24;
        if (cn_cp[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* UTF-8 解一个码点，失败返回 0 并前进 1 字节 */
static uint32_t utf8_next(const char **ps)
{
    const uint8_t *p = (const uint8_t *)*ps;
    uint8_t c = p[0];
    if (c < 0x80) { (*ps)++; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) { *ps += 2; return ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu); }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        *ps += 3;
        return ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    }
    if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *ps += 4;
        return ((uint32_t)(c & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
               ((uint32_t)(p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
    }
    (*ps)++;
    return 0;
}

static void draw_ascii(int x, int y, char ch, color_t fg)
{
    if (ch < 0x20 || ch > 0x7E) return;
    const uint8_t *g = &ascii12[(ch - 0x20) * 12];
    for (int r = 0; r < 12; r++) {
        uint8_t bits = g[r];
        for (int c = 0; c < 7; c++)
            if (bits & (0x80 >> c)) put_px(x + c, y + r, fg);
    }
}

static void draw_cn(int x, int y, uint32_t cp, color_t fg)
{
    const uint8_t *g = cn_glyph(cp);
    if (!g) {                     /* 字库外：画个空心方框 */
        for (int i = 0; i < 10; i++) {
            put_px(x + 1 + i, y + 1, fg);
            put_px(x + 1 + i, y + 10, fg);
            put_px(x + 1, y + 1 + i, fg);
            put_px(x + 10, y + 1 + i, fg);
        }
        return;
    }
    for (int r = 0; r < 12; r++) {
        uint16_t bits = (uint16_t)((g[r * 2] << 8) | g[r * 2 + 1]);
        for (int c = 0; c < 12; c++)
            if (bits & (0x800u >> c)) put_px(x + c, y + r, fg);
    }
}

static int text_w(const char *s)
{
    int w = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        w += (cp < 0x80) ? ASCII12_W : 12;
    }
    return w;
}

/* 画一行文字，超出 maxw 截断并补 ".."；返回实际宽度 */
static int draw_text(int x, int y, const char *s, color_t fg, int maxw)
{
    int cx = x;
    while (*s) {
        const char *save = s;
        uint32_t cp = utf8_next(&s);
        int gw = (cp < 0x80) ? ASCII12_W : 12;
        if (maxw >= 0 && cx + gw > x + maxw) {
            if (x + maxw - cx >= 14) {
                draw_ascii(cx, y, '.', fg);
                draw_ascii(cx + ASCII12_W, y, '.', fg);
            }
            break;
        }
        if (cp < 0x80) draw_ascii(cx, y, (char)cp, fg);
        else draw_cn(cx, y, cp, fg);
        cx += gw;
        (void)save;
    }
    return cx - x;
}

/* 右对齐 */
static void draw_text_r(int xr, int y, const char *s, color_t fg)
{
    int w = text_w(s);
    draw_text(xr - w, y, s, fg, -1);
}

/* monitor.c 复用同一套 12px 字库（汉字 12px / ASCII 7px） */
void ui_text(int x, int y, const char *s, color_t fg, int maxw)
{
    draw_text(x, y, s, fg, maxw);
}

int ui_text_w(const char *s)
{
    return text_w(s);
}

/* ==================== 帧格式 ZD v1 ==================== */
#define ZD_HDR        24
#define ZD_MAX_PAYLOAD 226
#define ZD_MAGIC0     'Z'
#define ZD_MAGIC1     'D'
#define ZD_VER        1

enum {
    ZT_GAME = 1,
    ZT_TEXT = 2,
    ZT_BEAT = 3,
    ZT_NODETAB = 4,
    ZT_ACK = 5,
    ZT_LTEXT = 6,          /* v1.1: LoRa 文本回传给 ADV */
    ZT_LNACK = 7,          /* v1.1: LoRa 转发失败通知 */
    ZT_TIME = 8,           /* v1.2: 时间同步（UTC + 来源），ADV 与 LoRa 网双向 */
};

#define ZF_WANT_ACK   0x01
#define ZF_FORWARDED  0x02
#define ZF_LORA       0x04     /* v1.1: ZT_TEXT 请求转发到 LoRa（必须单播给桥） */
#define ZD_TTL        3

static uint32_t s_my_id;          /* 例如 0x7DF43F68 */
static uint16_t s_seq;            /* 本机发送序号：开机随机起点，之后严格递增 */

/* v1.1 桥接层前置声明（定义在 zd_send 之前，供 lora_drain 使用） */
static uint16_t zd_send(uint8_t type, const uint8_t *pl, uint16_t plen, uint16_t msgid, uint8_t flags);
static void zd_ack_echo(uint16_t seq, uint16_t msgid);
static void zd_lnack(uint16_t seq, uint16_t msgid, uint8_t reason);
static void br_on_txdone(uint16_t tag, int ok);

static uint16_t crc16_ccitt(const uint8_t *d, size_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t frame_build(uint8_t *buf, uint8_t type, uint32_t dst, uint8_t flags,
                          uint16_t seq, uint16_t msgid, uint8_t frag, uint8_t frags,
                          const uint8_t *pl, uint16_t plen)
{
    if (plen > ZD_MAX_PAYLOAD) plen = ZD_MAX_PAYLOAD;
    buf[0] = ZD_MAGIC0;
    buf[1] = ZD_MAGIC1;
    buf[2] = ZD_VER;
    buf[3] = type;
    wr32(buf + 4, s_my_id);
    wr32(buf + 8, dst);
    buf[12] = ZD_TTL;            /* ttl，每次中继减 1 */
    buf[13] = flags;
    wr16(buf + 14, seq);
    wr16(buf + 16, msgid);
    buf[18] = frag;
    buf[19] = frags;
    wr16(buf + 20, plen);
    if (plen) memcpy(buf + ZD_HDR, pl, plen);
    uint16_t crc = crc16_ccitt(buf, 22, 0xFFFF);
    crc = crc16_ccitt(buf + ZD_HDR, plen, crc);
    wr16(buf + 22, crc);
    return (size_t)ZD_HDR + plen;
}

/* ==================== 去重窗口 ==================== */
#define DEDUP_SRC 8
#define DEDUP_N   32

typedef struct {
    uint32_t src;
    uint16_t last[DEDUP_N];
    uint8_t  cnt;
    uint8_t  head;
} dedup_t;

static dedup_t s_dd[DEDUP_SRC];

/* 返回 1 = 首次见到（应处理），0 = 重复 */
static int dedup_check(uint32_t src, uint16_t seq)
{
    int slot = -1, empty = -1;
    for (int i = 0; i < DEDUP_SRC; i++) {
        if (s_dd[i].cnt && s_dd[i].src == src) { slot = i; break; }
        if (!s_dd[i].cnt && empty < 0) empty = i;
    }
    if (slot < 0) {
        slot = (empty >= 0) ? empty : 0;
        memset(&s_dd[slot], 0, sizeof(s_dd[slot]));
        s_dd[slot].src = src;
    }
    dedup_t *d = &s_dd[slot];
    for (int i = 0; i < d->cnt; i++)
        if (d->last[i] == seq) return 0;
    d->last[d->head] = seq;
    d->head = (d->head + 1) % DEDUP_N;
    if (d->cnt < DEDUP_N) d->cnt++;
    return 1;
}

/* ==================== 分片重组（收方向）==================== */
#define RA_MAX 512
typedef struct {
    uint32_t src;
    uint16_t msgid;
    uint8_t  frags;
    uint8_t  got;
    uint8_t  mask;
    uint16_t len;
    char     buf[RA_MAX];
} reasm_t;

static reasm_t s_ra;

/* 返回 1 表示收齐，*out_len 为总长 */
static int reasm_feed(uint32_t src, uint16_t msgid, uint8_t frag, uint8_t frags,
                      const uint8_t *pl, uint16_t plen, uint16_t *out_len)
{
    if (frags == 0 || frags > 8 || frag >= frags) return 0;
    if (s_ra.src != src || s_ra.msgid != msgid || s_ra.frags != frags) {
        memset(&s_ra, 0, sizeof(s_ra));
        s_ra.src = src;
        s_ra.msgid = msgid;
        s_ra.frags = frags;
    }
    int off = frag * ZD_MAX_PAYLOAD;
    if (off + plen > RA_MAX) return 0;
    memcpy(s_ra.buf + off, pl, plen);
    if (off + plen > s_ra.len) s_ra.len = (uint16_t)(off + plen);
    uint8_t bit = (uint8_t)(1u << frag);
    if (!(s_ra.mask & bit)) {
        s_ra.mask |= bit;
        s_ra.got++;
    }
    uint8_t all = (uint8_t)((1u << frags) - 1u);
    if (s_ra.got == frags && (s_ra.mask & all) == all) {
        *out_len = s_ra.len;
        return 1;
    }
    return 0;
}

/* ==================== 消息列表 ==================== */
#define MSG_MAX   24
#define MSG_LEN   96

enum { MS_PENDING = 0, MS_ACKED, MS_FAILED };

typedef struct {
    uint32_t src;
    uint8_t  out;
    uint8_t  hops;
    int8_t   rssi;
    uint32_t ts;
    uint8_t  st;          /* 仅发件：0=等 ACK 1=已确认 2=失败 */
    uint8_t  tries;
    uint8_t  src_type;    /* 0=ESP-NOW 1=LoRa(Meshtastic) */
    uint16_t seq;
    uint16_t msgid;
    int32_t  next_ms;
    char     text[MSG_LEN + 1];
} msg_t;

static msg_t s_msg[MSG_MAX];
static int   s_msg_head;     /* 下一条写入位置 */
static int   s_msg_cnt;
static int   s_scroll;       /* 向上滚动了几行 */

static msg_t *msg_add_ex(uint32_t src, int out, const char *text, int8_t rssi, uint8_t hops,
                         uint8_t src_type)
{
    msg_t *m = &s_msg[s_msg_head];
    memset(m, 0, sizeof(*m));
    m->src = src;
    m->out = (uint8_t)out;
    m->rssi = rssi;
    m->hops = hops;
    m->src_type = src_type;
    m->ts = (uint32_t)(esp_timer_get_time() / 1000000);
    strncpy(m->text, text, MSG_LEN);
    m->text[MSG_LEN] = 0;
    s_msg_head = (s_msg_head + 1) % MSG_MAX;
    if (s_msg_cnt < MSG_MAX) s_msg_cnt++;
    ESP_LOGI(TAG, "MSG %s %s%08lx: %s", out ? "OUT" : "IN ", src_type ? "L/" : "E/",
             (unsigned long)src, m->text);
    return m;
}

static msg_t *msg_add(uint32_t src, int out, const char *text, int8_t rssi, uint8_t hops)
{
    return msg_add_ex(src, out, text, rssi, hops, 0);
}

static msg_t *msg_at(int back)   /* back=0 最新 */
{
    if (back >= s_msg_cnt) return NULL;
    int idx = (s_msg_head - 1 - back + MSG_MAX * 2) % MSG_MAX;
    return &s_msg[idx];
}

/* ==================== 对端状态 ==================== */
static uint32_t s_peer_addr;         /* 最近一次收到非本机帧的 src id */
static uint32_t s_peer_seen_ms;
static uint8_t  s_peer_pct = 255;
static uint8_t  s_peer_role = 0;
static int      s_peer_rssi = 0;
static int      s_peer_hops = 0;

/* ==================== 电量 ==================== */
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static int s_batt_mv = 0;
static int s_batt_pct = 255;

static void batt_init(void)
{
    adc_oneshot_unit_init_cfg_t u = {0};
    u.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK) { s_adc = NULL; return; }
    adc_oneshot_chan_cfg_t c = {0};
    c.atten = ADC_ATTEN_DB_12;
    c.bitwidth = ADC_BITWIDTH_12;
    adc_oneshot_config_channel(s_adc, (adc_channel_t)PIN_BATT, &c);

    adc_cali_curve_fitting_config_t cal = {0};
    cal.unit_id = ADC_UNIT_1;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
}

/* 1S 锂电实测曲线：标定点取自 Meshtastic 同机读数（3290mV -> 9%） */
static int mv_to_pct(int mv)
{
    static const int tab_mv[]  = {3000, 3150, 3300, 3450, 3600, 3700, 3750, 3800, 3900, 4000, 4100, 4200};
    static const int tab_pct[] = {   0,    4,   10,   20,   35,   48,   58,   66,   80,   89,   96,  100};
    const int n = (int)(sizeof(tab_mv) / sizeof(tab_mv[0]));
    if (mv <= tab_mv[0]) return 0;
    if (mv >= tab_mv[n - 1]) return 100;
    for (int i = 1; i < n; i++) {
        if (mv <= tab_mv[i]) {
            int span = tab_mv[i] - tab_mv[i - 1];
            int dp = tab_pct[i] - tab_pct[i - 1];
            return tab_pct[i - 1] + (mv - tab_mv[i - 1]) * dp / span;
        }
    }
    return 100;
}

static void batt_poll(void)
{
    if (!s_adc) return;
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, (adc_channel_t)PIN_BATT, &raw) != ESP_OK) return;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return;
    /* 电池分压 1:2，采样点电压 x2 */
    int v = mv * 2;
    s_batt_mv = v;
    /* 没接电池 / 分压不匹配时读数会低到几百 mV，此时不显示百分比 */
    s_batt_pct = (v < 2500) ? 255 : (uint8_t)mv_to_pct(v);
}

/* ==================== 预设快回（没有键盘，只能发预设） ==================== */
static const char *kPreset[] = {
    "收到",
    "稍等一下",
    "在路上",
    "我到了",
    "测试 123",
    "OK",
};
#define PRESET_N ((int)(sizeof(kPreset) / sizeof(kPreset[0])))
static int s_preset_idx;

/* ==================== ESP-NOW ==================== */
static bool s_wifi_ok;
static uint32_t s_tx_ok, s_tx_err;
static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t st)
{
    (void)tx_info;
    if (st == ESP_NOW_SEND_SUCCESS) s_tx_ok++;
    else s_tx_err++;
}

/* 监视器（GMK）单播：GMKHI1 要直接回给主机，必须先注册对端 peer */
static void espnow_send_mac(const uint8_t mac[6], const uint8_t *data, int len)
{
    if (!s_wifi_ok) return;
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t p;
        memset(&p, 0, sizeof(p));
        memcpy(p.peer_addr, mac, 6);
        p.channel = 1;
        p.ifidx = WIFI_IF_STA;
        p.encrypt = false;
        if (esp_now_add_peer(&p) != ESP_OK) return;
    }
    esp_now_send(mac, data, (size_t)len);
}

static int now_ms(void) { return (int)(esp_timer_get_time() / 1000); }

/* ==================== LoRa 节点表（Meshtastic 侧） ==================== */
#define LORA_NODE_MAX 16

typedef struct {
    uint32_t id;
    char     sn[12];
    char     ln[24];
    int8_t   rssi;
    int8_t   snr4;        /* 四分之一 dB */
    uint8_t  hops;
    uint8_t  gps;         /* 0=未知 1=无 GPS（橙） 2=有 GPS（红） */
    uint32_t seen;        /* now_ms() */
} lnode_t;

static lnode_t s_lnode[LORA_NODE_MAX];
static int     s_lnode_cnt;
static int     s_nscroll;

static lnode_t *lnode_find(uint32_t id)
{
    for (int i = 0; i < s_lnode_cnt; i++)
        if (s_lnode[i].id == id) return &s_lnode[i];
    return NULL;
}

static lnode_t *lnode_slot(uint32_t id)
{
    lnode_t *n = lnode_find(id);
    if (n) return n;
    if (s_lnode_cnt < LORA_NODE_MAX) {
        n = &s_lnode[s_lnode_cnt++];
    } else {                       /* 表满：淘汰最久没动静的 */
        n = &s_lnode[0];
        for (int i = 1; i < LORA_NODE_MAX; i++)
            if ((int32_t)(s_lnode[i].seen - n->seen) < 0) n = &s_lnode[i];
    }
    memset(n, 0, sizeof(*n));
    n->id = id;
    return n;
}

static void ble_nodes_sync(void);

static void lnode_update(uint32_t id, const char *sn, const char *ln,
                         int8_t rssi, int8_t snr4, uint8_t hops, uint8_t gps)
{
    lnode_t *n = lnode_slot(id);
    if (sn && sn[0]) {
        strncpy(n->sn, sn, sizeof(n->sn) - 1);
        n->sn[sizeof(n->sn) - 1] = 0;
    }
    if (ln && ln[0]) {
        strncpy(n->ln, ln, sizeof(n->ln) - 1);
        n->ln[sizeof(n->ln) - 1] = 0;
    }
    n->rssi = rssi;
    n->snr4 = snr4;
    n->hops = hops;
    if (gps) n->gps = gps;        /* 新包没带能力位时保留已知状态 */
    n->seen = (uint32_t)now_ms();
    ble_nodes_sync();
}

/* 把 LoRa 节点表镜像给 BLE 层，用于 App 的节点同步 */
static void ble_nodes_sync(void)
{
    ble_node_t t[LORA_NODE_MAX];
    int n = 0;
    uint32_t now = (uint32_t)now_ms();
    for (int i = 0; i < LORA_NODE_MAX; i++) {
        if (!s_lnode[i].id) continue;
        memset(&t[n], 0, sizeof(t[n]));
        t[n].id = s_lnode[i].id;
        snprintf(t[n].long_name, sizeof(t[n].long_name), "%s", s_lnode[i].ln);
        snprintf(t[n].short_name, sizeof(t[n].short_name), "%s", s_lnode[i].sn);
        t[n].rssi = s_lnode[i].rssi;
        t[n].snr4 = s_lnode[i].snr4;
        t[n].hops = s_lnode[i].hops;
        uint32_t age = (now - s_lnode[i].seen) / 1000u;
        t[n].age_s = age < 86400u ? age : 86400u;
        n++;
    }
    ble_mesh_set_nodes(t, n);
}

static void lnode_label(char *out, size_t cap, uint32_t id)
{
    lnode_t *n = lnode_find(id);
    if (n && n->sn[0]) snprintf(out, cap, "%s", n->sn);
    else snprintf(out, cap, "%04lX", (unsigned long)(id & 0xFFFF));
}

/* ==================== v1.1 ZD <-> LoRa 桥 ==================== */
#define BR_TEXT_MAX  72
#define BR_PEND_N    4

typedef struct {
    uint8_t  used;
    uint16_t tag;
    uint16_t seq;
    uint16_t msgid;
} br_pend_t;

static br_pend_t s_brp[BR_PEND_N];
static uint16_t s_br_tag = 0x8000;

static uint16_t br_tag_next(void)
{
    s_br_tag++;
    if (s_br_tag == 0) s_br_tag = 0x8000;      /* tag 0 保留给非桥接发送 */
    return s_br_tag;
}

static void br_pend_add(uint16_t tag, uint16_t seq, uint16_t msgid)
{
    int slot = -1;
    for (int i = 0; i < BR_PEND_N; i++) {
        if (!s_brp[i].used) { slot = i; break; }
    }
    if (slot < 0) slot = 0;                    /* 覆盖最旧（LoRa 串行发送，极少发生） */
    s_brp[slot].used = 1;
    s_brp[slot].tag = tag;
    s_brp[slot].seq = seq;
    s_brp[slot].msgid = msgid;
}

static void br_on_txdone(uint16_t tag, int ok)
{
    if (tag == 0) return;
    for (int i = 0; i < BR_PEND_N; i++) {
        if (!s_brp[i].used || s_brp[i].tag != tag) continue;
        uint16_t seq = s_brp[i].seq, msgid = s_brp[i].msgid;
        s_brp[i].used = 0;
        if (!ok) {
            ESP_LOGW(TAG, "BRIDGE air timeout seq=%u msgid=%u -> LNACK(3)", seq, msgid);
            zd_lnack(seq, msgid, 3);
        } else {
            ESP_LOGI(TAG, "BRIDGE air done seq=%u msgid=%u -> ACK", seq, msgid);
            zd_ack_echo(seq, msgid);
        }
        return;
    }
}

/* ADV 文本 -> Meshtastic 广播文本；空口文本带 [A:xxxx] 来源前缀 */
static void bridge_lora_send(const uint8_t *body, uint16_t blen, uint32_t src,
                             uint16_t seq, uint16_t msgid, int want_ack)
{
    if (blen == 0) {
        if (want_ack) zd_lnack(seq, msgid, 4);
        return;
    }
    char air[BR_TEXT_MAX + 1];
    int pfx = snprintf(air, sizeof(air), "[A:%04x] ", (unsigned)(src & 0xFFFFu));
    if (pfx < 0 || pfx >= (int)sizeof(air)) pfx = 0;
    int avail = BR_TEXT_MAX - pfx;
    if (avail < 0) avail = 0;
    int c = (blen > (uint16_t)avail) ? avail : (int)blen;
    int trunc = (blen > (uint16_t)c);
    if (trunc) {
        c -= 2;
        if (c < 0) c = 0;
        while (c > 0 && ((uint8_t)body[c] & 0xC0) == 0x80) c--;
    }
    int len = pfx;
    for (int i = 0; i < c; i++) {
        uint8_t ch = body[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        air[len++] = (char)ch;
    }
    if (trunc && len + 2 <= (int)sizeof(air) - 1) { air[len++] = '.'; air[len++] = '.'; }
    air[len] = 0;

    uint16_t tag = br_tag_next();
    int rc = lora_send_text_tagged(0xFFFFFFFFu, air, tag);
    if (rc == 0) {
        if (want_ack) br_pend_add(tag, seq, msgid);
        ESP_LOGI(TAG, "BRIDGE adv=!%08lx seq=%u msgid=%u -> LoRa '%s'",
                 (unsigned long)src, seq, msgid, air);
    } else {
        uint8_t reason = (rc == -1) ? 1 : 2;
        ESP_LOGW(TAG, "BRIDGE rejected rc=%d reason=%u tx_allowed=%d", rc, reason, (int)lora_tx_allowed());
        if (want_ack) zd_lnack(seq, msgid, reason);
    }
}

/* ==================== 时间同步 ==================== */
/* 时间来源优先级：1=GPS（最可信） 2=手动 0=未同步。
 * 有 GPS 的那台是时间源；ADV 手动设的时间也能经 ZD 推进来，再广播到 LoRa 网。 */
static uint32_t s_utc;          /* 当前 UTC 秒，0=未同步 */
static uint8_t  s_tsrc;         /* 0/1/2 */
static uint32_t s_tset_local;   /* 校准时 now_ms() */
static uint32_t s_tlast_bcast;  /* 上次广播时刻 */

static uint32_t time_now_utc(void)
{
    if (!s_utc) return 0;
    return s_utc + ((uint32_t)now_ms() - s_tset_local) / 1000u;
}

static void time_apply(uint32_t utc, uint8_t src, const char *via)
{
    if (!utc || src == 0 || src > 2) return;
    /* GPS 可覆盖手动；同级只在未同步时采纳，避免来回跳 */
    int accept = (s_utc == 0) || (src == 1 && s_tsrc != 1) || (src == s_tsrc && s_tsrc == 0);
    if (!accept) return;
    uint32_t before = time_now_utc();
    int32_t drift = before ? (int32_t)(utc - before) : 0;
    s_utc = utc;
    s_tsrc = src;
    s_tset_local = (uint32_t)now_ms();
    ESP_LOGI(TAG, "TIME %s -> utc=%lu src=%u drift=%lds", via, (unsigned long)utc, src, (long)drift);
    s_tlast_bcast = 0;      /* 立刻转发给别人 */
}

static void time_broadcast(void)
{
    uint32_t utc = time_now_utc();
    if (!utc || s_tsrc == 0) return;
    uint8_t pl[6];
    wr32(pl, utc);
    pl[4] = s_tsrc;
    pl[5] = (s_tsrc == 1) ? 1 : 5;
    zd_send(ZT_TIME, pl, 6, 0, 0);
    if (lora_tx_allowed()) lora_send_time(utc, s_tsrc);
    s_tlast_bcast = (uint32_t)now_ms();
    ESP_LOGI(TAG, "TIME broadcast utc=%lu src=%u (ZD + LoRa)", (unsigned long)utc, s_tsrc);
}

/* 从 LoRa 任务队列取事件，只在主任务里改 UI 数据，避免跨任务踩结构 */
static void lora_drain(void)
{
    lora_evt_t e;
    while (lora_evt_pop(&e)) {
        if (e.kind == LORA_EVT_TXDONE) {
            br_on_txdone(e.pkt_lo, e.tx_ok);
        } else if (e.kind == LORA_EVT_TIME) {
            time_apply(e.utc, e.tsrc, "LoRa");
        } else if (e.kind == LORA_EVT_NODE) {
            lnode_update(e.from, e.short_name, e.long_name, e.rssi, e.snr4, e.hops, e.gps);
            ESP_LOGI(TAG, "NODE +%08lx '%s'/%s h=%u rssi=%d snr=%d.%d gps=%u", (unsigned long)e.from,
                     e.long_name, e.short_name, e.hops, e.rssi, e.snr4 / 4, (e.snr4 % 4) * 25 / 10,
                     e.gps);
        } else if (e.kind == LORA_EVT_TEXT) {
            lnode_update(e.from, NULL, NULL, e.rssi, e.snr4, e.hops, 0);
            msg_add_ex(e.from, 0, e.text, e.rssi, e.hops, 1);
            ble_mesh_on_lora_text(e.from, e.text, e.rssi, e.snr4, e.hops);

            /* v1.1: 回传 ADV（ZT_LTEXT，payload = lora_from + rssi + snr4 + hops + lflags + text） */
            uint8_t lp[8 + 96];
            size_t tl = strlen(e.text);
            if (tl > 96) tl = 96;
            wr32(lp, e.from);
            lp[4] = (uint8_t)(int8_t)e.rssi;
            lp[5] = (uint8_t)(int8_t)e.snr4;
            lp[6] = e.hops;
            lp[7] = 0;
            memcpy(lp + 8, e.text, tl);
            zd_send(ZT_LTEXT, lp, (uint16_t)(8 + tl), e.pkt_lo, 0);
            ESP_LOGI(TAG, "BRIDGE LoRa->ADV from=!%08lx rssi=%d snr=%d.%d hops=%u len=%u",
                     (unsigned long)e.from, e.rssi, e.snr4 / 4, (e.snr4 % 4) * 25 / 10, e.hops, (unsigned)tl);

            if (s_scroll > 0) s_scroll++;
        }
    }
}

static uint16_t zd_send_ex(uint8_t type, const uint8_t *pl, uint16_t plen,
                           uint16_t msgid, uint8_t flags, uint16_t seq)
{
    if (!s_wifi_ok) return seq;
    uint8_t buf[ZD_HDR + ZD_MAX_PAYLOAD];
    size_t n = frame_build(buf, type, 0xFFFFFFFFu, flags, seq, msgid, 0, 1, pl, plen);
    esp_err_t e = esp_now_send(kBroadcast, buf, n);
    if (e != ESP_OK) ESP_LOGW(TAG, "send type=%u fail %d", type, e);
    return seq;
}

static uint16_t zd_send(uint8_t type, const uint8_t *pl, uint16_t plen, uint16_t msgid, uint8_t flags)
{
    return zd_send_ex(type, pl, plen, msgid, flags, s_seq++);
}

static void zd_ack_echo(uint16_t seq, uint16_t msgid)
{
    uint8_t a[4];
    wr16(a, seq);
    wr16(a + 2, msgid);
    zd_send(ZT_ACK, a, 4, 0, 0);
}

static void zd_lnack(uint16_t seq, uint16_t msgid, uint8_t reason)
{
    uint8_t a[5];
    wr16(a, seq);
    wr16(a + 2, msgid);
    a[4] = reason;
    zd_send(ZT_LNACK, a, 5, 0, 0);
}

static void send_text(const char *text)
{
    size_t n = strlen(text);
    if (n == 0) return;
    if (n > ZD_MAX_PAYLOAD) n = ZD_MAX_PAYLOAD;
    uint16_t msgid = (uint16_t)(esp_random() & 0xFFFF);
    uint16_t seq = s_seq++;
    msg_t *m = msg_add(s_my_id, 1, text, 0, 0);
    m->st = MS_PENDING;
    m->tries = 1;
    m->msgid = msgid;
    m->seq = seq;
    m->next_ms = now_ms() + 1500;
    zd_send_ex(ZT_TEXT, (const uint8_t *)text, (uint16_t)n, msgid, ZF_WANT_ACK, seq);
    s_scroll = 0;                      /* 发送后回到最新 */
}

#if C3MSG_M2_TEST
/* M2 验收：同一 seq 连发两次（去重）、超长文本（折行/截断）、多条（滚动） */
static void m2_test_tick(int t)
{
    static const char *kLong =
        "长文本折行测试：这条消息故意写得很长，用来验证窄屏自动换行、截断与滚动是否正常，"
        "末尾再混一段 mixed English text 一起测。";
    static int t0 = -1, step;
    if (!s_wifi_ok) return;
    if (t0 < 0) { t0 = t; return; }
    int el = t - t0;
    if (step == 0 && el >= 8000) {
        step = 1;
        uint16_t sq = 0x4D32;
        zd_send_ex(ZT_TEXT, (const uint8_t *)"M2DUP1", 6, 0x1234, 0, sq);
        vTaskDelay(pdMS_TO_TICKS(250));
        zd_send_ex(ZT_TEXT, (const uint8_t *)"M2DUP2", 6, 0x1234, 0, sq);
        ESP_LOGI(TAG, "M2TEST dup pair sent seq=%u (对端只应收下 1 条)", sq);
    } else if (step == 1 && el >= 11000) {
        step = 2;
        zd_send_ex(ZT_TEXT, (const uint8_t *)kLong, (uint16_t)strlen(kLong), 0x2222, 0, s_seq++);
        ESP_LOGI(TAG, "M2TEST long %uB sent", (unsigned)strlen(kLong));
    } else if (step == 2 && el >= 13000) {
        step = 3;
        zd_send_ex(ZT_TEXT, (const uint8_t *)"M2-1", 4, 0x3331, 0, s_seq++);
    } else if (step == 3 && el >= 15000) {
        step = 4;
        zd_send_ex(ZT_TEXT, (const uint8_t *)"M2-2", 4, 0x3332, 0, s_seq++);
    } else if (step == 4 && el >= 17000) {
        step = 5;
        zd_send_ex(ZT_TEXT, (const uint8_t *)"M2-3", 4, 0x3333, 0, s_seq++);
        ESP_LOGI(TAG, "M2TEST done");
    }
}
#endif

/* 中继：ttl 减 1 后原样转发（去重窗口保证不会形成环） */
static uint32_t s_relay_cnt;
static uint32_t s_dup_cnt;
static uint32_t s_c3drop_cnt;      /* 被 ESP-NOW 策略挡掉的 C3↔C3 帧数 */

static void relay_frame(const uint8_t *data, int len)
{
    if (len < ZD_HDR || len > ZD_HDR + ZD_MAX_PAYLOAD) return;
    uint8_t buf[ZD_HDR + ZD_MAX_PAYLOAD];
    memcpy(buf, data, (size_t)len);
    uint8_t old_ttl = buf[12];
    if (old_ttl <= 1) return;                 /* ttl 用尽，不再转发 */
    buf[12] = (uint8_t)(old_ttl - 1);
    buf[13] |= ZF_FORWARDED;
    uint16_t plen = rd16(buf + 20);
    uint16_t crc = crc16_ccitt(buf, 22, 0xFFFF);
    crc = crc16_ccitt(buf + ZD_HDR, plen, crc);
    wr16(buf + 22, crc);
    if (esp_now_send(kBroadcast, buf, (size_t)len) == ESP_OK) {
        s_relay_cnt++;
        if (s_relay_cnt <= 8)          /* 前 8 次打印，用于观察 ttl 递减 */
            ESP_LOGI(TAG, "RLY from %08lx seq=%u ttl %u->%u",
                     (unsigned long)rd32(buf + 4), rd16(buf + 14), old_ttl, buf[12]);
    }
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    /* 只要收到任何 ESP-NOW 帧，就说明对端在、链路通（不论是不是我们的协议） */
    if (info) {
        const uint8_t *m = info->src_addr;
        s_peer_addr = ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | m[5];
        s_peer_seen_ms = (uint32_t)now_ms();
        s_peer_rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    }

    /* GMK 帧（五子棋主机 → 监视器）走 monitor 模块，与 ZD 帧互不干扰 */
    if (len >= 6 && data[0] == 'G' && data[1] == 'M') {
        monitor_on_frame(info->src_addr, data, len,
                         info->rx_ctrl ? (int)info->rx_ctrl->rssi : -120);
        mirror_on_frame(data, len);          /* 兜底转发（仅在监视器请求时发） */
        return;
    }

    if (len < ZD_HDR) {
        /* 不是 ZD 帧，只记来源 MAC */
        static int n_log;
        if (n_log < 16) {
            n_log++;
            ESP_LOGI(TAG, "rx short %d B from %02X:%02X:%02X:%02X:%02X:%02X",
                     len, info->src_addr[0], info->src_addr[1], info->src_addr[2],
                     info->src_addr[3], info->src_addr[4], info->src_addr[5]);
        }
        return;
    }
    if (data[0] != ZD_MAGIC0 || data[1] != ZD_MAGIC1) {
        static int n_log2;
        if (n_log2 < 16) {
            n_log2++;
            ESP_LOGI(TAG, "rx foreign %d B from %02X:%02X:%02X:%02X:%02X:%02X %02X%02X%02X%02X",
                     len, info->src_addr[0], info->src_addr[1], info->src_addr[2],
                     info->src_addr[3], info->src_addr[4], info->src_addr[5],
                     data[0], data[1], data[2], data[3]);
        }
        return;
    }
    if (data[2] != ZD_VER) return;

    uint16_t plen = rd16(data + 20);
    if ((int)plen + ZD_HDR != len) return;
    uint16_t crc = crc16_ccitt(data, 22, 0xFFFF);
    crc = crc16_ccitt(data + ZD_HDR, plen, crc);
    if (crc != rd16(data + 22)) {
        ESP_LOGW(TAG, "crc bad from %02X:%02X", info->src_addr[4], info->src_addr[5]);
        return;
    }

    uint32_t src = rd32(data + 4);
    if (src == s_my_id) return;                 /* 自己的广播回环 */

    /* v1.2：ESP-NOW 只服务 Cardputer ADV ↔ 就近 C3。
     * C3 系 NodeNum 的高字节固定为 0x7D（由 50:78:7D:xx:xx:xx MAC 推导），
     * 这类来源一律丢弃，设备之间只走 LoRa 路由。ADV(0xACxxxxxx) 不受影响。 */
    if ((src >> 24) == 0x7D) {
        s_c3drop_cnt++;
        if (s_c3drop_cnt <= 5)
            ESP_LOGW(TAG, "ESP-NOW C3->C3 blocked src=%08lx (LoRa only)", (unsigned long)src);
        return;
    }

    uint32_t dst = rd32(data + 8);
    int for_me = (dst == 0xFFFFFFFFu || dst == s_my_id);

    /* 先去重（中继过的帧也会进窗口），重复帧一律丢，避免多跳成环 */
    if (!dedup_check(src, rd16(data + 14))) {
        s_dup_cnt++;
        if (s_dup_cnt <= 8)            /* 前 8 次打印，用于实测去重窗口 */
            ESP_LOGW(TAG, "DUP src=%08lx seq=%u ttl=%u fwd=%u",
                     (unsigned long)src, rd16(data + 14), data[12],
                     (data[13] & ZF_FORWARDED) ? 1u : 0u);
        return;
    }

    uint8_t type = data[3];

    if (!for_me) {
        /* 不是给我的单播：只做中继（M2 多跳） */
        relay_frame(data, len);
        return;
    }
    if (dst == 0xFFFFFFFFu) relay_frame(data, len);   /* 广播帧也按 TTL 继续扩散 */

    uint16_t seq = rd16(data + 14);

    int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    s_peer_addr = src;
    s_peer_seen_ms = (uint32_t)now_ms();
    s_peer_rssi = rssi;
    s_peer_hops = (data[12] < ZD_TTL) ? (ZD_TTL - data[12]) : 0;   /* 经过几跳 */

    const uint8_t *pl = data + ZD_HDR;

    switch (type) {
    case ZT_TEXT: {
        const uint8_t *body = pl;
        uint16_t blen = plen;
        if (data[19] > 1) {                       /* frags>1：先重组 */
            if (!reasm_feed(src, rd16(data + 16), data[18], data[19], pl, plen, &blen)) break;
            body = (const uint8_t *)s_ra.buf;
        }
        char tmp[MSG_LEN + 1];
        uint16_t c = blen > MSG_LEN ? MSG_LEN : blen;
        if (blen > MSG_LEN) {                     /* 超长截断：退回完整 UTF-8 字符并加 ".." */
            c = MSG_LEN - 2;
            while (c > 0 && ((uint8_t)body[c] & 0xC0) == 0x80) c--;
        }
        if (c) memcpy(tmp, body, c);
        tmp[c] = 0;
        if (blen > MSG_LEN) strcat(tmp, "..");
        for (uint16_t i = 0; i < c; i++)
            if (tmp[i] == '\n' || tmp[i] == '\r' || tmp[i] == '\t') tmp[i] = ' ';
        msg_add(src, 0, tmp, rssi, 0);
        ESP_LOGI(TAG, "TEXT %08lx len=%u rssi=%d hops=%u", (unsigned long)src, blen,
                 rssi, s_peer_hops);
        if (s_scroll > 0) s_scroll++;             /* 正在看历史时不要把画面顶走 */
        int want_ack = (data[13] & ZF_WANT_ACK) ? 1 : 0;
        if (data[13] & ZF_LORA) {
            if (dst != s_my_id) {
                /* v1.1 规定 ZF_LORA 必须单播给桥；广播请求会被多台 C3 重复上空气 */
                ESP_LOGW(TAG, "BRIDGE ignored: ZF_LORA requires unicast, dst=%08lx", (unsigned long)dst);
                if (want_ack) zd_lnack(seq, rd16(data + 16), 4);
            } else {
                bridge_lora_send(body, blen, src, seq, rd16(data + 16), want_ack);
            }
        } else if (want_ack) {
            zd_ack_echo(seq, rd16(data + 16));
        }
        break;
    }
    case ZT_BEAT: {
        if (plen >= 4) {
            s_peer_pct = pl[2];
            s_peer_role = pl[3];
            ESP_LOGI(TAG, "BEAT %08lx rssi=%d mv=%u pct=%u role=%u",
                     (unsigned long)src, rssi, (unsigned)(pl[0] | (pl[1] << 8)), pl[2], pl[3]);
        }
        break;
    }
    case ZT_ACK: {
        uint16_t aseq = plen >= 2 ? rd16(pl) : 0;
        uint16_t amid = plen >= 4 ? rd16(pl + 2) : 0;
        int hit = 0;
        for (int i = 0; i < s_msg_cnt; i++) {
            msg_t *m = msg_at(i);
            if (m && m->out && m->st == MS_PENDING && m->seq == aseq && m->msgid == amid) {
                m->st = MS_ACKED;
                hit = 1;
                break;
            }
        }
        ESP_LOGI(TAG, "ACK from %08lx seq=%u msgid=%u%s", (unsigned long)src, aseq, amid, hit ? "" : " (stale)");
        break;
    }
    case ZT_NODETAB: {
        int n = plen / 6;
        for (int i = 0; i < n; i++)
            ESP_LOGI(TAG, "NODE %08lx rssi=%d hops=%u",
                     (unsigned long)rd32(pl + i * 6), (int8_t)pl[i * 6 + 4], pl[i * 6 + 5]);
        break;
    }
    case ZT_TIME: {
        if (plen >= 5) {
            uint32_t utc = rd32(pl);
            time_apply(utc, pl[4], "ZD");
        }
        break;
    }
    default:
        break;
    }
}

static void wifi_now_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 绝不擦除：这是 Meshtastic 的配置分区 */
        ESP_LOGW(TAG, "nvs init: %s (不擦除)", esp_err_to_name(e));
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
#ifndef C3MSG_TX_POWER
/* 0.25dBm 单位。20dBm 在两块板贴近放置时会让对端接收饱和（实测 A->B 丢 87%），
 * 10dBm 实测 100% 送达且仍有几十米量级余量，故选 40。 */
#define C3MSG_TX_POWER 40
#endif
    esp_wifi_set_max_tx_power(C3MSG_TX_POWER);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, kBroadcast, 6);
    peer.channel = 1;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t pe = esp_now_add_peer(&peer);
    if (pe != ESP_OK) ESP_LOGW(TAG, "add broadcast peer: %s", esp_err_to_name(pe));

    s_wifi_ok = true;

    uint8_t ch = 0;
    wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
    ESP_LOGI(TAG, "ESP-NOW ready, channel=%u, my_id=%08lx", ch, (unsigned long)s_my_id);
}

/* ==================== 界面 ==================== */
/* 布局按屏高选：小屏 160x80 紧凑 4 行；大屏 320x240 行距加大、行数按高度算 */
typedef struct {
    int top_h, list_y, list_line, list_n, comp_y;
} ui_layout_t;
static ui_layout_t L;

static void ui_layout_init(void)
{
    if (SCR_H >= 200) {                     /* ST7789 320x240 */
        L.top_h = 18;
        L.list_y = 20;
        L.list_line = 18;
        L.comp_y = SCR_H - 28;
        L.list_n = (L.comp_y - 1 - L.list_y) / L.list_line;
        if (L.list_n > 12) L.list_n = 12;
    } else {                                /* ST7735S 160x80 */
        L.top_h = 12;
        L.list_y = 13;
        L.list_line = 13;
        L.list_n = 4;
        L.comp_y = 66;
    }
    printf("ui layout: %dx%d top=%d list_y=%d line=%d rows=%d comp_y=%d\n",
           SCR_W, SCR_H, L.top_h, L.list_y, L.list_line, L.list_n, L.comp_y);
}

static void peer_label(char *out, size_t n, uint32_t id)
{
    if (id == 0) { snprintf(out, n, "---"); return; }
    switch (id) {
    case 0xACA5AF00u: snprintf(out, n, "CPT");  return;   /* Cardputer STA */
    case 0x7DF43F68u: snprintf(out, n, "3F68"); return;
    case 0x7DF331D0u: snprintf(out, n, "31D0"); return;
    default: break;
    }
    if ((id & 0xFFFFu) == 0xAF00u) { snprintf(out, n, "CPT"); return; }  /* 低 16 位兜底 */
    snprintf(out, n, "%04lX", (unsigned long)(id & 0xFFFF));
}

/* 信号强度 5 格 */
static void draw_bars(int x, int y, int rssi)
{
    int lv = 0;
    if (rssi > -60) lv = 5;
    else if (rssi > -68) lv = 4;
    else if (rssi > -76) lv = 3;
    else if (rssi > -84) lv = 2;
    else if (rssi > -92) lv = 1;
    for (int i = 0; i < 5; i++) {
        int h = 2 + i * 1;          /* 2..6 像素高 */
        int bx = x + i * 3;
        for (int j = 0; j < h; j++) {
            put_px(bx, y + 6 - j, i < lv ? C_IN : C_LINE);
            put_px(bx + 1, y + 6 - j, i < lv ? C_IN : C_LINE);
        }
    }
}

/* ---- 以行为单位排版（长消息按宽度折行，便于滚动）---- */
#define VROW_MAX   40
#define VROW_TEXT  40      /* 12 个汉字 x3B + 省略号，够一行用 */

typedef struct {
    int  back;              /* msg_at(back) 的下标 */
    uint8_t seg;            /* 第几段（0 = 首段，带发件人） */
    char txt[VROW_TEXT];
} vrow_t;

static vrow_t s_rows[VROW_MAX];
static int    s_row_cnt;
static int    s_mode;       /* 0=看消息 1=选预设 2=LoRa 节点列表 3=监视器 4=设置 */
static int    s_cfg_item;   /* 设置页当前项 0=档位 1=功率 2=时间 */

/* 不能出现在行首的收尾标点（，。、：；！？）》”’…—·） */
static int no_line_start(uint32_t cp)
{
    switch (cp) {
    case 0xFF0C: case 0x3002: case 0x3001: case 0xFF1A: case 0xFF1B:
    case 0xFF01: case 0xFF1F: case 0xFF09: case 0x300B: case 0x3011:
    case 0x300D: case 0x201D: case 0x2019: case 0x2026: case 0x2014:
    case 0x00B7:
        return 1;
    default:
        return 0;
    }
}

static void build_rows(void)
{
    s_row_cnt = 0;
    int n = s_msg_cnt > 12 ? 12 : s_msg_cnt;
    for (int k = n - 1; k >= 0; k--) {           /* 旧 -> 新 */
        msg_t *m = msg_at(k);
        if (!m) continue;
        const char *s = m->text;
        int seg = 0;
        while (*s && seg < 4 && s_row_cnt < VROW_MAX) {
            int avail = (seg == 0) ? (SCR_W - 6 - 42) : (SCR_W - 16);
            int used = 0;
            const char *p = s;
            while (*p) {
                const char *q = p;
                uint32_t cp = utf8_next(&q);
                int gw = (cp < 0x80) ? ASCII12_W : 12;
                if (used + gw > avail) break;
                used += gw;
                p = q;
            }
            /* 避头尾：如果断点正好落在收尾标点前，把标点拉到本行（允许溢出到右边距） */
            if (p > s && *p) {
                const char *q2 = p;
                uint32_t cp2 = utf8_next(&q2);
                if (no_line_start(cp2)) p = q2;
            }
            int len = (int)(p - s);
            if (len <= 0) break;
            if (len > VROW_TEXT - 1) len = VROW_TEXT - 1;
            vrow_t *r = &s_rows[s_row_cnt++];
            r->back = k;
            r->seg = (uint8_t)seg;
            memcpy(r->txt, s, len);
            r->txt[len] = 0;
            s += len;
            seg++;
        }
        if (*s && s_row_cnt > 0 && seg == 4) {      /* 还有内容但行数用尽：末行补 ".." */
            vrow_t *r = &s_rows[s_row_cnt - 1];
            int avail2 = (r->seg == 0) ? (SCR_W - 6 - 42) : (SCR_W - 16);
            if (text_w(r->txt) + 2 * ASCII12_W <= avail2) strcat(r->txt, "..");
        }
    }
}

static void ui_draw(void)
{
    fill_rect(0, 0, SCR_W, SCR_H, C_BG);

    char buf[32];
    int peer_live = (s_peer_addr && (uint32_t)now_ms() - s_peer_seen_ms < 15000);

    /* ---- 顶栏：本机 ID / 信号 / 对端 / 电量 ---- */
    int ty = (L.top_h - 12) / 2;
    if (ty < 0) ty = 0;
    fill_rect(0, 0, SCR_W, L.top_h, C_BAR);
    snprintf(buf, sizeof(buf), "%04lX", (unsigned long)(s_my_id & 0xFFFF));
    draw_text(2, ty, buf, C_DIM, -1);

    draw_bars(32, ty + 2, peer_live ? s_peer_rssi : -120);
    if (peer_live) snprintf(buf, sizeof(buf), "%d", s_peer_rssi);
    else snprintf(buf, sizeof(buf), "--");
    draw_text(50, ty, buf, peer_live ? C_FG : C_DIM, 24);

    char pl[16];
    peer_label(pl, sizeof(pl), peer_live ? s_peer_addr : 0);
    draw_text(78, ty, pl, peer_live ? C_IN : C_DIM, 46);

    if (SCR_W >= 300) {          /* 大屏顶栏空出的中段放一行状态 */
        snprintf(buf, sizeof(buf), "LoRa 433.000");
        draw_text(150, ty, buf, C_DIM, 110);
    }

    int pct = s_batt_pct;
    if (pct == 255) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d%%", pct);
    draw_text_r(SCR_W - 16, ty, buf, (pct != 255 && pct < 20) ? C_ERR : C_FG);

    int bx = SCR_W - 13, by = (L.top_h - 6) / 2;
    for (int i = 0; i < 11; i++) {
        put_px(bx + i, by, C_DIM);
        put_px(bx + i, by + 5, C_DIM);
    }
    for (int j = 0; j < 6; j++) {
        put_px(bx, by + j, C_DIM);
        put_px(bx + 10, by + j, C_DIM);
    }
    for (int j = 0; j < 3; j++) put_px(bx + 11, by + 2 + j, C_DIM);
    if (pct != 255) {
        int w = (pct * 9) / 100;
        if (w < 1) w = 1;
        fill_rect(bx + 1, by + 1, w, 4, pct < 20 ? C_ERR : C_IN);
    }
    hline(0, L.top_h, SCR_W, C_LINE);

    /* ---- 消息区 / LoRa 节点区 / 设置区 ---- */
    if (s_mode == 4) {
        /* 设置页：档位 / 功率 / 时间。全部运行期可改，不写 NVS（避免动 Meshtastic 配置） */
        char v0[28], v1[28], v2[32];
        snprintf(v0, sizeof(v0), "%s %dK", lora_preset_name(),
                 lora_preset_idx() == 0 ? 250 : 125);
        if (!lora_tx_allowed()) snprintf(v1, sizeof(v1), "只收不发(无天线)");
        else snprintf(v1, sizeof(v1), "%ddBm", lora_tx_power_dbm());
        uint32_t utc = time_now_utc();
        if (!utc) snprintf(v2, sizeof(v2), "未同步");
        else snprintf(v2, sizeof(v2), "%02lu:%02lu %s",
                      (unsigned long)((utc / 3600) % 24), (unsigned long)((utc / 60) % 60),
                      s_tsrc == 1 ? "GPS" : "手动");
        const char *names[3] = {"档位", "功率", "时间"};
        const char *vals[3] = {v0, v1, v2};
        for (int i = 0; i < 3; i++) {
            int y = L.list_y + i * L.list_line;
            if (i == s_cfg_item) fill_rect(0, y - 1, SCR_W, L.list_line, C_BAR);
            draw_text(4, y, names[i], i == s_cfg_item ? C_WARN : C_DIM, 34);
            draw_text(34, y, vals[i], i == s_cfg_item ? C_FG : C_DIM, SCR_W - 38);
        }
    } else if (s_mode == 2) {
        /* ---- LoRa 节点列表：按最后活跃时间倒序，4 行，短按循环 ---- */
        int order[LORA_NODE_MAX];
        int n = s_lnode_cnt;
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = 1; i < n; i++) {           /* 新的在前 */
            int t = order[i], j = i - 1;
            while (j >= 0 && (int32_t)(s_lnode[order[j]].seen - s_lnode[t].seen) < 0) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = t;
        }
        if (s_nscroll < 0) s_nscroll = 0;
        if (n > 0 && s_nscroll >= n) s_nscroll = 0;

        /* 列位置按屏宽等比：小屏同原值（44/60/90/120） */
        int cx_h = SCR_W * 28 / 100, cx_rssi = SCR_W * 37 / 100;
        int cx_snr = SCR_W * 56 / 100, cx_age = SCR_W * 75 / 100;
        for (int i = 0; i < L.list_n && i < n; i++) {
            lnode_t *nd = &s_lnode[order[(s_nscroll + i) % n]];
            int y = L.list_y + i * L.list_line;
            char nb[12], hb[8], rb[8], sb[10], ab[8];
            snprintf(nb, sizeof(nb), "%s", nd->sn[0] ? nd->sn : "----");
            snprintf(hb, sizeof(hb), "h%u", nd->hops);
            snprintf(rb, sizeof(rb), "%d", nd->rssi);
            snprintf(sb, sizeof(sb), "%d.%d", nd->snr4 / 4, (nd->snr4 % 4) * 25 / 10);
            uint32_t age = (uint32_t)now_ms() - nd->seen;
            if (age < 60000) snprintf(ab, sizeof(ab), "%lus", (unsigned long)(age / 1000));
            else if (age < 3600000) snprintf(ab, sizeof(ab), "%lum", (unsigned long)(age / 60000));
            else snprintf(ab, sizeof(ab), "%luh", (unsigned long)(age / 3600000));

            /* 设备类型一眼分辨：有 GPS=红，无 GPS=橙，未知=灰 */
            color_t nc = nd->gps == 2 ? C_ERR : (nd->gps == 1 ? C_ORANGE : C_DIM);
            draw_text(2, y, nb, nc, cx_h - 4);
            draw_text(cx_h, y, hb, C_DIM, 14);
            draw_text(cx_rssi, y, rb, nd->rssi < -110 ? C_ERR : C_FG, 28);
            draw_text(cx_snr, y, sb, C_DIM, cx_age - cx_snr - 4);
            draw_text(cx_age, y, ab, C_DIM, 38);
        }
        if (n == 0) {
            draw_text(8, L.list_y + 3, "暂无 LoRa 节点", C_DIM, -1);
            draw_text(8, L.list_y + 3 + L.list_line, "收到官方广播后自动出现", C_DIM, -1);
            draw_text(8, L.list_y + 3 + L.list_line * 2,
                      monitor_supported() ? "超长按 切 监视器页" : "超长按 2.5s 返回", C_DIM, -1);
        }
    } else {
        /* ---- 消息区：最新在最下面，可向上滚动 ---- */
        build_rows();
        int maxs = s_row_cnt - L.list_n;
        if (maxs < 0) maxs = 0;
        if (s_scroll > maxs) s_scroll = maxs;
        if (s_scroll < 0) s_scroll = 0;
        int start = s_row_cnt - L.list_n - s_scroll;

        for (int i = 0; i < L.list_n; i++) {
            int ri = start + i;
            int y = L.list_y + i * L.list_line;
            if (ri < 0 || ri >= s_row_cnt) continue;
            vrow_t *r = &s_rows[ri];
            msg_t *m = msg_at(r->back);
            if (!m) continue;

            if (r->seg == 0) {
                if (m->out) {
                    uint16_t pc = C_WARN;
                    if (m->st == MS_ACKED) pc = C_OUT;
                    else if (m->st == MS_FAILED) pc = C_ERR;
                    draw_text(2, y, ">", pc, -1);
                    draw_text(2 + ASCII12_W + 2, y, r->txt, pc, SCR_W - 6 - ASCII12_W - 2);
                    if (m->st == MS_FAILED) draw_text(SCR_W - 8, y, "!", C_ERR, -1);
                } else {
                    /* 来源标记：L = LoRa(Meshtastic)，E = ESP-NOW */
                    char tag[16];
                    if (m->src_type) lnode_label(tag, sizeof(tag), m->src);
                    else peer_label(tag, sizeof(tag), m->src);
                    uint16_t tc = m->src_type ? C_OUT : C_IN;
                    draw_text(2, y, m->src_type ? "L" : "E", tc, -1);
                    draw_text(2 + ASCII12_W + 2, y, tag, tc, -1);
                    int tagw = (ASCII12_W + 2) + text_w(tag) + 4;
                    draw_text(2 + tagw, y, r->txt, C_FG, SCR_W - 4 - tagw);
                }
            } else {
                draw_text(14, y, r->txt, m->out ? C_WARN : C_FG, SCR_W - 14);
            }
        }

        if (s_msg_cnt == 0) {
            draw_text(8, L.list_y + 3, "暂无消息", C_DIM, -1);
            draw_text(8, L.list_y + 3 + L.list_line, "长按 BOOT 进入发送", C_DIM, -1);
            draw_text(8, L.list_y + 3 + L.list_line * 2, "超长按 看 LoRa 节点", C_DIM, -1);
            if (monitor_supported())
                draw_text(8, L.list_y + 3 + L.list_line * 3, "再看 五子棋监视器", C_DIM, -1);
        }

        /* 正在看历史时左侧留一条竖线，并在底部显示上翻行数 */
        if (s_scroll > 0) {
            for (int y = L.list_y; y < L.comp_y - 1; y++) put_px(0, y, C_WARN);
        }
    }

    hline(0, L.comp_y - 1, SCR_W, C_LINE);

    /* ---- 底部状态/输入条 ---- */
    fill_rect(0, L.comp_y, SCR_W, SCR_H - L.comp_y, C_BAR);
    int cy = L.comp_y + (SCR_H - L.comp_y - 12) / 2;
    if (cy < L.comp_y) cy = L.comp_y;
    if (s_mode == 0) {
        if (s_scroll > 0) {
            snprintf(buf, sizeof(buf), "^%d", s_scroll);
            draw_text(4, cy, buf, C_WARN, -1);
        } else {
            draw_text(4, cy, "长按 写消息", C_DIM, -1);
        }
        if (s_lnode_cnt > 0) {
            snprintf(buf, sizeof(buf), "L%d", s_lnode_cnt);
            draw_text(SCR_W / 2, cy, buf, C_OUT, 30);
        }
        if (s_batt_mv > 2500) {
            snprintf(buf, sizeof(buf), "%d.%02dV", s_batt_mv / 1000, (s_batt_mv % 1000) / 10);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        draw_text_r(SCR_W - 4, cy, buf, C_DIM);
    } else if (s_mode == 2) {
        snprintf(buf, sizeof(buf), "LoRa %d/%d", s_lnode_cnt, LORA_NODE_MAX);
        draw_text(4, cy, buf, C_OUT, 76);
        draw_text_r(SCR_W - 4, cy, "长按返回", C_DIM);
    } else if (s_mode == 4) {
        draw_text(4, cy, "短按切项 长按改值", C_DIM, SCR_W - 8);
    } else {
        /* 选预设：▶ 预设文本  n/N */
        for (int j = 0; j < 7; j++)
            for (int i = 0; i <= j; i++) put_px(4 + i, cy + 3 + j - i, C_WARN);
        draw_text(14, cy, kPreset[s_preset_idx], C_FG, SCR_W - 52);
        snprintf(buf, sizeof(buf), "%d/%d", s_preset_idx + 1, PRESET_N);
        draw_text_r(SCR_W - 4, cy, buf, C_DIM);
    }
}

static void ui_flush(void)
{
    if (s_mode == 3) {
        monitor_draw();          /* 五子棋监视器页 */
    } else {
        ui_draw();
    }
    display_flush();
}

/* 调试：帧缓冲按调色板 RLE 打到串口（逻辑坐标 + 面板实际 565，见 display.c） */
#define UI_DUMP_AT_BOOT 0   /* 验证帧缓冲时置 1，产线 0 */
#define LORA_UI_SELFTEST 0  /* 产线版：关闭节点页帧缓冲自检 */
static void ui_dump(const char *tag)
{
#if UI_DUMP_AT_BOOT
    display_dump(tag);
#else
    (void)tag;
#endif
}

/* ==================== 按键 ==================== */
#define BTN_TICK_MS   10
#define BTN_LONG_MS   800
#define BTN_PAGE_MS   2500

static void btn_poll(int *held_ms, int *was_down)
{
    int down = (gpio_get_level(PIN_BTN) == 0);
    if (down && !*was_down) {
        *held_ms = 0;
        *was_down = 1;
    } else if (down && *was_down) {
        if (*held_ms < 10000) *held_ms += BTN_TICK_MS;
    } else if (!down && *was_down) {
        *was_down = 0;
        if (s_mode == 1) {
            if (*held_ms >= BTN_LONG_MS) {
                send_text(kPreset[s_preset_idx]);
                s_mode = 0;                  /* 发出后回到看消息 */
                ESP_LOGI(TAG, "BTN long -> send '%s'", kPreset[s_preset_idx]);
            } else {
                s_preset_idx = (s_preset_idx + 1) % PRESET_N;
                ESP_LOGI(TAG, "BTN short -> preset %d/%d", s_preset_idx + 1, PRESET_N);
            }
        } else if (*held_ms >= BTN_PAGE_MS) {
            /* 超长按 2.5s：消息页 → LoRa 节点页 → 监视器页 → 设置页 → 消息页 */
            if (s_mode == 0 || s_mode == 1) s_mode = 2;
            else if (s_mode == 2) s_mode = monitor_supported() ? 3 : 4;
            else if (s_mode == 3) s_mode = 4;
            else s_mode = 0;
            s_nscroll = 0;
            ESP_LOGI(TAG, "BTN verylong -> page %d (lora nodes=%d)", s_mode, s_lnode_cnt);
        } else if (*held_ms >= BTN_LONG_MS) {
            if (s_mode == 4) {
                /* 设置页：长按改当前项的值 */
                if (s_cfg_item == 0) {
                    lora_set_preset((lora_preset_idx() + 1) % lora_preset_count());
                    ESP_LOGI(TAG, "BTN long -> preset %s", lora_preset_name());
                } else if (s_cfg_item == 1) {
                    int dbm = lora_tx_power_dbm();
                    dbm = (dbm < 17) ? 17 : (dbm < 20 ? 20 : 14);
                    lora_set_tx_power(dbm);
                    ESP_LOGI(TAG, "BTN long -> tx %d dBm", lora_tx_power_dbm());
                } else {
                    ESP_LOGI(TAG, "BTN long -> time is driven by GPS/ADV, nothing to toggle");
                }
            } else if (s_mode == 2 || s_mode == 3) {
                s_mode = 0;
                ESP_LOGI(TAG, "BTN long -> back to messages");
            } else {
                s_mode = 1;                  /* 看消息 -> 选预设 */
                ESP_LOGI(TAG, "BTN long -> compose");
            }
        } else {
            if (s_mode == 0) {
                /* 向上翻一行；翻到顶再按一次回到最新 */
                build_rows();
                int maxs = s_row_cnt - L.list_n;
                if (maxs < 0) maxs = 0;
                s_scroll = (s_scroll >= maxs) ? 0 : s_scroll + 1;
                ESP_LOGI(TAG, "BTN short -> scroll %d/%d", s_scroll, maxs);
            } else if (s_mode == 2) {
                int n = s_lnode_cnt > 0 ? s_lnode_cnt : 1;
                s_nscroll = (s_nscroll + 1) % n;
                ESP_LOGI(TAG, "BTN short -> node scroll %d/%d", s_nscroll, n);
            } else if (s_mode == 3) {
                /* 监视器页：短按无操作，画面跟随主机 */
                ESP_LOGI(TAG, "BTN short -> monitor page (no-op)");
            } else if (s_mode == 4) {
                s_cfg_item = (s_cfg_item + 1) % 3;
                ESP_LOGI(TAG, "BTN short -> cfg item %d", s_cfg_item);
            } else {
                s_preset_idx = (s_preset_idx + 1) % PRESET_N;
                ESP_LOGI(TAG, "BTN short -> preset %d/%d", s_preset_idx + 1, PRESET_N);
            }
        }
        *held_ms = 0;
    }
}

/* ==================== 启动 ==================== */
static void gpio_init_all(void)
{
    /* LoRa 保持空闲（CS/RST 高、RXEN 低），避免和屏幕抢 SPI */
    gpio_config_t io;
    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = (1ULL << PIN_LORA_CS) | (1ULL << PIN_LORA_RST) | (1ULL << PIN_LORA_RXEN);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(PIN_LORA_CS, 1);
    gpio_set_level(PIN_LORA_RST, 1);
    gpio_set_level(PIN_LORA_RXEN, 0);

    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = (1ULL << PIN_LORA_BUSY) | (1ULL << PIN_LORA_DIO1);
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);

    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_DC);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_LCD_DC, 1);

    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = (1ULL << PIN_BTN);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = 1;
    gpio_config(&io);
}

/* 监视器页自检：伪造一帧 GMKSTATE，验证解析 + 棋盘渲染 + 页面切换 */
#define MON_SELFTEST 0     /* 验证时置 1，产线 0 */
#if MON_SELFTEST
static void mon_selftest(void)
{
    uint8_t st[240];
    memset(st, 0, sizeof(st));
    st[0] = 'G'; st[1] = 'M'; st[2] = 0x20;
    st[4] = 0x00; st[5] = 0x00;   /* 非 PVP */
    st[6] = 0; st[7] = 1;         /* 轮黑 */
    st[8] = 3;                    /* Lv.3 */
    st[11] = 7; st[12] = 7;       /* 光标 */
    st[13] = 12;                  /* 手数 */
    st[14] = 7; st[15] = 6;       /* 最后一手 G8 */
    st[18] = 12; st[19] = 8;      /* 我方 12 / AI 8 */
    st[20] = 2; st[21] = 5;
    st[24] = 2; st[25] = 1;
    st[33] = 125;                 /* 2:05 */
    static const struct { uint8_t r, c, v; } kStones[] = {
        {7, 6, 1}, {8, 8, 2}, {7, 7, 1}, {9, 9, 2}, {6, 8, 1},
        {8, 6, 2}, {10, 10, 1}, {5, 5, 2}, {7, 8, 1}, {8, 7, 2},
    };
    for (int i = 0; i < (int)(sizeof(kStones) / sizeof(kStones[0])); i++) {
        int idx = kStones[i].r * 15 + kStones[i].c;
        st[181 + idx / 4] |= (uint8_t)(kStones[i].v << ((idx % 4) * 2));
    }
    uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    monitor_on_frame(mac, st, 240, -52);

    int keep = s_mode;
    s_mode = 3;
    ui_flush();
    display_dump("mon-page");
    s_mode = keep;
    ui_flush();
    display_dump("msg-page");         /* 验证能切回 LoRa 消息页 */
    monitor_init(espnow_send_mac);   /* 清掉伪造状态，等真实 GMK 包 */
}
#endif

/* 屏参自检：四角色块 + 方位文字，dump 出来核对方向/颜色/无镜像 */
#define PANEL_SELFTEST 0   /* 屏参自检：验证时置 1，产线 0 */
#if PANEL_SELFTEST
static void panel_selftest(void)
{
    const int w = SCR_W, h = SCR_H;
    fill_rect(0, 0, w / 2, h / 2, C_ERR);                     /* TL 红 */
    fill_rect(w / 2, 0, w - w / 2, h / 2, C_IN);              /* TR 绿 */
    fill_rect(0, h / 2, w / 2, h - h / 2, C_OUT);             /* BL 蓝 */
    fill_rect(w / 2, h / 2, w - w / 2, h - h / 2, C_WARN);    /* BR 黄 */

    draw_text(6, 6, "TL", C_BG, -1);
    draw_text_r(w - 6, 6, "TR", C_BG);
    draw_text(6, h - 20, "BL", C_BG, -1);
    draw_text_r(w - 6, h - 20, "BR", C_BG);
    draw_text(w / 2 - 10, h / 2 - 8, "UP", C_BG, -1);

    display_flush();
    display_dump("selftest");
    vTaskDelay(pdMS_TO_TICKS(1500));
}
#endif

/* 送屏最小验证：只画全红/全绿/全蓝各 2s 循环，不跑任何 UI。
 * 2026-11-15 已在 AA:D0 上目视确认三色纯正，通路 OK，故关掉。 */
#define PANEL_RGB_TEST 0

/* 小屏列偏移自检：ST7735S 80x160 最左侧固定竖条，轮播 off_x/off_y 供目视。
 * 2026-11-15 现场确认 X26 Y1 无竖线，已定稿进 profile，故关掉。 */
#define SMALL_OFFSET_TEST 0

#if SMALL_OFFSET_TEST
/* 每档显示 白底 -> 红底 各 3s：若那条线跟着变色说明这一列有被写，
 * 始终不变色说明它根本不在写入窗口内。串口同步打印 "SMALL DIAG X26 Y0 WHITE"。
 * 几何：sw_rotate 下逻辑 x -> 物理行(off_y)，逻辑 y -> 物理列(off_x)，
 * 所以横屏"竖线"由 off_y 决定，off_x 出错只会露出横条。 */
static void small_offset_selftest(void)
{
    static const struct { uint8_t ox, oy; } kCfg[] = {
        {26, 0},   /* 当前 profile，用来复现那条线 */
        {26, 1},   /* 行偏移 1：80x160 模块最常见的可见行 1..160 */
        {24, 1}, {25, 1}, {27, 1},
        {24, 0}, {25, 0}, {27, 0},
    };
    char buf[16];
    while (1) {
        for (int i = 0; i < (int)(sizeof(kCfg) / sizeof(kCfg[0])); i++) {
            display_set_offsets(kCfg[i].ox, kCfg[i].oy);
            snprintf(buf, sizeof(buf), "X%d Y%d", kCfg[i].ox, kCfg[i].oy);
            for (int p = 0; p < 2; p++) {
                color_t bg = p ? C_RGB_R : C_STONE_W;
                color_t fg = p ? C_STONE_W : C_STONE_B;
                display_fill(bg);
                draw_text(4, 4, buf, fg, -1);
                draw_text(4, SCR_H - 16, p ? "RED" : "WHITE", fg, -1);
                display_flush();
                printf("SMALL DIAG %s %s\n", buf, p ? "RED" : "WHITE");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }
    }
}
#endif

void app_main(void)
{
    gpio_init_all();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_my_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    /* 开机随机 seq 起点：对端按 (from,seq) 去重，固定从 0 开始会被当成重复帧丢掉 */
    s_seq = (uint16_t)esp_random();

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    printf("\n=== c3msg M1 (build %s %s) ===\n", __DATE__, __TIME__);
    printf("mac sta : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("my_id   : %08lX\n", (unsigned long)s_my_id);
    printf("chip    : model=%d rev=%d.%d cores=%d\n", ci.model, ci.revision / 100, ci.revision % 100, ci.cores);
    printf("flash   : %lu KB\n", (unsigned long)(flash_size / 1024));
    printf("now     : ESP-NOW ch1 + LoRa 433.000 SF12/BW125, ZD v1.1, preset=%d\n", PRESET_N);
    fflush(stdout);

    display_init();            /* 探测屏型 -> 选 profile -> 初始化面板 -> 分配帧缓冲 */

#if PANEL_RGB_TEST
    /* 最小验证：三屏纯色循环，永不返回。屏不纯正之前不上 UI。 */
    printf("PANEL_RGB_TEST: on, cycling R/G/B 2s each\n");
    fflush(stdout);
    display_rgb_cycle();
#endif

#if SMALL_OFFSET_TEST
    if (g_panel->sw_rotate) {          /* 只在小屏上跑，大屏走正常界面 */
        printf("SMALL_OFFSET_TEST: on, cycling offsets\n");
        fflush(stdout);
        small_offset_selftest();
    }
#endif

    ui_layout_init();
    monitor_init(espnow_send_mac);   /* 五子棋监视器页：GMK 镜像（与 LoRa 页共存） */

#if PANEL_SELFTEST
    panel_selftest();
#endif

    /* 开机 splash */
    {
        char sb[40];
        int y0 = (SCR_H >= 200) ? 40 : 10;
        fill_rect(0, 0, SCR_W, SCR_H, C_BG);
        draw_text(6, y0, "C3 MSG  M1", C_IN, -1);
        snprintf(sb, sizeof(sb), "ID %08lX", (unsigned long)s_my_id);
        draw_text(6, y0 + 20, sb, C_FG, -1);
        draw_text(6, y0 + 40, g_panel->name, C_DIM, -1);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(600));
    }

#if MON_SELFTEST
    mon_selftest();
#endif

    batt_init();
    batt_poll();
#if UI_DUMP_AT_BOOT
    /* 自检：造一个 LoRa 节点 + 两条消息（E/L 各一条），验证来源徽标与排版 */
    memset(&s_lnode[0], 0, sizeof(s_lnode[0]));
    s_lnode[0].id = 0x7DF43F68u;
    snprintf(s_lnode[0].sn, sizeof(s_lnode[0].sn), "3F68");
    snprintf(s_lnode[0].ln, sizeof(s_lnode[0].ln), "C3MSG 3f68");
    s_lnode[0].rssi = -51;
    s_lnode[0].snr4 = 26;
    s_lnode[0].hops = 1;
    s_lnode[0].seen = now_ms();
    s_lnode_cnt = 1;
    msg_add(0xA5AF0000u, 0, "ESP-NOW 测试消息", -62, 0);
    msg_add_ex(0x7DF43F68u, 0, "LoRa 测试消息", -51, 1, 1);
#endif
    ui_flush();
    ui_dump("msg-page");
#if UI_DUMP_AT_BOOT
    {
        int keep = s_mode;
        s_mode = 2;
        ui_draw();
        ui_dump("node-page");
        s_mode = keep;
        ui_draw();
    }
#endif

    wifi_now_init();
    ui_flush();

    lora_init(s_my_id);        /* 常驻 LoRa 接收 + NodeInfo 广播（独立任务） */
    mirror_init();             /* GMK 镜像兜底：监视器请求时经 LoRa 转发状态帧 */

    {   /* 官方 Meshtastic App 的 BLE 接口 */
        char bln[24], bsn[12];
        snprintf(bsn, sizeof(bsn), "%04x", (unsigned)(s_my_id & 0xFFFF));
        snprintf(bln, sizeof(bln), "C3MSG %s", bsn);
        ble_mesh_init(s_my_id, bln, bsn);
        ble_nodes_sync();
    }

    int btn_held = 0, btn_down = 0;
    int t_beat = 0, t_net = 0, t_ui = 0, t_batt = 0, t_mon = 0;
    int last_sec = -1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BTN_TICK_MS));
        int t = now_ms();

        lora_drain();          /* 只在主任务里改消息表/节点表 */
        mirror_tick(t);        /* 镜像兜底：文本/时间空闲时才发 */

#if LORA_UI_SELFTEST
        /* 自检：注入三档 GPS 状态的节点，切到节点页打印帧缓冲，验证“橙/红/灰”配色 */
        static int dumped = 0;
        if (!dumped) {
            lnode_update(0x11111111, "o001", "C3MSG o001", -61, 40, 1, 1);   /* 无 GPS -> 橙 */
            lnode_update(0x22222222, "r002", "GPS node", -70, 32, 2, 2);     /* 有 GPS -> 红 */
            lnode_update(0x33333333, "g003", "Meshtastic", -85, 16, 3, 0);   /* 未知 -> 灰 */
            int keep = s_mode;
            s_mode = 2;
            ui_draw();
            ui_dump("node-page");
            s_mode = keep;
            dumped = 1;
        }
#endif

        btn_poll(&btn_held, &btn_down);
        if (t - t_ui >= 200) {
            t_ui = t;
            ui_flush();
        }

        if (s_wifi_ok && t - t_beat >= 5000) {
            t_beat = t;
            uint8_t b[5];
            wr16(b, (uint16_t)(s_batt_mv > 0 ? s_batt_mv : 0));
            b[2] = (uint8_t)(s_batt_pct == 255 ? 0 : s_batt_pct);
            b[3] = 1;                       /* role: 1 = 节点/中继 */
            /* v1.1 caps: bit0=具备 LoRa 桥, bit1=当前允许发射（老解析器忽略此字节） */
            b[4] = (uint8_t)(0x01 | (lora_tx_allowed() ? 0x02 : 0));
            zd_send(ZT_BEAT, b, 5, 0, 0);
        }

        if (s_wifi_ok && t - t_net >= 15000) {
            t_net = t;
            uint8_t tab[6 * 4];
            int n = 0;
            wr32(tab + n * 6, s_my_id);
            tab[n * 6 + 4] = (uint8_t)(int8_t)0;
            tab[n * 6 + 5] = 0;
            n++;
            if (s_peer_addr && t - (int)s_peer_seen_ms < 15000) {
                wr32(tab + n * 6, s_peer_addr);
                tab[n * 6 + 4] = (uint8_t)(int8_t)s_peer_rssi;
                tab[n * 6 + 5] = (uint8_t)s_peer_hops;
                n++;
            }
            zd_send(ZT_NODETAB, tab, (uint16_t)(n * 6), 0, 0);
        }

        if (t - t_batt >= 30000) {
            t_batt = t;
            batt_poll();
        }

        if (t - t_mon >= 30000) {
            t_mon = t;
            int nb = 0, ns = 0, na = 0, gid = 0;
            char nm[20] = "";
            monitor_get_stats(&nb, &ns, &na, &gid, nm, sizeof(nm));
            if (nb || ns || na)
                ESP_LOGI(TAG, "MON beac=%d state=%d app=%d gid=%d name=%s",
                         nb, ns, na, gid, nm);
        }

#if C3MSG_BOOT_TEST
        /* 自检：每 20s 发一条，最多 5 条，用于无人值守验证双向收发 */
        static int t_test, n_test;
        if (s_wifi_ok && n_test < 5 && t - t_test >= 20000) {
            t_test = t;
            n_test++;
            char tb[24];
            snprintf(tb, sizeof(tb), "t%04lX-%d", (unsigned long)(s_my_id & 0xFFFF), n_test);
            send_text(tb);
        }
#endif

#if C3MSG_M2_TEST
        m2_test_tick(t);
#endif

        /* 发送失败重试：等 ACK 超时就用同一 seq 重发（对端按 seq 去重），最多 3 次 */
        if (s_wifi_ok) {
            for (int i = 0; i < s_msg_cnt; i++) {
                msg_t *m = msg_at(i);
                if (!m || !m->out || m->st != MS_PENDING) continue;
                if ((int32_t)(t - m->next_ms) < 0) continue;
                if (m->tries >= 3) {
                    m->st = MS_FAILED;
                    ESP_LOGW(TAG, "MSG FAIL seq=%u tries=%u", m->seq, m->tries);
                    continue;
                }
                m->tries++;
                m->next_ms = t + 1500;
                zd_send_ex(ZT_TEXT, (const uint8_t *)m->text, (uint16_t)strlen(m->text),
                           m->msgid, ZF_WANT_ACK, m->seq);
                ESP_LOGW(TAG, "MSG retry %u seq=%u", m->tries, m->seq);
            }
        }

        int sec = t / 1000;
        if (sec != last_sec) {
            last_sec = sec;
            if (sec % 30 == 0) {
                char bs[32];
                if (s_batt_mv > 2500)
                    snprintf(bs, sizeof(bs), "%dmV/%d%%", s_batt_mv, s_batt_pct);
                else
                    snprintf(bs, sizeof(bs), "no-batt");
                ESP_LOGI(TAG, "up %ds batt=%s peer=%08lx rssi=%d msgs=%d tx=%lu/%lu rly=%lu dup=%lu c3drop=%lu lnode=%d heap=%lu",
                         sec, bs, (unsigned long)s_peer_addr, s_peer_rssi,
                         s_msg_cnt, (unsigned long)s_tx_ok, (unsigned long)s_tx_err,
                         (unsigned long)s_relay_cnt, (unsigned long)s_dup_cnt,
                         (unsigned long)s_c3drop_cnt,
                         s_lnode_cnt, (unsigned long)esp_get_free_heap_size());
                /* 时间源每 30 s 广播一次（ZD + LoRa）；没同步就什么都不发 */
                if (s_tsrc != 0 && (uint32_t)now_ms() - s_tlast_bcast >= 30000)
                    time_broadcast();
            }
        }
    }
}

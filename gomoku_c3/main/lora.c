/*
 * lora.c — Meshtastic 兼容层，移植自已实机验证的 mtx_min
 *
 * 已验证的真值（从原厂 SPIFFS 配置解出，官方节点实收）：
 *   region MY_433 + LONG_SLOW + override_frequency 433.0MHz
 *   频道 "awa"，32B PSK，channel hash 0x17
 *   SF12 / BW125 / CR4-8 / LDRO=1，sync word 实际字节 0x24 0xB4
 *   16B 明文头 {to,from,id,flags,hash,next_hop,relay} + AES256-CTR(Data)
 *
 * 本模块只做两件事：
 *   1) 常驻接收：解 NodeInfo / 文本，塞进队列交给 UI 任务显示；
 *   2) 让本机在 LoRa 网络里可见：开机 30s 后、之后每 30 分钟广播一次 NodeInfo。
 *
 * 共存约束（血泪教训）：
 *   - SPI2 与 LCD 共用总线；LCD 是手动 CS，必须在整个 CS 窗口内持有总线，
 *     否则 LoRa 事务会插进 LCD 的 CS 低位窗口里，屏幕直接花。
 *   - 绝不动 GPIO3(DIO1)：不扫 ADC、不改上下拉、不用它判完成。
 *   - 不写 NVS / 分区表 / bootloader，只在 app 分区里跑。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_err.h"
#include "hal/usb_serial_jtag_ll.h"
#include "mbedtls/aes.h"

#include "lora.h"
#include "bus.h"   /* 收到的事件额外 publish 到模块总线（gomoku_c3 模块化叠加） */

/* ==================== 引脚（与 main.c 一致） ==================== */
#define P_SCK    10
#define P_MOSI   7
#define P_MISO   6
#define P_CS     8
#define P_RST    5
#define P_BUSY   4
#define P_DIO1   3
#define P_RXEN   12

#define FREQ_HZ      433000000.0
#define XTAL_HZ      32000000.0
#define CH_HASH      0x17

#define PORT_TEXT    1
#define PORT_NODE    4
#define PORT_TIME    0x0101      /* 自定义私有 portnum：ZD 时间广播（Meshtastic 侧按未知 port 忽略） */
#define PORT_MIRROR  0x0102      /* 私有：GMK 状态镜像兜底（桥侧 c3msg 发出） */

/* GPS 能力上报（写进 NodeInfo.User 私有字段 15）：
 * 1=本机无 GPS 硬件（对端显示橙色），2=有 GPS（红色），0/缺省=未知。
 * 三台设备都没有 GNSS 模块，明确上报 1；以后接 GPS 改成 2。 */
#ifndef C3MSG_GPS_STATE
#define C3MSG_GPS_STATE 1
#endif

#define TX_DBM       20          /* 20dBm 已实测稳定；22dBm 虽 10/10 通过，产线不冒险 */
#define TX_DBM_SAFE  14          /* 天线状态未确认时的保守档：宁可少传，不烧功放 */

/* ==================== 调制档位（三台必须完全一致） ====================
 * 本模块（E22-400M22S / SX1268）实测确认：BW 码 0x04=125k、0x05=250k、0x06=500k。
 * Meshtastic 对应关系：
 *   LONG_SLOW = SF12 / BW125 / CR4-8 / LDRO=1（原档，一帧约 4.3 s）
 *   LONG_FAST = SF11 / BW250 / CR4-5 / LDRO=0（新默认，同长度约 1.1 s）
 * 只改空口参数，帧格式/频道 hash/PSK 全不动，所以协议向后兼容。 */
typedef struct {
    const char *name;
    uint8_t  sf;            /* SetModulationParams byte0 */
    uint8_t  bw;            /* byte1 */
    uint8_t  cr;            /* byte2：0x01=4/5 … 0x04=4/8 */
    uint8_t  ldro;          /* byte3 */
    uint16_t ms_per_byte;   /* 空口估算（ms/B，SF12BW125≈54.3，SF11BW250≈13.6） */
    uint16_t preamble;      /* 前导符号数（Meshtastic 统一 16） */
} lora_preset_t;

static const lora_preset_t kPreset[] = {
    {"LONG_FAST", 0x0B, 0x05, 0x01, 0x00, 14, 16},
    {"LONG_SLOW", 0x0C, 0x04, 0x04, 0x01, 54, 16},
};
#define PRESET_N ((int)(sizeof(kPreset) / sizeof(kPreset[0])))
static int s_preset = 0;        /* 0=LONG_FAST（默认）1=LONG_SLOW */
#define NODEINFO_MS  (30 * 60 * 1000)
#define BOOT_TX_MS   30000

static const uint8_t PSK[32] = {
    0x85, 0x1F, 0x24, 0x15, 0x01, 0x3B, 0xB4, 0xB5, 0xA8, 0x09, 0x3D, 0x70, 0x1C, 0xD4, 0xE3, 0x5A,
    0x4A, 0x3C, 0xAF, 0x75, 0x40, 0xA0, 0x04, 0xB3, 0xBB, 0x48, 0xC5, 0x79, 0x93, 0x05, 0x52, 0x1D};

/* 非阻塞日志：主机不读 USB FIFO 时绝不阻塞调用任务（与 main.c 的 nblog 同一策略） */
static void llog(const char *fmt, ...)
{
    char b[200];
    int n = snprintf(b, sizeof(b), "[%6d] L ", (int)(esp_timer_get_time() / 1000));
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof(b) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    if (n < 0) return;
    b[n++] = '\n';

    int off = 0, budget = 30;
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
#define LLOGI(fmt, ...) llog("I " fmt, ##__VA_ARGS__)
#define LLOGW(fmt, ...) llog("W " fmt, ##__VA_ARGS__)

/* 调试开关：1 = 打印收到的原始帧前 40 字节（定位 SPI 读通路位翻转） */
#define LRX_HEXLOG 0
__attribute__((unused)) static void dbg_hex(const char *tag, const uint8_t *d, int n)
{
    char b[160];
    int p = snprintf(b, sizeof(b), "%s[%d]", tag, n);
    for (int i = 0; i < n && i < 40 && p < (int)sizeof(b) - 4; i++)
        p += snprintf(b + p, sizeof(b) - (size_t)p, " %02x", d[i]);
    llog("%s", b);
}

static spi_device_handle_t s_spi;
static uint32_t g_node;
static uint8_t  s_mac[6];

/* 天线安全红线：50:78:7D:F2:AA:D0 的 433 天线可能已断，默认只收不发。
 * 这里做两层保护：① 禁止任何 SetTx；② 即使误调发射，PA 也只给最低档 -9dBm。 */
static int s_tx_ok = 1;                 /* 0 = RX only */
static int s_tx_block_logged = 0;
static int s_tx_dbm = TX_DBM;           /* 设置页可调，受 s_tx_ok 约束 */
static const uint8_t NO_ANTENNA_MAC[6] = {0x50, 0x78, 0x7D, 0xF2, 0xAA, 0xD0};

/* 文本发送请求队列：BLE 与 ZD 桥只入队，实际射频操作由 lora 任务串行执行 */
typedef struct {
    uint32_t to;
    uint16_t len;
    uint16_t tag;           /* 原样回传在 LORA_EVT_TXDONE，0=不关心 */
    uint8_t  kind;          /* 0=文本 1=时间广播 */
    uint32_t utc;           /* kind=1: UTC 秒 */
    uint8_t  tsrc;          /* kind=1: 0=未同步 1=GPS 2=手动 */
    char     text[80];
} lora_txreq_t;
static QueueHandle_t s_txq;
static volatile uint32_t s_nmirror;   /* 收到的镜像兜底帧数 */
static lora_mirror_cb_t s_mirror_cb;
static QueueHandle_t s_q;

static void tx_id_add(uint32_t id);
static int  tx_id_recent(uint32_t id);

static volatile uint32_t s_nrx, s_ncrc, s_nto, s_nhash, s_nnode, s_ntext, s_nother, s_ndup;

/* ==================== 基础 SPI ==================== */
static int busy(void) { return gpio_get_level(P_BUSY); }

static int wait_busy_low(int ms)
{
    int64_t t0 = esp_timer_get_time();
    while (busy()) {
        if ((esp_timer_get_time() - t0) / 1000 > ms) return -1;
        esp_rom_delay_us(50);
    }
    return 0;
}

/* 基座推屏用「手动 CS + 多笔事务」，整窗期间 LCD CS(GPIO1) 保持低。
 * 若此时发起 LoRa 事务，面板会把 LoRa 的字节当成像素数据（花屏）。
 * 这里只在驱动侧让路，不改基座推屏逻辑。 */
#define P_LCD_CS 1
static void wait_lcd_idle(void)
{
    for (int i = 0; i < 40; i++) { /* 最多让路 ~40ms */
        if (gpio_get_level(P_LCD_CS)) return;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void xfer(const uint8_t *tx, uint8_t *rx, int n)
{
    spi_transaction_t t = {0};
    t.length = (size_t)n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    /* 手动 CS：整个 CS 低位窗口独占总线，避免和 LCD 的 CS 窗口交叠 */
    wait_lcd_idle();
    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    gpio_set_level(P_CS, 0);
    esp_rom_delay_us(2);
    spi_device_polling_transmit(s_spi, &t);
    esp_rom_delay_us(2);
    gpio_set_level(P_CS, 1);
    spi_device_release_bus(s_spi);
}

static int cmd(uint8_t op, const uint8_t *p, int n)
{
    uint8_t tx[1 + 96] = {0}, rx[1 + 96] = {0};
    if (n > 96) return -100;
    if (wait_busy_low(100) < 0) return -1;
    tx[0] = op;
    if (n > 0) memcpy(tx + 1, p, (size_t)n);
    xfer(tx, rx, 1 + n);
    esp_rom_delay_us(50);
    if (wait_busy_low(100) < 0) return -2;
    return rx[0];
}

/* 读命令应答：数据从 rx[2] 起；两次一致才采信（本板 SPI 读通路有裕量问题） */
static int query(uint8_t op, uint8_t *out, int nout)
{
    uint8_t tx[16] = {op}, rx[16] = {0};
    uint8_t prev[8] = {0};
    if (3 + nout > 16) return -100;
    for (int k = 0; k < 5; k++) {
        if (wait_busy_low(100) < 0) return -1;
        memset(rx, 0, sizeof(rx));
        tx[0] = op;
        xfer(tx, rx, 3 + nout);
        if (k > 0 && memcmp(prev, rx + 2, (size_t)nout) == 0) {
            memcpy(out, rx + 2, (size_t)nout);
            return rx[0];
        }
        memcpy(prev, rx + 2, (size_t)nout);
        esp_rom_delay_us(300);
    }
    memcpy(out, prev, (size_t)nout);
    return 0;
}

static int rd_reg(uint16_t addr, uint8_t *out, int n)
{
    uint8_t tx[4 + 32] = {(uint8_t)0x1D, (uint8_t)(addr >> 8), (uint8_t)addr, 0x00};
    uint8_t rx[4 + 32] = {0};
    uint8_t prev[32] = {0};
    if (n > 32) return -1;
    for (int k = 0; k < 5; k++) {
        if (wait_busy_low(100) < 0) return -1;
        memset(rx, 0, sizeof(rx));
        xfer(tx, rx, 4 + n);
        if (k > 0 && memcmp(prev, rx + 4, (size_t)n) == 0) {
            memcpy(out, rx + 4, (size_t)n);
            return 0;
        }
        memcpy(prev, rx + 4, (size_t)n);
        esp_rom_delay_us(300);
    }
    memcpy(out, prev, (size_t)n);
    return 0;
}

static void wr_reg(uint16_t addr, const uint8_t *d, int n)
{
    uint8_t tx[3 + 8] = {(uint8_t)0x0D, (uint8_t)(addr >> 8), (uint8_t)addr};
    uint8_t rx[3 + 8] = {0};
    if (n > 8) return;
    memcpy(tx + 3, d, (size_t)n);
    if (wait_busy_low(100) < 0) return;
    xfer(tx, rx, 3 + n);
}

static int get_irq(uint16_t *irq)
{
    uint8_t v[2] = {0};
    int st = query(0x12, v, 2);
    *irq = (uint16_t)((v[0] << 8) | v[1]);
    return st;
}

static void clear_irq(uint16_t irq)
{
    uint8_t p[2] = {(uint8_t)(irq >> 8), (uint8_t)irq};
    cmd(0x02, p, 2);
}

static int irq_raw(uint16_t *irq)
{
    uint8_t tx[6] = {0x12, 0, 0, 0, 0, 0}, rx[6] = {0};
    xfer(tx, rx, 6);
    *irq = (uint16_t)((rx[2] << 8) | rx[3]);
    return rx[0];
}

static void hard_reset(void)
{
    gpio_set_level(P_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(P_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ==================== 射频初始化 ==================== */
static int s_tcxo_v = 7;          /* 本板 E22 只有 3.3V(code 7) 档能真正进 SF12 */
static int s_poll_ms = 15000;
static int s_min_air;
static int s_tx_ms;
static int s_dio1;

static void set_packet_params(uint8_t len)
{
    uint8_t pp[6] = {0x00, kPreset[s_preset].preamble, 0x00, len, 0x01, 0x00};   /* 前导16/显式头/CRC/IQ标准 */
    cmd(0x8C, pp, 6);
}

static void set_irq_mask(uint16_t mask)
{
    uint8_t p[8] = {(uint8_t)(mask >> 8), (uint8_t)mask,
                    (uint8_t)(mask >> 8), (uint8_t)mask,
                    0x00, 0x00, 0x00, 0x00};
    cmd(0x08, p, 8);
}

/* Meshtastic syncWord=0x2B/0x44 -> RadioLib 映射 {0x24,0xB4}；读到一致才算写入成功 */
static int set_sync_word(void)
{
    static const uint8_t want[2] = {0x24, 0xB4};
    for (int i = 0; i < 8; i++) {
        wr_reg(0x0740, want, 2);
        uint8_t rb[8] = {0};
        rd_reg(0x0740, rb, 6);
        for (int k = 0; k + 1 < 6; k++)
            if (rb[k] == 0x24 && rb[k + 1] == 0xB4) return 1;
    }
    return 0;
}

static void radio_setup(int tcxo_v)
{
    hard_reset();
    uint8_t st = 0x00;
    cmd(0x80, &st, 1);

    if (tcxo_v) {
        uint8_t tcxo[4] = {(uint8_t)tcxo_v, 0x00, 0x02, 0x80};   /* 10ms 超时 */
        cmd(0x97, tcxo, 4);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint8_t reg = 0x01;
    cmd(0x96, &reg, 1);

    uint8_t pkt = 0x01;
    cmd(0x8A, &pkt, 1);

    uint32_t frf = (uint32_t)((FREQ_HZ * 33554432.0) / XTAL_HZ + 0.5);
    uint8_t fp[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    cmd(0x86, fp, 4);

    uint8_t pa[4] = {0x04, 0x07, 0x00, 0x01};
    cmd(0x95, pa, 4);

    uint8_t txp[2] = {(uint8_t)(s_tx_ok ? s_tx_dbm : -9), 0x04};
    cmd(0x8E, txp, 2);

    uint8_t mod[4] = {kPreset[s_preset].sf, kPreset[s_preset].bw,
                      kPreset[s_preset].cr, kPreset[s_preset].ldro};
    cmd(0x8B, mod, 4);

    uint8_t d2 = 0x01;
    cmd(0x9D, &d2, 1);
    set_irq_mask(0x0243);
}

static void set_tx_power(int dbm)
{
    uint8_t txp[2] = {(uint8_t)dbm, 0x04};
    cmd(0x8E, txp, 2);
}

static void write_payload(const uint8_t *d, int n)
{
    uint8_t base[2] = {0x00, 0x00};
    cmd(0x8F, base, 2);
    uint8_t wb[1 + 64];
    if (n > 64) n = 64;
    wb[0] = 0x00;
    memcpy(wb + 1, d, (size_t)n);
    cmd(0x0E, wb, 1 + n);
}

/* 发射并等 TxDone：DIO1 本板不可靠，轮询 IRQ；连续两次合法 IRQ 且过了理论空口时间才采信 */
static int tx_wait_done(uint16_t *out_irq, uint16_t *out_err)
{
    uint8_t d2 = 0x01;
    cmd(0x9D, &d2, 1);
    uint8_t rc = 0x00;
    cmd(0x80, &rc, 1);
    set_irq_mask(0x0243);
    clear_irq(0xFFFF);

    gpio_set_level(P_RXEN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t to[3] = {0x00, 0x00, 0x00};
    if (cmd(0x83, to, 3) < 0) {
        LLOGW("  txwait: SetTx 被拒绝（BUSY 未释放）");
        *out_irq = 0;
        *out_err = 0xEEEE;
        return 0;
    }

    int64_t t0 = esp_timer_get_time();
    s_dio1 = 0;
    int saw_tx = 0, settle = 0, done = 0, notx = 0;
    for (;;) {
        if (esp_timer_get_time() - t0 > (int64_t)s_poll_ms * 1000) break;
        if (gpio_get_level(P_DIO1)) s_dio1 = 1;

        uint16_t irq = 0;
        int st = irq_raw(&irq);
        int mode = (st >> 4) & 0x07;      /* 0x6 = TX；0x2 = STDBY */
        if (st & 0x80) {
            if (mode == 0x06) {
                saw_tx = 1;
                settle = 0;
            } else if (saw_tx) {
                if (esp_timer_get_time() - t0 < (int64_t)s_min_air * 1000) settle = 0;
                else if (++settle >= 3) { done = 1; break; }
            } else if (esp_timer_get_time() - t0 > 2000000) {
                notx = 1;
                break;
            }
        } else {
            settle = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_tx_ms = (int)((esp_timer_get_time() - t0) / 1000);

    vTaskDelay(pdMS_TO_TICKS(200));
    uint16_t irq = 0;
    irq_raw(&irq);
    *out_irq = irq;

    uint8_t ev[2] = {0};
    if (query(0x17, ev, 2) >= 0) *out_err = (uint16_t)((ev[0] << 8) | ev[1]);
    else *out_err = 0xEEEE;

    LLOGI("  txwait: dio1=%d t=%dms irq=0x%04X err=0x%04X%s", s_dio1, s_tx_ms, irq, *out_err,
          notx ? " (未进TX模式)" : "");

    clear_irq(0xFFFF);
    uint8_t sb = 0x00;
    cmd(0x80, &sb, 1);
    return done;
}

/* ==================== AES256-CTR ==================== */
static void aesctr(uint32_t from, uint32_t id, uint8_t *buf, int n)
{
    uint8_t nonce[16] = {0};
    uint8_t stream[16] = {0};
    size_t off = 0;
    memcpy(nonce, &id, 4);          /* 与官方 initNonce 等价：id 在前 8B、from 在 8..11 */
    memcpy(nonce + 8, &from, 4);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, PSK, 256);
    mbedtls_aes_crypt_ctr(&ctx, (size_t)n, &off, nonce, stream, buf, buf);
    mbedtls_aes_free(&ctx);
}

/* ==================== protobuf ==================== */
static int pb_varint(uint8_t *o, uint64_t v)
{
    int n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        o[n++] = b;
    } while (v);
    return n;
}

static int pb_tag(uint8_t *o, int field, int wt) { return pb_varint(o, (uint64_t)((field << 3) | wt)); }

static int pb_u32(uint8_t *o, int field, uint32_t v)
{
    int p = pb_tag(o, field, 0);
    return p + pb_varint(o + p, v);
}

static int pb_bytes(uint8_t *o, int field, const uint8_t *d, int n)
{
    int p = pb_tag(o, field, 2);
    p += pb_varint(o + p, (uint64_t)n);
    memcpy(o + p, d, (size_t)n);
    return p + n;
}

static int pb_string(uint8_t *o, int field, const char *s)
{
    return pb_bytes(o, field, (const uint8_t *)s, (int)strlen(s));
}

/* ==================== 帧构造（NodeInfo 广播） ==================== */
static void put32le(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int build_user(uint8_t *u)
{
    int n = 0;
    char idbuf[16], lname[20], sname[8];
    snprintf(idbuf, sizeof(idbuf), "!%08x", (unsigned)g_node);
    snprintf(sname, sizeof(sname), "%04x", (unsigned)(g_node & 0xFFFF));
    snprintf(lname, sizeof(lname), "C3MSG %s", sname);
    n += pb_string(u + n, 1, idbuf);
    n += pb_string(u + n, 2, lname);
    n += pb_string(u + n, 3, sname);
    n += pb_bytes(u + n, 4, s_mac, 6);
    n += pb_u32(u + n, 5, 255);       /* hw_model 未识别，官方列表里照常显示 */
    /* 私有能力位（官方 App 会当作未知字段跳过）：
     * 15 = GPS 状态，1=无 GPS（本机橙） 2=有 GPS（对端红），缺省=未知 */
    n += pb_u32(u + n, 15, C3MSG_GPS_STATE);
    return n;
}

static int build_nodeinfo(uint8_t *out, uint32_t id)
{
    uint8_t user[96], data[128];
    int un = build_user(user);
    int dn = 0;
    dn += pb_u32(data + dn, 1, PORT_NODE);
    dn += pb_bytes(data + dn, 2, user, un);
    aesctr(g_node, id, data, dn);

    put32le(out, 0xFFFFFFFFu);
    put32le(out + 4, g_node);
    put32le(out + 8, id);
    out[12] = 0xE7;                   /* hop_limit=7, hop_start=7 */
    out[13] = CH_HASH;
    out[14] = 0x00;
    out[15] = 0x00;
    memcpy(out + 16, data, (size_t)dn);
    return 16 + dn;
}

/* 时间广播：portnum=PORT_TIME，payload 6B = utc32 LE + 来源 + 精度秒。
 * 老固件按未知 port 忽略，不影响文本/节点功能。 */
static int tx_time(uint32_t utc, uint8_t src)
{
    if (!s_tx_ok) return 0;
    uint8_t pay[8] = {0}, data[64], frame[96];
    put32le(pay, utc);
    pay[4] = src;
    pay[5] = src == 1 ? 1 : 5;        /* 精度：GPS 1s，手动 5s */
    int dn = 0;
    dn += pb_u32(data + dn, 1, PORT_TIME);
    dn += pb_bytes(data + dn, 2, pay, 6);
    uint32_t id = (uint32_t)(esp_timer_get_time() / 1000) ^ (uint32_t)esp_random();
    if (!id) id = 1;
    aesctr(g_node, id, data, dn);

    put32le(frame, 0xFFFFFFFFu);
    put32le(frame + 4, g_node);
    put32le(frame + 8, id);
    frame[12] = 0xE7;
    frame[13] = CH_HASH;
    frame[14] = 0x00;
    frame[15] = 0x00;
    memcpy(frame + 16, data, (size_t)dn);
    int n = 16 + dn;

    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(200));
        radio_setup(s_tcxo_v);
        set_sync_word();
        s_poll_ms = 15000;
        s_min_air = (int)((kPreset[s_preset].ms_per_byte * n + 900) * 0.75);
        set_tx_power(s_tx_dbm);
        write_payload(frame, n);
        set_packet_params((uint8_t)n);
        uint16_t irq = 0, err = 0;
        ok = tx_wait_done(&irq, &err);
    }
    LLOGI("LTX TIME utc=%lu src=%u len=%d dbm=%d %s air=%dms", (unsigned long)utc, src, n,
          s_tx_dbm, ok ? "DONE" : "TIMEOUT", s_tx_ms);
    if (ok) tx_id_add(id);
    return ok;
}

static int tx_nodeinfo(void)
{
    if (!s_tx_ok) {
        if (!s_tx_block_logged) {
            s_tx_block_logged = 1;
            LLOGW("TX disabled: no antenna — LoRa TX blocked, RX only (node=!%08x)", (unsigned)g_node);
        }
        return 0;
    }
    uint8_t frame[128];
    uint32_t id = (uint32_t)(esp_timer_get_time() / 1000) ^ (uint32_t)esp_random();
    if (!id) id = 1;
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);
    int n = build_nodeinfo(frame, id);
    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(200));
        radio_setup(s_tcxo_v);
        set_sync_word();
        s_poll_ms = 15000;
        s_min_air = (int)((kPreset[s_preset].ms_per_byte * n + 900) * 0.75);
        set_tx_power(s_tx_dbm);
        write_payload(frame, n);
        set_packet_params((uint8_t)n);
        uint16_t irq = 0, err = 0;
        ok = tx_wait_done(&irq, &err);
        LLOGI("LTX NODEINFO id=0x%08lx len=%d dbm=%d irq=0x%04X %s%s", (unsigned long)id, n,
              s_tx_dbm, irq, ok ? "DONE" : "TIMEOUT", attempt ? " (retry)" : "");
    }
    LLOGI("LTX NODEINFO done=%d node=!%08x air=%dms wall=%lums", ok, (unsigned)g_node, s_tx_ms,
          (unsigned long)((uint32_t)(esp_timer_get_time() / 1000) - start));
    return ok;
}

/* 文本发送：由 lora 任务串行调用，发完由调用方恢复 RX */
static int tx_text(uint32_t to, const char *text, uint16_t len)
{
    if (!s_tx_ok) {
        if (!s_tx_block_logged) {
            s_tx_block_logged = 1;
            LLOGW("TX disabled: no antenna — text TX blocked, RX only");
        }
        return 0;
    }
    if (len > 72) len = 72;
    uint8_t data[128], frame[160];
    int dn = 0;
    dn += pb_u32(data + dn, 1, PORT_TEXT);
    dn += pb_bytes(data + dn, 2, (const uint8_t *)text, (int)len);
    uint32_t id = (uint32_t)(esp_timer_get_time() / 1000) ^ (uint32_t)esp_random();
    if (!id) id = 1;
    aesctr(g_node, id, data, dn);

    put32le(frame, to);
    put32le(frame + 4, g_node);
    put32le(frame + 8, id);
    frame[12] = 0xE7;                   /* hop_limit=7, hop_start=7 */
    frame[13] = CH_HASH;
    frame[14] = 0x00;
    frame[15] = 0x00;
    memcpy(frame + 16, data, (size_t)dn);
    int n = 16 + dn;

    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);
    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(200));
        radio_setup(s_tcxo_v);
        set_sync_word();
        s_poll_ms = 15000;
        s_min_air = (int)((kPreset[s_preset].ms_per_byte * n + 900) * 0.75);
        set_tx_power(s_tx_dbm);
        write_payload(frame, n);
        set_packet_params((uint8_t)n);
        uint16_t irq = 0, err = 0;
        ok = tx_wait_done(&irq, &err);
        LLOGI("LTX TEXT id=0x%08lx to=!%08x len=%d dbm=%d irq=0x%04X %s%s",
              (unsigned long)id, (unsigned)to, n, s_tx_dbm, irq,
              ok ? "DONE" : "TIMEOUT", attempt ? " (retry)" : "");
    }
    LLOGI("LTX TEXT done=%d air=%dms wall=%lums", ok, s_tx_ms,
          (unsigned long)((uint32_t)(esp_timer_get_time() / 1000) - start));
    if (ok) tx_id_add(id);
    return ok;
}

/* ==================== 接收 ==================== */
static void rx_start(void)
{
    uint8_t d2 = 0x01;
    cmd(0x9D, &d2, 1);
    set_packet_params(0xFF);
    set_irq_mask(0x0243);
    clear_irq(0xFFFF);
    gpio_set_level(P_RXEN, 1);
    uint8_t to[3] = {0xFF, 0xFF, 0xFF};
    cmd(0x82, to, 3);
}

static int rd_varint(const uint8_t *p, int n, int *i, uint32_t *v)
{
    uint32_t r = 0;
    int s = 0;
    while (*i < n && s < 32) {
        uint8_t c = p[(*i)++];
        r |= (uint32_t)(c & 0x7F) << s;
        if (!(c & 0x80)) { *v = r; return 1; }
        s += 7;
    }
    return 0;
}

/* 去重：同一 (from,id) 只处理一次（多路径/中继会重复到达） */
#define LDEDUP_N 24
static uint32_t s_dd_from[LDEDUP_N];
static uint32_t s_dd_id[LDEDUP_N];
static int s_dd_pos;

static int lora_seen(uint32_t from, uint32_t id)
{
    for (int i = 0; i < LDEDUP_N; i++)
        if (s_dd_from[i] == from && s_dd_id[i] == id) return 1;
    s_dd_from[s_dd_pos] = from;
    s_dd_id[s_dd_pos] = id;
    s_dd_pos = (s_dd_pos + 1) % LDEDUP_N;
    return 0;
}

/* 本机刚发出的文本包 id：若被其他节点转发回来，按回环丢弃，避免 ADV 看到自己发的消息 */
#define LTX_ID_N 8
static uint32_t s_tx_ids[LTX_ID_N];
static int s_tx_id_pos;

static void tx_id_add(uint32_t id)
{
    if (!id) return;
    s_tx_ids[s_tx_id_pos] = id;
    s_tx_id_pos = (s_tx_id_pos + 1) % LTX_ID_N;
}

static int tx_id_recent(uint32_t id)
{
    if (!id) return 0;
    for (int i = 0; i < LTX_ID_N; i++)
        if (s_tx_ids[i] == id) return 1;
    return 0;
}

/* 把收到的事件镜像到模块总线：模块之间因此不需要互相 include */
static void publish_bus(const lora_evt_t *e)
{
    bus_ev_t b;
    memset(&b, 0, sizeof(b));
    if (e->kind == LORA_EVT_TEXT) {
        b.type = EV_LORA_TEXT;
        b.a = (uint32_t)(uint8_t)e->rssi | ((uint32_t)(uint8_t)e->snr4 << 8);
        b.b = e->from;
        memcpy(b.text, e->text, sizeof(b.text) - 1); /* 96 字节总线缓冲，截断即可 */
        b.text[sizeof(b.text) - 1] = 0;
    } else if (e->kind == LORA_EVT_NODE) {
        b.type = EV_NODE_UP;
        b.a = e->from;
        b.b = ((uint32_t)e->hops << 8) | e->gps;
        snprintf(b.text, sizeof(b.text), "%s", e->short_name);
    } else if (e->kind == LORA_EVT_TIME) {
        b.type = EV_TIME;
        b.a = e->utc;
        b.b = e->tsrc;
    } else {
        return; /* TXDONE 只在驱动内部用 */
    }
    bus_publish(&b);
}

static void push_evt(const lora_evt_t *e)
{
    if (!s_q || xQueueSend(s_q, e, 0) != pdTRUE) LLOGW("evt 队列满，丢弃 kind=%u from=%08lx", e->kind,
                                                       (unsigned long)e->from);
    publish_bus(e);
}

static void cpystr(char *dst, size_t cap, const uint8_t *src, uint32_t n)
{
    if (n >= cap) {
        n = (uint32_t)cap - 1;
        /* 别把 UTF-8 字符截成半个，否则屏上会出现替换符 */
        while (n > 0 && (src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void parse_user(const uint8_t *p, int n, lora_evt_t *e)
{
    int i = 0;
    while (i < n) {
        uint32_t key;
        if (!rd_varint(p, n, &i, &key)) break;
        int field = (int)(key >> 3), wt = (int)(key & 7);
        if (wt == 2) {
            uint32_t l;
            if (!rd_varint(p, n, &i, &l)) break;
            if ((int)l < 0 || i + (int)l > n) break;
            if (field == 2) cpystr(e->long_name, sizeof(e->long_name), p + i, l);
            else if (field == 3) cpystr(e->short_name, sizeof(e->short_name), p + i, l);
            i += (int)l;
        } else if (wt == 0) {
            uint32_t v;
            if (!rd_varint(p, n, &i, &v)) break;
            if (field == 15 && (v == 1 || v == 2)) e->gps = (uint8_t)v;
        } else break;
    }
    if (!e->short_name[0] && e->long_name[0]) {
        size_t l = strlen(e->long_name);
        cpystr(e->short_name, sizeof(e->short_name),
               (const uint8_t *)e->long_name + (l > 4 ? l - 4 : 0), (uint32_t)(l > 4 ? 4 : l));
    }
    if (!e->short_name[0]) snprintf(e->short_name, sizeof(e->short_name), "%04lx", (unsigned long)(e->from & 0xFFFF));
}

static void decode(const uint8_t *f, int n, int8_t rssi, int8_t snr4)
{
    if (n < 17) return;

    uint32_t dst = rd32le(f);
    uint32_t from = rd32le(f + 4);
    uint32_t id = rd32le(f + 8);
    if (from == g_node) return;                       /* 自己的包 */
    if (dst != 0xFFFFFFFFu && dst != g_node) { s_nother++; return; }
    if (f[13] != CH_HASH) { s_nhash++; return; }

    uint8_t hl = (uint8_t)(f[12] & 0x07), hs = (uint8_t)((f[12] >> 5) & 0x07);
    uint8_t hops = (hs > hl) ? (uint8_t)(hs - hl) : 0;

    if (lora_seen(from, id)) { s_ndup++; return; }

    static uint8_t buf[280];
    int dn = n - 16;
    if (dn > (int)sizeof(buf)) dn = (int)sizeof(buf);
    memcpy(buf, f + 16, (size_t)dn);
    aesctr(from, id, buf, dn);

    int i = 0;
    uint32_t portnum = 0;
    const uint8_t *pay = NULL;
    int plen = 0;
    while (i < dn) {
        uint32_t key;
        if (!rd_varint(buf, dn, &i, &key)) break;
        int field = (int)(key >> 3), wt = (int)(key & 7);
        if (wt == 0) {
            uint32_t v;
            if (!rd_varint(buf, dn, &i, &v)) break;
            if (field == 1) portnum = v;
        } else if (wt == 2) {
            uint32_t l;
            if (!rd_varint(buf, dn, &i, &l)) break;
            if ((int)l < 0 || i + (int)l > dn) break;
            if (field == 2) { pay = buf + i; plen = (int)l; }
            i += (int)l;
        } else break;
    }

    lora_evt_t e;
    memset(&e, 0, sizeof(e));
    e.from = from;
    e.rssi = rssi;
    e.snr4 = snr4;
    e.hops = hops;

    if (portnum == PORT_NODE && pay && plen > 0) {
        parse_user(pay, plen, &e);
        e.kind = LORA_EVT_NODE;
        s_nnode++;
        LLOGI("LRX NODE #%lu from=!%08x name='%s' short='%s' len=%d hops=%u rssi=%d snr=%.2f",
              (unsigned long)s_nnode, (unsigned)from, e.long_name, e.short_name, n, hops, rssi, snr4 / 4.0);
        push_evt(&e);
    } else if (portnum == PORT_TEXT && pay && plen > 0) {
        if (tx_id_recent(id)) {
            LLOGI("LRX loopback (own TX id=0x%08lx) dropped", (unsigned long)id);
            return;
        }
        int c = plen;
        if (c > (int)sizeof(e.text) - 1) {
            c = (int)sizeof(e.text) - 3;
            while (c > 0 && ((uint8_t)pay[c] & 0xC0) == 0x80) c--;
        }
        memcpy(e.text, pay, (size_t)c);
        e.text[c] = 0;
        if (plen > c) strcat(e.text, "..");
        for (int k = 0; e.text[k]; k++)
            if (e.text[k] == '\n' || e.text[k] == '\r' || e.text[k] == '\t') e.text[k] = ' ';
        e.kind = LORA_EVT_TEXT;
        e.pkt_lo = (uint16_t)(id & 0xFFFF);
        s_ntext++;
        LLOGI("LRX TEXT #%lu from=!%08x len=%d hops=%u rssi=%d snr=%.2f: %s",
              (unsigned long)s_ntext, (unsigned)from, plen, hops, rssi, snr4 / 4.0, e.text);
        push_evt(&e);
    } else if (portnum == PORT_TIME && pay && plen >= 5) {
        e.kind = LORA_EVT_TIME;
        e.utc = (uint32_t)pay[0] | ((uint32_t)pay[1] << 8) | ((uint32_t)pay[2] << 16) |
                ((uint32_t)pay[3] << 24);
        e.tsrc = pay[4];
        s_ntext++;
        LLOGI("LRX TIME from=!%08x utc=%lu src=%u hops=%u rssi=%d", (unsigned)from,
              (unsigned long)e.utc, e.tsrc, hops, rssi);
        push_evt(&e);
    } else if (portnum == PORT_MIRROR && pay && plen >= 7) {
        s_nmirror++;
        if (s_mirror_cb) s_mirror_cb(pay, plen, rssi);
        if ((s_nmirror % 10) == 1)
            LLOGI("LRX MIRROR #%lu len=%d hops=%u rssi=%d", (unsigned long)s_nmirror,
                  plen, hops, rssi);
    } else {
        LLOGI("LRX other from=!%08x port=%lu len=%d hops=%u rssi=%d", (unsigned)from,
              (unsigned long)portnum, n, hops, rssi);
    }
}

/* 分块读 FIFO：本板 SPI 读通路裕量差，单次事务压到 56B 且两次一致才采信 */
static int rx_read_payload(uint8_t *dst, int len, uint8_t off)
{
    int got = 0;
    while (got < len) {
        int n = len - got;
        if (n > 56) n = 56;
        uint8_t prev[56], cur[56];
        int ok = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t t[3 + 56] = {0x1E, (uint8_t)(off + got), 0};
            uint8_t r[3 + 56] = {0};
            if (wait_busy_low(100) < 0) break;
            xfer(t, r, 3 + n);
            memcpy(cur, r + 3, (size_t)n);
            if (k > 0 && memcmp(prev, cur, (size_t)n) == 0) { ok = 1; break; }
            memcpy(prev, cur, (size_t)n);
            esp_rom_delay_us(300);
        }
        if (!ok) LLOGW("LRX FIFO 读取不稳定 off=%d n=%d", off + got, n);
        memcpy(dst + got, prev, (size_t)n);
        got += n;
    }
    return got;
}

static void rx_poll(void)
{
    uint16_t irq = 0;
    if (get_irq(&irq) < 0) return;

    if (irq & 0x0200) {                 /* RX timeout（连续模式下不应发生） */
        s_nto++;
        clear_irq(0xFFFF);
        rx_start();
        return;
    }
    if (irq & 0x0040) {                 /* CRC 错 */
        s_ncrc++;
        clear_irq(0xFFFF);
        rx_start();
        return;
    }
    if (!(irq & 0x0002)) return;        /* 还没收到 */

    uint8_t t13[6] = {0x13, 0, 0, 0, 0, 0}, r13[6] = {0};
    if (wait_busy_low(100) < 0) return;
    xfer(t13, r13, 6);
    uint8_t len = r13[2], off = r13[3];

    uint8_t t14[6] = {0x14, 0, 0, 0, 0, 0}, r14[6] = {0};
    if (wait_busy_low(100) < 0) return;
    xfer(t14, r14, 6);

    if (len == 0) {
        LLOGW("LRX 空包 irq=0x%04X", irq);
        clear_irq(0xFFFF);
        rx_start();
        return;
    }

    static uint8_t frame[280];
    int rl = rx_read_payload(frame, len, off);

    int8_t rssi = (int8_t)(-(int)r14[2] / 2);
    int8_t snr4 = (int8_t)r14[3];

    s_nrx++;
    LLOGI("LRX #%lu len=%d rssi=%d snr=%.2f off=%d irq=0x%04X", (unsigned long)s_nrx, len, rssi,
          snr4 / 4.0, off, irq);
#if LRX_HEXLOG
    dbg_hex("raw", frame, rl);
#endif

    decode(frame, rl, rssi, snr4);

    clear_irq(0xFFFF);
    rx_start();
}

static const char *find_ver(const uint8_t *d, int n)
{
    static char buf[24];
    for (int i = 0; i + 5 < n; i++) {
        if (memcmp(d + i, "SX126", 5) == 0) {
            int k = 0;
            while (k < 15 && i + k < n) { buf[k] = (char)d[i + k]; k++; }
            buf[k] = 0;
            return buf;
        }
    }
    return "(not found)";
}

/* ==================== 任务 ==================== */
static void lora_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));

    radio_setup(s_tcxo_v);
    int sync_ok = set_sync_word();
    uint8_t ptype = 0xFF;
    query(0x11, &ptype, 1);
    uint8_t ver[20] = {0};
    rd_reg(0x031D, ver, 20);
    LLOGI("radio boot: chip='%s' pkttype=%d sync=%s node=!%08x freq=433.000 SF%d/BW%d CR4-%d ldro=%d hash=0x%02X",
          find_ver(ver, 20), ptype, sync_ok ? "OK(24B4)" : "MISS", (unsigned)g_node,
          kPreset[s_preset].sf == 0x0C ? 12 : 11,
          kPreset[s_preset].bw == 0x04 ? 125 : (kPreset[s_preset].bw == 0x05 ? 250 : 500),
          kPreset[s_preset].cr == 0x01 ? 5 : (kPreset[s_preset].cr == 0x04 ? 8 : 6),
          kPreset[s_preset].ldro, CH_HASH);
    LLOGI("channel: name='%s' region=MY_433 override=433.000MHz preset=%s(%d/%d) psk=32B(8192bit) pkt=16B hdr+payload max=%d",
          "awa", kPreset[s_preset].name, s_preset, PRESET_N, 255);
    LLOGI("tx policy: %s dbm=%d", !s_tx_ok ? "DISABLED (no antenna) - RX only, PA=-9dBm, no NodeInfo"
          : (s_tx_dbm < TX_DBM ? "ENABLED conservative (antenna unconfirmed)" : "ENABLED full"),
          s_tx_ok ? s_tx_dbm : -9);

    rx_start();
    {
        uint8_t st = 0;
        query(0xC0, &st, 1);
        LLOGI("LRX started, chip mode=0x%X (4/5=FS/RX), RXEN=%d, CS=%d",
              (st >> 4) & 0x07, gpio_get_level(P_RXEN), gpio_get_level(P_CS));
    }

    /* 开机广播加随机抖动，避免多台设备同时上电时 NodeInfo 在空口互撞 */
    uint32_t boot_ms = BOOT_TX_MS + (esp_random() % 20000u);
    int64_t t_nodeinfo = 0;
    if (s_tx_ok) {
        LLOGI("boot nodeinfo in %lu ms (jitter)", (unsigned long)boot_ms);
        t_nodeinfo = esp_timer_get_time() + (int64_t)boot_ms * 1000;
    } else {
        LLOGI("boot nodeinfo skipped (TX disabled: no antenna)");
    }
    int64_t t_stat = esp_timer_get_time();
    while (1) {
        rx_poll();

        lora_txreq_t req;
        if (s_txq && xQueueReceive(s_txq, &req, 0) == pdTRUE) {
            int ok;
            if (req.kind == 1) {
                ok = tx_time(req.utc, req.tsrc);
                rx_start();
            } else {
                ok = tx_text(req.to, req.text, req.len);
                rx_start();
                lora_evt_t te;
                memset(&te, 0, sizeof(te));
                te.kind = LORA_EVT_TXDONE;
                te.tx_ok = ok ? 1 : 0;
                te.pkt_lo = req.tag;
                push_evt(&te);
            }
        }

        int64_t now = esp_timer_get_time();
        if (t_nodeinfo && now >= t_nodeinfo) {
            tx_nodeinfo();
            t_nodeinfo = now + (int64_t)(NODEINFO_MS + esp_random() % 60000u) * 1000;
            rx_start();
        }
        if (now - t_stat >= 30000000LL) {
            t_stat = now;
            uint8_t st = 0;
            query(0xC0, &st, 1);
            LLOGI("lora alive rx=%lu crc=%lu to=%lu hashmiss=%lu notforme=%lu dup=%lu node=%lu text=%lu mode=0x%X heap=%lu",
                  (unsigned long)s_nrx, (unsigned long)s_ncrc, (unsigned long)s_nto,
                  (unsigned long)s_nhash, (unsigned long)s_nother, (unsigned long)s_ndup,
                  (unsigned long)s_nnode,
                  (unsigned long)s_ntext, (unsigned)((st >> 4) & 0x07),
                  (unsigned long)esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* NodeInfo 能力位自检：编码一帧 User 再解回来，验证 gps 字段真的在包里
 * （验证时置 1，产线 0；不发射，纯本地回环） */
#define NODEINFO_SELFTEST 0
#if NODEINFO_SELFTEST
static void nodeinfo_selftest(void)
{
    uint8_t u[96];
    int un = build_user(u);
    lora_evt_t e;
    memset(&e, 0, sizeof(e));
    parse_user(u, un, &e);
    llog("SELFTEST user=%dB short='%s' gps=%u expect=%d %s",
         un, e.short_name, e.gps, C3MSG_GPS_STATE,
         e.gps == C3MSG_GPS_STATE ? "OK" : "FAIL");
}
#endif

void lora_init(uint32_t my_node)
{
    g_node = my_node;
    esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
    if (memcmp(s_mac, NO_ANTENNA_MAC, 6) == 0) s_tx_ok = 0;
    /* B 板（F3:31:D0）天线状态未确认：先 14dBm 保守运行，确认天线完好后再谈满功率 */
    {
        static const uint8_t CONSERVATIVE_MAC[6] = {0x50, 0x78, 0x7D, 0xF3, 0x31, 0xD0};
        if (memcmp(s_mac, CONSERVATIVE_MAC, 6) == 0) s_tx_dbm = TX_DBM_SAFE;
    }

#if NODEINFO_SELFTEST
    nodeinfo_selftest();
#endif

    s_q = xQueueCreate(8, sizeof(lora_evt_t));
    s_txq = xQueueCreate(4, sizeof(lora_txreq_t));

    spi_device_interface_config_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.clock_speed_hz = 500 * 1000;        /* 读通路裕量差，降速换取零位翻转 */
    dev.mode = 0;
    dev.spics_io_num = -1;                  /* 手动 CS（GPIO8），与 LCD 一样由代码控制 */
    dev.queue_size = 1;
    esp_err_t e = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (e != ESP_OK) {
        LLOGW("spi_bus_add_device 失败: %d，LoRa 不可用", e);
        return;
    }

    xTaskCreate(lora_task, "lora", 4096, NULL, 4, NULL);
}

bool lora_evt_pop(lora_evt_t *out)
{
    if (!s_q) return false;
    return xQueueReceive(s_q, out, 0) == pdTRUE;
}

bool lora_tx_allowed(void)
{
    return s_tx_ok != 0;
}

int lora_send_text(uint32_t to, const char *text)
{
    return lora_send_text_tagged(to, text, 0);
}

int lora_send_text_tagged(uint32_t to, const char *text, uint16_t tag)
{
    if (!s_tx_ok) {
        if (!s_tx_block_logged) {
            s_tx_block_logged = 1;
            LLOGW("TX disabled: no antenna — text TX blocked, RX only");
        }
        return -1;
    }
    if (!s_txq || !text) return -1;
    lora_txreq_t r;
    memset(&r, 0, sizeof(r));
    r.to = to;
    r.tag = tag;
    size_t n = strlen(text);
    if (n > sizeof(r.text) - 1) n = sizeof(r.text) - 1;
    r.len = (uint16_t)n;
    memcpy(r.text, text, n);
    if (xQueueSend(s_txq, &r, 0) != pdTRUE) return -2;
    return 0;
}

int lora_send_time(uint32_t utc, uint8_t src)
{
    if (!s_tx_ok) return -1;
    if (!s_txq || !utc) return -1;
    lora_txreq_t r;
    memset(&r, 0, sizeof(r));
    r.kind = 1;
    r.utc = utc;
    r.tsrc = src;
    if (xQueueSend(s_txq, &r, 0) != pdTRUE) return -2;
    return 0;
}

void lora_set_tx_power(int dbm)
{
    if (!s_tx_ok) {
        LLOGW("tx power change ignored: TX disabled (no antenna)");
        return;
    }
    if (dbm < -9) dbm = -9;
    if (dbm > 20) dbm = 20;
    s_tx_dbm = dbm;
    LLOGI("tx power set to %d dBm", dbm);
}

int lora_tx_power_dbm(void)
{
    return s_tx_ok ? s_tx_dbm : -9;
}

void lora_set_mirror_cb(lora_mirror_cb_t cb)
{
    s_mirror_cb = cb;
}

void lora_get_psk(uint8_t out[32])
{
    memcpy(out, PSK, 32);
}

const char *lora_channel_name(void)
{
    return "awa";
}

/* ==================== 调制档位 ==================== */
int lora_preset_count(void) { return PRESET_N; }

int lora_preset_idx(void) { return s_preset; }

const char *lora_preset_name(void) { return kPreset[s_preset].name; }

void lora_set_preset(int idx)
{
    if (idx < 0 || idx >= PRESET_N) idx = 0;
    if (idx == s_preset) return;
    s_preset = idx;
    /* 空口参数立即生效；接收侧重新开窗，发送队列不丢（参数在真正 SetTx 时才写入） */
    {
        uint8_t mod[4] = {kPreset[s_preset].sf, kPreset[s_preset].bw,
                          kPreset[s_preset].cr, kPreset[s_preset].ldro};
        cmd(0x8B, mod, 4);
    }
    set_packet_params(0xFF);      /* 先恢复最大包长，避免沿用上次文本长度 */
    rx_start();
    LLOGI("preset -> %s (SF%d/%dK CR4-%d ldro=%d), 预测 %d ms/100B",
          kPreset[s_preset].name,
          kPreset[s_preset].sf == 0x0C ? 12 : 11,
          kPreset[s_preset].bw == 0x04 ? 125 : 250,
          kPreset[s_preset].cr == 0x01 ? 5 : 8,
          kPreset[s_preset].ldro,
          kPreset[s_preset].ms_per_byte * 100);
}

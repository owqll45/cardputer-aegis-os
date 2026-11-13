/*
 * gomoku_c3 — ESP32-C3 五子棋监视器(状态同步版)
 * 协议: Cardputer 每 500ms 广播 GMKBEAC+mac → 本机回 GMKHI1 → 主机回 GMKWELC
 *       → 主机每次画面变化发 GMKSTATE(0x20) 全量状态包 238B, 本机本地重绘
 *       (棋盘/光标/比分/趋势/嘲讽文本全在包里, 丢包自愈)
 *       心跳 GMKBEAT 1s; GMKBYE 断开; 3 秒无包判掉线
 * 按键(GPIO9): 只用于设置菜单, 短按=打开/下一项, 双击=确认, 三击=返回
 * 屏幕: ST7789 横屏 320x240, SCK=10 MOSI=7 MISO=6 CS=1 DC=0 BL=13
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "u8g2font.h"
#include "xbox_ble.h"

static const char* TAG = "mon";

// ---------------- 系统标识 ----------------
#define OS_NAME    "AEGIS OS"
#define OS_NAME_CN "神盾"
#define OS_VERSION "v1.0"

// ---------------- 屏幕 ----------------
#define PIN_SCK  10
#define PIN_MOSI 7
#define PIN_MISO 6
#define PIN_CS   1
#define PIN_DC   0
#define PIN_BL   13
#define PIN_BTN  9

#define LCD_W 320
#define LCD_H 240

static spi_device_handle_t s_spi;

// 颜色: 帧缓冲用 8bit 调色板索引(省一半 RAM 给蓝牙), 调色板里放真实 565
typedef uint8_t color_t;
enum {
    CI_BLACK, CI_WHITE, CI_RED, CI_GREEN, CI_BLUE, CI_YELLOW, CI_CYAN,
    CI_GRAY, CI_DGRAY, CI_LGRAY, CI_ORANGE, CI_MAGENTA, CI_BG, CI_WOOD,
    CI_PANEL, CI_TREND, CI_TMID, CI_SELBG,
    CI_DGREEN, CI_BRICK, CI_STEEL,
};
static const uint16_t kPal[256] = {
    [CI_BLACK]   = 0x0000,
    [CI_WHITE]   = 0xFFFF,
    [CI_RED]     = 0xF800,
    [CI_GREEN]   = 0x07E0,
    [CI_BLUE]    = 0x001F,
    [CI_YELLOW]  = 0xFFE0,
    [CI_CYAN]    = 0x07FF,
    [CI_GRAY]    = 0x7BEF,
    [CI_DGRAY]   = 0x39E7,
    [CI_LGRAY]   = 0xC618,
    [CI_ORANGE]  = 0xFC00,
    [CI_MAGENTA] = 0xF81F,
    [CI_BG]      = 0x3186,
    [CI_WOOD]    = 0xE79C,
    [CI_PANEL]   = 0x1082,
    [CI_TREND]   = 0x4208,
    [CI_TMID]    = 0x18E3,
    [CI_SELBG]   = 0x2104,
    [CI_DGREEN]  = 0x0540,  // 黑白棋绿底
    [CI_BRICK]   = 0xC904,  // 坦克砖墙
    [CI_STEEL]   = 0x8C51,  // 坦克钢墙
};

#define C_BLACK   CI_BLACK
#define C_WHITE   CI_WHITE
#define C_RED     CI_RED
#define C_GREEN   CI_GREEN
#define C_BLUE    CI_BLUE
#define C_YELLOW  CI_YELLOW
#define C_CYAN    CI_CYAN
#define C_GRAY    CI_GRAY
#define C_DGRAY   CI_DGRAY
#define C_LGRAY   CI_LGRAY
#define C_ORANGE  CI_ORANGE
#define C_MAGENTA CI_MAGENTA
#define C_BG      CI_BG     // Cardputer THEME_COLOR_BG 0x333333 → 565
#define C_WOOD    CI_WOOD   // 棋盘木色
#define C_PANEL   CI_PANEL  // 战报底
#define C_TREND   CI_TREND  // 趋势图边框
#define C_TMID    CI_TMID   // 趋势图中线
#define C_SELBG   CI_SELBG  // 菜单选中底
#define C_DGREEN  CI_DGREEN // 黑白棋绿底
#define C_BRICK   CI_BRICK  // 砖墙
#define C_STEEL   CI_STEEL  // 钢墙

static inline uint16_t sw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static void lcd_write(const uint8_t* data, size_t len, int is_data)
{
    if (len == 0) return;
    gpio_set_level(PIN_DC, is_data ? 1 : 0);
    gpio_set_level(PIN_CS, 0);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(PIN_CS, 1);
}
static void lcd_cmd(uint8_t cmd) { lcd_write(&cmd, 1, 0); }
static void lcd_data1(uint8_t d) { lcd_write(&d, 1, 1); }

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t d[4];
    lcd_cmd(0x2A);
    d[0] = x0 >> 8; d[1] = x0; d[2] = x1 >> 8; d[3] = x1;
    lcd_write(d, 4, 1);
    lcd_cmd(0x2B);
    d[0] = y0 >> 8; d[1] = y0; d[2] = y1 >> 8; d[3] = y1;
    lcd_write(d, 4, 1);
    lcd_cmd(0x2C);
}

static void st7789_init(void)
{
    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x3A); lcd_data1(0x55);
    lcd_cmd(0x36); lcd_data1(0x60); // 横屏 MX|MV
    lcd_cmd(0x20);                  // 反色关(此屏开反色会全反)
    lcd_cmd(0x13);
    lcd_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ---------------- 中文字体 ----------------
extern const uint8_t lgfx_efont_cn_10_b[];
extern const uint8_t lgfx_efont_cn_12[];
extern const uint8_t lgfx_efont_cn_14[];
#define F10B lgfx_efont_cn_10_b
#define F12  lgfx_efont_cn_12
#define F14  lgfx_efont_cn_14

// ---------------- 内存帧缓冲(320x240x1 = 75KB 索引色, 画完一次推屏, 不闪) ----------------
static uint8_t s_scr[LCD_W * LCD_H];
static uint16_t s_push[LCD_W * 4];

static void fb_px(int x, int y, color_t c)
{
    if ((unsigned)x < LCD_W && (unsigned)y < LCD_H) s_scr[y * LCD_W + x] = c;
}

static void fb_fill(int x, int y, int w, int h, color_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint8_t* row = s_scr + (y + j) * LCD_W + x;
        memset(row, c, w);
    }
}

static void fb_hline(int x, int y, int n, color_t c) { fb_fill(x, y, n, 1, c); }
static void fb_vline(int x, int y, int n, color_t c) { fb_fill(x, y, 1, n, c); }

static void fb_rect(int x, int y, int w, int h, color_t c)
{
    fb_hline(x, y, w, c);
    fb_hline(x, y + h - 1, w, c);
    fb_vline(x, y, h, c);
    fb_vline(x + w - 1, y, h, c);
}

static void fb_line(int x0, int y0, int x1, int y1, color_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        fb_px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fb_disc(int cx, int cy, int r, color_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int half = (int)sqrtf((float)(r * r - dy * dy));
        fb_hline(cx - half, cy + dy, 2 * half + 1, c);
    }
}

static void fb_circle(int cx, int cy, int r, color_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        fb_px(cx + x, cy + y, c); fb_px(cx - x, cy + y, c);
        fb_px(cx + x, cy - y, c); fb_px(cx - x, cy - y, c);
        fb_px(cx + y, cy + x, c); fb_px(cx - y, cy + x, c);
        fb_px(cx + y, cy - x, c); fb_px(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static uint8_t s_gbuf[20 * 20];
uint32_t g_push_cnt = 0;
uint32_t g_push_err = 0;
uint32_t g_render_cnt = 0;

// 透明背景画一个 glyph, 返回步进
static int fb_glyph(const uint8_t* font, int x, int ytop, uint32_t cp, color_t fg)
{
    u8g2_glyph_t g;
    if (u8g2_decode(font, cp, &g, s_gbuf) != 0) return 0;
    if (g.w > 20 || g.h > 20) return g.xadv; // 超出静态缓冲, 跳过
    int base = u8g2_font_baseline(font);
    int gx = x + g.xoff;
    int gy = ytop + base - g.yoff - g.h;
    for (int j = 0; j < g.h; j++)
        for (int i = 0; i < g.w; i++)
            if (s_gbuf[j * g.w + i]) fb_px(gx + i, gy + j, fg);
    return g.xadv;
}

static int fb_text(const uint8_t* font, int x, int y, const char* s, color_t fg)
{
    while (*s) {
        uint32_t cp = u8g2_utf8_next(&s);
        x += fb_glyph(font, x, y, cp, fg);
    }
    return x;
}

// 在区域 [x0, x0+w) 内水平居中
static void fb_text_cx(int x0, int w, int y, const char* s, const uint8_t* font, color_t fg)
{
    int tw = u8g2_str_width(font, s);
    int x = x0 + (w - tw) / 2;
    if (x < x0) x = x0;
    fb_text(font, x, y, s, fg);
}

static void push_screen(void)
{
    extern uint32_t g_push_cnt;
    extern uint32_t g_push_err;
    g_push_cnt++;
    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 0);
    for (int y = 0; y < LCD_H; y += 4) {
        const uint8_t* src = s_scr + y * LCD_W;
        for (int i = 0; i < LCD_W * 4; i++) s_push[i] = sw(kPal[src[i]]);
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = LCD_W * 4 * 16;
        t.tx_buffer = s_push;
        esp_err_t e = spi_device_polling_transmit(s_spi, &t);
        if (e != ESP_OK) g_push_err++;
    }
    gpio_set_level(PIN_CS, 1);
}

// ================= 电量 ADC 自动探测 =================
static uint32_t now_ms(void);
// C3 自带电池但分压引脚未知: 扫 GPIO1..4, 找落在锂电池 1:2 分压区间的那个
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static int s_batt_gpio = -2;   // -2=未探测 -1=未找到 >=0 引脚
static int s_batt_pct = 255;   // 本机电量
static uint8_t s_host_pct = 255; // Cardputer 电量(状态包带过来)
static uint8_t s_net_flags = 0;  // 联机对战: bit0联机中 bit1键盘准备 bit2手柄准备 bit3已开局
static uint8_t s_cur_player = 1; // 当前该谁下 1=黑 2=白
static int s_cheat_mine = 0;     // 手柄方(本机)作弊辅助开关, 手柄 Y 键切换

static int adc_read_mv(int gpio)
{
    int raw = 0, mv = 0;
    if (!s_adc) return -1;
    if (adc_oneshot_read(s_adc, (adc_channel_t)gpio, &raw) != ESP_OK) return -1;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    return raw * 3100 / 4095; // 无校准表时按线性估算
}

static int mv_to_pct(int mv)
{
    if (mv <= 3000) return 0;
    if (mv >= 4200) return 100;
    if (mv <= 3700) return (mv - 3000) * 45 / 700;
    return 45 + (mv - 3700) * 55 / 500;
}

static void batt_init(void)
{
    adc_oneshot_unit_init_cfg_t u = { 0 };
    u.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "adc init fail");
        s_adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t c = { 0 };
    c.atten = ADC_ATTEN_DB_12;
    c.bitwidth = ADC_BITWIDTH_12;
    // 只配 GPIO2..4: GPIO0/1 是 LCD 的 DC/CS, 绝不能碰
    for (int g = 2; g <= 4; g++) adc_oneshot_config_channel(s_adc, (adc_channel_t)g, &c);

    // 校准表: 让 mV 是真实电压(C3 的 ADC 线性估算会偏高)
    adc_cali_curve_fitting_config_t cal = { 0 };
    cal.unit_id = ADC_UNIT_1;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc cali fail, 用线性估算");
        s_cali = NULL;
    }

    int best = -1, best_mv = 0, best_span = 99999;
    for (int g = 2; g <= 4; g++) { // 跳过 GPIO0(DC) / GPIO1(CS)
        int mn = 99999, mx = 0, sum = 0;
        for (int i = 0; i < 8; i++) {
            int mv = adc_read_mv(g);
            if (mv < 0) mv = 0;
            if (mv < mn) mn = mv;
            if (mv > mx) mx = mv;
            sum += mv;
        }
        int avg = sum / 8, span = mx - mn;
        ESP_LOGI(TAG, "adc gpio%d avg=%dmV span=%dmV", g, avg, span);
        // 电池经分压: 引脚约 1.5~2.1V; 放宽到 1.4~2.25V 且要求读数稳定
        if (avg >= 1400 && avg <= 2250 && span < 120 && span < best_span) {
            best = g;
            best_mv = avg;
            best_span = span;
        }
    }
    s_batt_gpio = best;
    if (best >= 0) s_batt_pct = (uint8_t)mv_to_pct(best_mv * 2);
    ESP_LOGI(TAG, "battery gpio=%d pin=%dmV pct=%d", best, best_mv, s_batt_pct);
}

static void batt_update(void)
{
    if (s_batt_gpio < 0 || !s_adc) return;
    int sum = 0, n = 0;
    for (int i = 0; i < 6; i++) {
        int mv = adc_read_mv(s_batt_gpio);
        if (mv >= 0) { sum += mv; n++; }
    }
    if (!n) return;
    s_batt_pct = (uint8_t)mv_to_pct((sum / n) * 2);
}

// ================= 顶部彩色提示(队列, 逐条连贯显示) =================
#define TOAST_Q 4
typedef struct {
    char txt[40];
    color_t col;
    uint32_t dur; // 0 = 常驻直到被替换
} toast_item_t;

static toast_item_t s_tq[TOAST_Q];
static int s_tq_n = 0, s_tq_i = 0;
static uint32_t s_tq_at = 0;

static void toast_clear(void) { s_tq_n = 0; s_tq_i = 0; }

static void toast_push(const char* t, color_t col, uint32_t dur)
{
    if (s_tq_n >= TOAST_Q) { // 满了丢最旧
        for (int i = 1; i < s_tq_n; i++) s_tq[i - 1] = s_tq[i];
        s_tq_n--;
        if (s_tq_i > 0) s_tq_i--;
    }
    toast_item_t* it = &s_tq[s_tq_n++];
    strncpy(it->txt, t, sizeof(it->txt) - 1);
    it->txt[sizeof(it->txt) - 1] = 0;
    it->col = col;
    it->dur = dur;
    if (s_tq_n == 1) s_tq_at = now_ms();
}

static void toast_set(const char* t, color_t col, uint32_t dur)
{
    toast_clear();
    toast_push(t, col, dur);
}

static bool toast_active(void) { return s_tq_n > 0 && s_tq_i < s_tq_n; }

static bool toast_tick(void)
{
    if (!toast_active()) return false;
    toast_item_t* it = &s_tq[s_tq_i];
    if (it->dur && now_ms() - s_tq_at >= it->dur) {
        s_tq_i++;
        s_tq_at = now_ms();
        return true;
    }
    return false;
}

static void toast_draw(void)
{
    if (!toast_active()) return;
    toast_item_t* it = &s_tq[s_tq_i];
    int tw = u8g2_str_width(F12, it->txt) + 20;
    if (tw > LCD_W - 8) tw = LCD_W - 8;
    int bx = (LCD_W - tw) / 2, by = 4;
    fb_fill(bx, by, tw, 18, C_BLACK);
    fb_rect(bx, by, tw, 18, it->col);
    fb_text_cx(bx, tw, by + 2, it->txt, F12, it->col);
}

// ================= 电量图标 =================
static void draw_batt_icon(int x, int y, color_t col, int pct)
{
    const int w = 16, h = 9;
    fb_rect(x, y, w, h, col);
    fb_fill(x + w, y + 3, 2, 3, col);
    if (pct < 0) return;
    int bars = (pct * (w - 4) + 99) / 100;
    if (bars < 1) bars = 1;
    fb_fill(x + 2, y + 2, bars, h - 4, col);
}

static void draw_batt_row(void)
{
    // 主机(Cardputer)=绿色, 本机=白色, 两个贴在一起, 不显示百分比
    const int y = 222;
    draw_batt_icon(274, y, C_GREEN, s_host_pct == 255 ? -1 : s_host_pct);
    draw_batt_icon(292, y, C_WHITE, s_batt_pct == 255 ? -1 : s_batt_pct);
}

// ================= 信号强度 / 丢包 =================
static int s_rssi = -127;      // 对端信号 dBm
static int s_loss_pct = 0;     // 丢包率

static int rssi_bars(int v)
{
    if (v >= -60) return 4;
    if (v >= -68) return 3;
    if (v >= -76) return 2;
    if (v >= -88) return 1;
    return 0;
}

// 右下角: 4 格信号条 + dBm (靠最右边)
static void draw_signal_right(int xr, int y)
{
    int bars = rssi_bars(s_rssi);
    color_t c = bars >= 3 ? C_GREEN : (bars == 2 ? C_YELLOW : (bars == 1 ? C_ORANGE : C_RED));
    char buf[28];
    if (s_rssi <= -127) strcpy(buf, "--");
    else snprintf(buf, sizeof(buf), "%ddBm", s_rssi);
    int tw = u8g2_str_width(F10B, buf);
    int bw = 22;
    int x = xr - tw - bw - 4;      // 文字右端贴 xr
    for (int i = 0; i < 4; i++) {
        int h = 3 + i * 2;
        fb_fill(x + i * 6, y + (9 - h), 4, h, i < bars ? c : C_DGRAY);
    }
    fb_text(F10B, x + bw + 4, y, buf, s_rssi <= -127 ? C_DGRAY : c);
}

// 底部信息带(棋盘下方): 胜率条 + 事件日志
static char s_evt[40] = "等待对局...";
static uint32_t s_evt_ms = 0;

static void push_event(const char* txt)
{
    snprintf(s_evt, sizeof(s_evt), "%s", txt);
    s_evt_ms = now_ms();
}

// 右下角附加信息: 只保留丢包(延迟/性能已按用户要求移除)
static void draw_link_stats(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "丢%d%%", s_loss_pct);
    fb_text(F10B, 238, 222, buf, s_loss_pct > 10 ? C_RED : C_LGRAY);
}

// ================= ESP-NOW 客户端 + 状态包 =================
#define GMK_CH 1
#define GMK_STATE_LEN 240

// flags0 位
#define F0_OVER   1
#define F0_ANIM   2
#define F0_TAUNT  4
#define F0_PANEL  8
#define F0_THREAT 16
#define F0_REPLAY 32
#define F0_HELP   64
#define F0_RESULT 128
// flags1 位
#define F1_PVP    1
#define F1_ASSIST 2
#define F1_BLITZ  4
#define F1_RCONF  8
#define F1_AIMODE 16
#define F1_CHEAT  32   // 键盘方(对手)开启作弊辅助 → 本机顶部滚动横幅
#define F1_TAUNT_REMOTE 128 // 当前嘲讽是本机(手柄方)自己发的, 不显示在自己屏幕上

typedef struct {
    uint8_t flags0, flags1, winner, cur_player, difficulty, persona, mood;
    uint8_t cursor_x, cursor_y, move_count, last_r, last_c;
    uint8_t hint_r, hint_c;   // PVP 时复用 replay_idx/total 两个字节, 作弊提示点(255=无)
    uint8_t replay_idx, replay_total;
    uint8_t pw, aw, streak, best;
    uint8_t danger, cheat, blunders, min_danger, worst_no;
    uint16_t achv, achv_new;
    uint8_t clk1, clk2;
    uint16_t game_sec;
    int32_t score1, score2;
    uint8_t trend_count, trend_head;
    int8_t trend[24];
    uint8_t taunt_kind;
    char taunt[3][18];
    uint8_t threat[15][15];
    uint8_t board[15][15];
} gmk_state_t;

static gmk_state_t s_gs;
static volatile int s_gs_ready = 0;
static int s_have_state = 0;
static uint32_t s_anim_ms = 0;      // 胜利动画本地计时起点
static uint32_t s_last_render = 0;

static uint8_t s_host[6];
static volatile int s_linked = 0;
static uint32_t s_last_pkt_ms = 0;
static uint32_t s_hello_last = 0;

// ---- 多游戏镜像: 当前 app / 游戏画面切换(GMKAPP 0x10) ----
#define GID_LAUNCHER 0
#define GID_GOMOKU   1
#define GID_PONG     2
#define GID_REVERSI  3
#define GID_TANK     4
#define GID_UNO      5
#define GID_CONNECT4 6
#define GID_CHECKERS 7
#define GID_TETRIS   8
#define GID_SNAKE    9
#define GID_BREAKOUT 10
#define GID_2048     11
#define GID_MINESWEEPER 12
#define GID_FLAPPY   13
#define GID_INVADERS 14
#define GID_RACE     15
#define GID_FPS      16
#define GID_SHOGI    17
static uint8_t s_game_id = GID_GOMOKU;  // 默认五子棋, 兼容不发 GMKAPP 的旧主机
static uint8_t s_icon_id = 0;
static char s_app_name[17] = "Gomoku";

// Pong 镜像状态(PONGSTATE 0x21, 20B)
typedef struct {
    int16_t ball_x, ball_y;
    uint8_t p1y, p2y, score1, score2, flags, winner;
} pong_state_t;
static pong_state_t s_pong;
static int s_pong_have = 0;

// Reversi 镜像状态(RVSTATE 0x22, 29B)
typedef struct {
    uint8_t board[64];  // 解码后: 0空 1黑 2白
    uint8_t cx, cy, black, white, flags, cur, winner, last_r, last_c;
} reversi_state_t;
static reversi_state_t s_rv;
static int s_reversi_have = 0;

// Tank 镜像状态(TANKSTATE 0x23, 59B) — 场区坐标与 Cardputer 一致(8px/格, 104x104)
typedef struct {
    uint8_t t[2][4];      // x,y,dir,alive
    uint8_t nbullet;
    uint8_t bullet[6][3]; // x,y,dir
    uint8_t brick[22];    // 169bit
    uint8_t lives[2], kills[2], flags, winner;
} tank_state_t;
static tank_state_t s_tk;
static int s_tank_have = 0;

// UNO 镜像状态(UNOSTATE 0x24, 64B)
typedef struct {
    uint8_t flags, cur, dir, color, top, deck, pending;
    uint8_t hn[2];
    uint8_t hand[2][16];
    uint8_t sel, choosing, color_sel, round, match_end;
    uint8_t score[2], winner;
} uno_state_t;
static uno_state_t s_uno;
static int s_uno_have = 0;

// Connect4 镜像状态(CFSTATE 0x25, 27B)
typedef struct {
    uint8_t board[42];  // 0空 1红 2黄; idx=r*7+c, r=0为顶行
    uint8_t sel, flags, cur, winner, win_idx, win_dir, last_c, last_r;
    uint8_t score[2], hint_col, moves;
} c4_state_t;
static c4_state_t s_c4;
static int s_c4_have = 0;

// Checkers 镜像状态(CKSTATE 0x26, 38B)
typedef struct {
    uint8_t board[64];  // 0空 1红兵 2红王 3白兵 4白王; idx=r*8+c
    uint8_t cursor, sel, locked, flags, cur, winner, last_from, last_to;
    uint8_t score[2], hint_from, hint_to, quiet, moves;
    uint8_t dest_mask[8];  // 选中子的合法落点 64bit
} ck_state_t;
static ck_state_t s_ck;
static int s_ck_have = 0;

// ---- 10 款新游戏镜像状态 ----

// 2048 (TYPE 0x27)
typedef struct {
    uint8_t flags;      // bit0=game_over bit1=win bit2=cannot_move
    uint16_t score;
    uint8_t grid[9];    // 4×4 每格 4bit 编码 (0=空, 1..11=2^1..2^11)
} s2048_state_t;
static s2048_state_t s2048;
static int s2048_have = 0;

// Tetris (TYPE 0x28)
typedef struct {
    uint8_t board[100];         // 10 列 × 20 行, 每格 4bit 颜色 (0=空)
    uint8_t cur_x, cur_y, cur_type, cur_color;
    uint16_t score;
    uint8_t flags;              // bit0=game_over
} tetris_state_t;
static tetris_state_t s_tetris;
static int s_tetris_have = 0;

// Snake (TYPE 0x29)
typedef struct {
    uint8_t food_x, food_y;
    uint8_t dir;                // 0上 1右 2下 3左
    uint16_t score;
    uint8_t flags;              // bit0=game_over
    uint8_t grid_mask[39];     // 24×13=312bit, bit=1 表示有蛇身
} snake_state_t;
static snake_state_t s_snake;
static int s_snake_have = 0;

// Breakout (TYPE 0x2A)
typedef struct {
    uint16_t paddleX;           // Q8.8 定点
    uint16_t ballX, ballY;      // Q8.8
    int8_t ballDX, ballDY;      // Q7 有符号
    uint16_t score;
    uint8_t lives, flags;       // bit0=game_over bit1=running
    uint8_t brick_mask[6];      // 48bit, 1=活跃砖
} breakout_state_t;
static breakout_state_t s_breakout;
static int s_breakout_have = 0;

// Minesweeper (TYPE 0x2B) — 最大 228B
typedef struct {
    uint8_t grid[220];          // 20×11, 每格: 0=隐藏 1-8=数字 9=地雷 10=旗标
    uint8_t cursorX, cursorY, mines_count, flags;  // bit0=game_over bit1=won
} minesweeper_state_t;
static minesweeper_state_t s_ms;
static int s_ms_have = 0;

// Flappy (TYPE 0x2C)
typedef struct {
    uint16_t birdY;             // Q8.8
    int8_t birdV;               // signed Q7
    uint8_t pipe_count;
    struct { uint16_t x; int8_t gapY; uint8_t passed; } pipes[4];
    uint16_t score;
    uint8_t flags;              // bit0=game_over
} flappy_state_t;
static flappy_state_t s_flappy;
static int s_flappy_have = 0;

// Invaders (TYPE 0x2D, 32B)
typedef struct {
    uint8_t alive_mask[7];      // row-major, bit=alive
    uint16_t playerX;           // Q8.8
    int16_t alien_baseX;        // Q8.8, 第一个 alive alien 的当前 x
    int16_t alien_rowY;         // Q8.8, 第一行 alien 的当前 y
    int8_t alien_dir;           // +1 右, -1 左
    uint8_t bullet_count;       // 玩家子弹数
    uint8_t bullets[5][2];      // bullets[i] = {x, y}, uint8 0-255
    uint16_t score;
    uint8_t flags;              // bit0=game_over
} invaders_state_t;
static invaders_state_t s_inv;
static int s_inv_have = 0;

// Race (TYPE 0x2E)
typedef struct {
    uint16_t playerX;           // Q10.6
    uint8_t playerZ[3];         // 24bit
    uint16_t speed;
    uint8_t gear;
    uint8_t laps;
    uint16_t seg_count, cur_seg, cur_curve;
    uint8_t flags[2];           // bit0=game_over bit1=finished
} race_state_t;
static race_state_t s_race;
static int s_race_have = 0;

// FPS (TYPE 0x2F) — 特殊 raycast 监控画面
typedef struct {
    float posX, posY, dirX, dirY, planeX, planeY;
    uint8_t health, weapon_frame, level, flags;  // bit0=game_over
    uint8_t enemy_count;
    float enemy_x[8], enemy_y[8];
    uint8_t enemy_health[8], enemy_alive[8], enemy_state[8];
} fps_state_t;
static fps_state_t s_fps;
static int s_fps_have = 0;

// Shogi (TYPE 0x30)
typedef struct {
    uint8_t board[81];          // 0=空; makePiece(owner,kind,promoted)
    uint8_t hand[2][8];         // 持驹数量
    uint8_t turn, cursor_r, cursor_c, sel_r, sel_c;
    uint8_t game_over, winner;  // winner: 0=无 1=玩家 2=AI
    uint16_t move_count;
} shogi_state_t;
static shogi_state_t s_shogi;
static int s_shogi_have = 0;

// 钢墙布局(Cardputer app_tank.cpp isSteel 同一份, 必须保持一致)
static int tank_steel(int r, int c)
{
    if (r <= 0 || r >= 12 || c <= 0 || c >= 12) return 1;
    if (r == 4 && (c == 2 || c == 3 || c == 9 || c == 10)) return 1;
    if (r == 8 && (c == 2 || c == 3 || c == 9 || c == 10)) return 1;
    if (c == 6 && (r == 2 || r == 3 || r == 9 || r == 10)) return 1;
    return 0;
}

static int tank_brick(const uint8_t* mask, int r, int c)
{
    int idx = r * 13 + c;
    return (mask[idx / 8] >> (idx % 8)) & 1;
}

static void parse_tank(const uint8_t* d)
{
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 4; j++) s_tk.t[i][j] = d[4 + i * 4 + j];
    s_tk.nbullet = d[12];
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 3; j++) s_tk.bullet[i][j] = d[13 + i * 3 + j];
    memcpy(s_tk.brick, d + 31, 22);
    s_tk.lives[0] = d[53]; s_tk.lives[1] = d[54];
    s_tk.kills[0] = d[55]; s_tk.kills[1] = d[56];
    s_tk.flags = d[57];
    s_tk.winner = d[58];
    s_tank_have = 1;
    s_gs_ready = 1;
}

// 当前画面是否有数据可渲染
static int view_ready(void)
{
    if (s_game_id == GID_GOMOKU) return s_have_state;
    if (s_game_id == GID_PONG) return s_pong_have;
    if (s_game_id == GID_REVERSI) return s_reversi_have;
    if (s_game_id == GID_TANK) return s_tank_have;
    if (s_game_id == GID_UNO) return s_uno_have;
    if (s_game_id == GID_CONNECT4) return s_c4_have;
    if (s_game_id == GID_CHECKERS) return s_ck_have;
    if (s_game_id == GID_2048) return s2048_have;
    if (s_game_id == GID_TETRIS) return s_tetris_have;
    if (s_game_id == GID_SNAKE) return s_snake_have;
    if (s_game_id == GID_BREAKOUT) return s_breakout_have;
    if (s_game_id == GID_MINESWEEPER) return s_ms_have;
    if (s_game_id == GID_FLAPPY) return s_flappy_have;
    if (s_game_id == GID_INVADERS) return s_inv_have;
    if (s_game_id == GID_RACE) return s_race_have;
    if (s_game_id == GID_FPS) return s_fps_have;
    if (s_game_id == GID_SHOGI) return s_shogi_have;
    return 1; // Launcher 等: 收到 GMKAPP 就能画
}

static void parse_uno(const uint8_t* d)
{
    s_uno.flags = d[4];
    s_uno.cur = d[5];
    s_uno.dir = d[6];
    s_uno.color = d[7];
    s_uno.top = d[8];
    s_uno.deck = d[9];
    s_uno.pending = d[10];
    s_uno.hn[0] = d[11] > 16 ? 16 : d[11];
    s_uno.hn[1] = d[12] > 16 ? 16 : d[12];
    memcpy(s_uno.hand[0], d + 13, 16);
    memcpy(s_uno.hand[1], d + 29, 16);
    s_uno.sel = d[45];
    s_uno.choosing = d[46];
    s_uno.color_sel = d[47];
    s_uno.round = d[48];
    s_uno.match_end = d[49];
    s_uno.score[0] = d[50];
    s_uno.score[1] = d[51];
    s_uno.winner = d[52];
    s_uno_have = 1;
    s_gs_ready = 1;
}

static void parse_reversi(const uint8_t* d)
{
    for (int i = 0; i < 64; i++)
        s_rv.board[i] = (d[4 + i / 4] >> ((i % 4) * 2)) & 3;
    s_rv.cx = d[20]; s_rv.cy = d[21];
    s_rv.black = d[22]; s_rv.white = d[23];
    s_rv.flags = d[24]; s_rv.cur = d[25]; s_rv.winner = d[26];
    s_rv.last_r = d[27]; s_rv.last_c = d[28];
    s_reversi_have = 1;
    s_gs_ready = 1;
}

static void parse_connect4(const uint8_t* d)
{
    for (int i = 0; i < 42; i++)
        s_c4.board[i] = (d[4 + i / 4] >> ((i % 4) * 2)) & 3;
    s_c4.sel = d[15];
    s_c4.flags = d[16];
    s_c4.cur = d[17];
    s_c4.winner = d[18];
    s_c4.win_idx = d[19];
    s_c4.win_dir = d[20];
    s_c4.last_c = d[21];
    s_c4.last_r = d[22];
    s_c4.score[0] = d[23]; s_c4.score[1] = d[24];
    s_c4.hint_col = d[25];
    s_c4.moves = d[26];
    s_c4_have = 1;
    s_gs_ready = 1;
}

// 从 12 字节里取第 i 个 3bit 字段(棋盘压缩)
static int ck_get3(const uint8_t* p, int i)
{
    int off = i * 3;
    int v = 0;
    for (int k = 0; k < 3; k++)
        v |= ((p[(off + k) / 8] >> ((off + k) % 8)) & 1) << k;
    return v;
}

static void parse_checkers(const uint8_t* d)
{
    int pi = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (!((r + c) & 1)) { s_ck.board[r * 8 + c] = 0; continue; }
            s_ck.board[r * 8 + c] = (uint8_t)ck_get3(d + 4, pi++);
        }
    }
    s_ck.cursor = d[16];
    s_ck.sel = d[17];
    s_ck.locked = d[18];
    s_ck.flags = d[19];
    s_ck.cur = d[20];
    s_ck.winner = d[21];
    s_ck.last_from = d[22];
    s_ck.last_to = d[23];
    s_ck.score[0] = d[24]; s_ck.score[1] = d[25];
    s_ck.hint_from = d[26];
    s_ck.hint_to = d[27];
    s_ck.quiet = d[28];
    s_ck.moves = d[29];
    memcpy(s_ck.dest_mask, d + 30, 8);
    s_ck_have = 1;
    s_gs_ready = 1;
}

static void parse_pong(const uint8_t* d)
{
    s_pong.ball_x = (int16_t)(d[4] | (d[5] << 8));
    s_pong.ball_y = (int16_t)(d[6] | (d[7] << 8));
    s_pong.p1y = d[8]; s_pong.p2y = d[9];
    s_pong.score1 = d[10]; s_pong.score2 = d[11];
    s_pong.flags = d[12]; s_pong.winner = d[13];
    s_pong_have = 1;
    s_gs_ready = 1;
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void add_peer(const uint8_t* mac)
{
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, mac, 6);
    p.channel = GMK_CH;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_del_peer(mac);
    esp_now_add_peer(&p);
}

static void parse_state(const uint8_t* d)
{
    gmk_state_t* g = &s_gs;
    int prev_anim = g->flags0 & F0_ANIM;
    uint8_t prev_mc = g->move_count, prev_cheat = g->cheat, prev_over = g->flags0 & F0_OVER;
    int prev_taunt = g->flags0 & F0_TAUNT;
    g->flags0 = d[4]; g->flags1 = d[5]; g->winner = d[6]; g->cur_player = d[7];
    g->difficulty = d[8]; g->persona = d[9]; g->mood = d[10];
    g->cursor_x = d[11]; g->cursor_y = d[12]; g->move_count = d[13];
    g->last_r = d[14]; g->last_c = d[15]; g->replay_idx = d[16]; g->replay_total = d[17];
    // PVP 时 d[16]/d[17] 被主机复用成作弊提示点坐标(255=无)
    g->hint_r = (g->flags1 & F1_PVP) ? d[16] : 255;
    g->hint_c = (g->flags1 & F1_PVP) ? d[17] : 255;
    g->pw = d[18]; g->aw = d[19]; g->streak = d[20]; g->best = d[21];
    g->danger = d[22]; g->cheat = d[23]; g->blunders = d[24];
    g->min_danger = d[25]; g->worst_no = d[26];
    g->achv = (uint16_t)(d[27] | (d[28] << 8));
    g->achv_new = (uint16_t)(d[29] | (d[30] << 8));
    g->clk1 = d[31]; g->clk2 = d[32];
    g->game_sec = (uint16_t)(d[33] | (d[34] << 8));
    memcpy(&g->score1, d + 35, 4);
    memcpy(&g->score2, d + 39, 4);
    g->trend_count = d[43]; g->trend_head = d[44];
    memcpy(g->trend, d + 45, 24);
    g->taunt_kind = d[69];
    memcpy(g->taunt, d + 70, 54);
    g->taunt[0][17] = g->taunt[1][17] = g->taunt[2][17] = 0;
    for (int i = 0; i < 57; i++) {
        uint8_t v = d[124 + i];
        for (int k = 0; k < 4; k++) {
            int idx = i * 4 + k;
            if (idx >= 225) break;
            g->threat[idx / 15][idx % 15] = (v >> (k * 2)) & 3;
        }
    }
    for (int i = 0; i < 57; i++) {
        uint8_t v = d[181 + i];
        for (int k = 0; k < 4; k++) {
            int idx = i * 4 + k;
            if (idx >= 225) break;
            g->board[idx / 15][idx % 15] = (v >> (k * 2)) & 3;
        }
    }
    s_host_pct = d[238];
    s_net_flags = d[239];
    s_cur_player = d[7];
    // 胜利动画上升沿: 本地重新计时
    if ((g->flags0 & F0_ANIM) && !prev_anim) s_anim_ms = now_ms();

    // ---- 事件日志 ----
    if (g->move_count != prev_mc && g->move_count > 0 && g->last_r < 15 && g->last_c < 15) {
        char eb[40];
        int who = g->board[g->last_r][g->last_c];
        snprintf(eb, sizeof(eb), "%s落子 %c%d", who == 2 ? "白方" : "黑方",
                 'A' + g->last_c, g->last_r + 1);
        push_event(eb);
    }
    if (g->cheat > prev_cheat) push_event("对方开启作弊模式!");
    else if ((s_net_flags & 0x01) && (g->flags0 & F0_TAUNT) && !prev_taunt &&
             !(g->flags1 & F1_TAUNT_REMOTE)) push_event("对方发来嘲讽");
    else if ((g->flags0 & F0_OVER) && !prev_over) push_event("本局结束");

    // ---- 丢包率: 状态包序号跳变 ----
    {
        static uint8_t last_seq = 0;
        static int rx = 0, lost = 0;
        uint8_t sq = d[3];
        if (last_seq) {
            int gap = (int)(uint8_t)(sq - last_seq - 1);
            if (gap > 0 && gap < 30) lost += gap;
        }
        last_seq = sq;
        rx++;
        int tot = rx + lost;
        s_loss_pct = tot ? (lost * 100 / tot) : 0;
        if (rx > 3000) { rx /= 2; lost /= 2; }
    }

    s_have_state = 1;
    s_gs_ready = 1;
}

// forward declarations for 10 new game parse functions
static void parse_2048(const uint8_t* d);
static void parse_tetris(const uint8_t* d);
static void parse_snake(const uint8_t* d);
static void parse_breakout(const uint8_t* d);
static void parse_minesweeper(const uint8_t* d);
static void parse_flappy(const uint8_t* d);
static void parse_invaders(const uint8_t* d);
static void parse_race(const uint8_t* d);
static void parse_fps(const uint8_t* d);
static void parse_shogi(const uint8_t* d);

static void on_recv(const esp_now_recv_info_t* info, const uint8_t* d, int len)
{
    s_last_pkt_ms = now_ms();
    // 信号强度: 取对端发来的 GMK 包
    if (info && info->rx_ctrl && len >= 3 && memcmp(d, "GMK", 3) == 0)
        s_rssi = info->rx_ctrl->rssi;
    {
        static uint32_t n_beac = 0, n_welc = 0, n_state = 0, n_other = 0;
        if (len == 13 && memcmp(d, "GMKBEAC", 7) == 0) n_beac++;
        else if (len == 7 && memcmp(d, "GMKWELC", 7) == 0) n_welc++;
        else if (len == GMK_STATE_LEN) n_state++;
        else n_other++;
        static uint32_t last_log = 0;
        if (now_ms() - last_log > 5000) {
            last_log = now_ms();
            ESP_LOGI(TAG, "rx beac=%u welc=%u state=%u other=%u lastlen=%d",
                     (unsigned)n_beac, (unsigned)n_welc, (unsigned)n_state,
                     (unsigned)n_other, len);
        }
    }
    if (len == 13 && memcmp(d, "GMKBEAC", 7) == 0) {
        // 主机端重启后会重新广播 GMKBEAC: 无论 s_linked 当前是 0 还是 1 都要回 GMKHI1
        // (如果 s_linked=1 但主机重启了, 不回的话双方会假在线卡死)
        memcpy(s_host, d + 7, 6);
        add_peer(s_host);
        // 主机重置了, 所有之前游戏的 *_have 标记全部清掉
        s_have_state = 0; s_pong_have = 0; s_reversi_have = 0;
        s_tank_have = 0; s_uno_have = 0; s_c4_have = 0; s_ck_have = 0;
        s2048_have = 0; s_tetris_have = 0; s_snake_have = 0; s_breakout_have = 0;
        s_ms_have = 0; s_flappy_have = 0; s_inv_have = 0; s_race_have = 0;
        s_fps_have = 0; s_shogi_have = 0;
        if (now_ms() - s_hello_last > 800) {
            s_hello_last = now_ms();
            esp_now_send(s_host, (const uint8_t*)"GMKHI1", 6);
        }
        return;
    }
    if (len == 7 && memcmp(d, "GMKWELC", 7) == 0) {
        if (!s_linked) {
            memcpy(s_host, info->src_addr, 6);
            add_peer(s_host);
            s_linked = 1;
            ESP_LOGI(TAG, "linked");
        }
        return;
    }
    if (len == 6 && memcmp(d, "GMKBYE", 6) == 0) {
        s_linked = 0;
        return;
    }
    if (len == 24 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x10) {
        // GMKAPP: 前台 app 切换(名字+游戏id+图标id)
        // 主机心跳每 1 秒重发一次 GMKAPP (L224-228 glink.cpp), 但我们只在 game_id 真变了时才清 *_have.
        // 如果每次心跳都清 have, 会导致 view_ready() 反复 false→draw_status("等待画面数据...")
        // 和 true→render_state() 之间疯狂闪烁 (用户报告的 "连接成功画面 vs 已连接画面" 闪烁就是这个).
        uint8_t new_gid = d[4];
        s_icon_id = d[5];
        memcpy(s_app_name, d + 6, 16);
        s_app_name[16] = 0;
        if (new_gid != s_game_id) {
            // 真正切游戏: 清所有旧 have, 等待新游戏第一个 state 包
            s_game_id = new_gid;
            s_gs_ready = 1;  // 触发 render_state 按新 GID 分派 (此时 view_ready 会 false, 但至少不会错渲染)
            s_have_state = 0; s_pong_have = 0; s_reversi_have = 0;
            s_tank_have = 0; s_uno_have = 0; s_c4_have = 0; s_ck_have = 0;
            s2048_have = 0; s_tetris_have = 0; s_snake_have = 0; s_breakout_have = 0;
            s_ms_have = 0; s_flappy_have = 0; s_inv_have = 0; s_race_have = 0;
            s_fps_have = 0; s_shogi_have = 0;
        }
        // 心跳重发 (game_id 没变): 只更新名字/图标, 不清 have, 不触发 render
        return;
    }
    if (len == 20 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x21) {
        parse_pong(d);
        return;
    }
    if (len == 29 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x22) {
        parse_reversi(d);
        return;
    }
    if (len == 59 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x23) {
        parse_tank(d);
        return;
    }
    if (len == 64 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x24) {
        parse_uno(d);
        return;
    }
    if (len == 27 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x25) {
        parse_connect4(d);
        return;
    }
    if (len == 38 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x26) {
        parse_checkers(d);
        return;
    }
    if (len >= 15 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x27) { parse_2048(d); return; }
    if (len >= 111 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x28) { parse_tetris(d); return; }
    if (len >= 49 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x29) { parse_snake(d); return; }
    if (len >= 22 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x2A) { parse_breakout(d); return; }
    if (len >= 228 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x2B) { parse_minesweeper(d); return; }
    if (len >= 27 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x2C) { parse_flappy(d); return; }
    if (len >= 32 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x2D) { parse_invaders(d); return; }
    if (len >= 22 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x2E) { parse_race(d); return; }
    if (len >= 121 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x2F) { parse_fps(d); return; }
    if (len >= 110 && d[0] == 'G' && d[1] == 'M' && d[2] == 0x30) { parse_shogi(d); return; }
    if (len == GMK_STATE_LEN && d[0] == 'G' && d[1] == 'M' && d[2] == 0x20) {
        parse_state(d);
    }
}

// ===== 10 款新游戏 parse 函数 =====

static void parse_2048(const uint8_t* d)
{
    s2048.flags = d[4];
    s2048.score = (uint16_t)(d[5] | (d[6] << 8));
    // 主机 grid 8 字节 (16 格 × 4bit, 高 nibble=偶数idx, 低 nibble=奇数idx)
    memcpy(s2048.grid, &d[7], 8);
    s2048_have = 1;
    s_gs_ready = 1;
}

static void parse_tetris(const uint8_t* d)
{
    // 解码 board 10×20 每格 4bit
    for (int k = 0; k < 100; k += 2) {
        uint8_t b = d[4 + k / 2];
        s_tetris.board[k]     = (uint8_t)(b & 0xF);
        s_tetris.board[k + 1] = (uint8_t)((b >> 4) & 0xF);
    }
    s_tetris.cur_x = d[104];
    s_tetris.cur_y = d[105];
    s_tetris.cur_type = d[106];
    s_tetris.cur_color = d[107];
    s_tetris.score = (uint16_t)(d[108] | (d[109] << 8));
    s_tetris.flags = d[110];
    s_tetris_have = 1;
    s_gs_ready = 1;
}

static void parse_snake(const uint8_t* d)
{
    s_snake.food_x = d[4];
    s_snake.food_y = d[5];
    s_snake.dir = d[6];
    s_snake.score = (uint16_t)(d[7] | (d[8] << 8));
    s_snake.flags = d[9];
    memcpy(s_snake.grid_mask, &d[10], 39);
    s_snake_have = 1;
    s_gs_ready = 1;
}

static void parse_breakout(const uint8_t* d)
{
    s_breakout.paddleX = (uint16_t)(d[4] | (d[5] << 8));
    s_breakout.ballX   = (uint16_t)(d[6] | (d[7] << 8));
    s_breakout.ballY   = (uint16_t)(d[8] | (d[9] << 8));
    s_breakout.ballDX  = (int8_t)d[10];
    s_breakout.ballDY  = (int8_t)d[11];
    s_breakout.score   = (uint16_t)(d[12] | (d[13] << 8));
    s_breakout.lives   = d[14];
    s_breakout.flags   = d[15];
    memcpy(s_breakout.brick_mask, &d[16], 6);
    s_breakout_have = 1;
    s_gs_ready = 1;
}

static void parse_minesweeper(const uint8_t* d)
{
    memcpy(s_ms.grid, &d[4], 220);
    s_ms.cursorX   = d[224];
    s_ms.cursorY   = d[225];
    s_ms.mines_count = d[226];
    s_ms.flags     = d[227];
    s_ms_have = 1;
    s_gs_ready = 1;
}

static void parse_flappy(const uint8_t* d)
{
    s_flappy.birdY  = (uint16_t)(d[4] | (d[5] << 8));
    s_flappy.birdV  = (int8_t)d[6];
    s_flappy.pipe_count = (d[7] > 4) ? 4 : d[7];
    for (int i = 0; i < 4; i++) {
        int off = 8 + i * 4;
        s_flappy.pipes[i].x      = (uint16_t)(d[off] | (d[off + 1] << 8));
        s_flappy.pipes[i].gapY   = (int8_t)d[off + 2];
        s_flappy.pipes[i].passed = d[off + 3];
    }
    s_flappy.score = (uint16_t)(d[24] | (d[25] << 8));
    s_flappy.flags = d[26];
    s_flappy_have = 1;
    s_gs_ready = 1;
}

static void parse_invaders(const uint8_t* d)
{
    memcpy(s_inv.alive_mask, &d[4], 7);
    s_inv.playerX     = (uint16_t)(d[11] | (d[12] << 8));
    s_inv.alien_baseX = (int16_t)(d[13] | (d[14] << 8));
    s_inv.alien_rowY  = (int16_t)(d[15] | (d[16] << 8));
    s_inv.alien_dir   = (int8_t)d[17];
    s_inv.bullet_count = d[18];
    if (s_inv.bullet_count > 5) s_inv.bullet_count = 5;
    for (int i = 0; i < 5; i++) {
        s_inv.bullets[i][0] = d[19 + i * 2];
        s_inv.bullets[i][1] = d[20 + i * 2];
    }
    s_inv.score = (uint16_t)(d[29] | (d[30] << 8));
    s_inv.flags = d[31];
    s_inv_have = 1;
    s_gs_ready = 1;
}

static void parse_race(const uint8_t* d)
{
    s_race.playerX   = (uint16_t)(d[4] | (d[5] << 8));
    s_race.playerZ[0] = d[6]; s_race.playerZ[1] = d[7]; s_race.playerZ[2] = d[8];
    s_race.speed     = (uint16_t)(d[9] | (d[10] << 8));
    s_race.gear      = d[11];
    s_race.laps      = d[12];
    s_race.seg_count = (uint16_t)(d[13] | (d[14] << 8));
    s_race.cur_seg   = (uint16_t)(d[15] | (d[16] << 8));
    s_race.cur_curve = (uint16_t)(d[17] | (d[18] << 8));
    s_race.flags[0]  = d[19];
    s_race.flags[1]  = 0;
    s_race_have = 1;
    s_gs_ready = 1;
}

static void parse_fps(const uint8_t* d)
{
    memcpy(&s_fps.posX,  &d[4],  4);
    memcpy(&s_fps.posY,  &d[8],  4);
    memcpy(&s_fps.dirX,  &d[12], 4);
    memcpy(&s_fps.dirY,  &d[16], 4);
    memcpy(&s_fps.planeX, &d[20], 4);
    memcpy(&s_fps.planeY, &d[24], 4);
    s_fps.health       = d[28];
    s_fps.weapon_frame = d[29];
    s_fps.level        = d[30];
    s_fps.flags        = d[31];
    int ec = d[32];
    if (ec > 8) ec = 8;
    s_fps.enemy_count = (uint8_t)ec;
    int p = 33;
    for (int i = 0; i < ec; i++) {
        memcpy(&s_fps.enemy_x[i], &d[p], 4); p += 4;
        memcpy(&s_fps.enemy_y[i], &d[p], 4); p += 4;
        s_fps.enemy_health[i] = d[p++];
        s_fps.enemy_alive[i]  = d[p++];
        s_fps.enemy_state[i]  = d[p++];
    }
    s_fps_have = 1;
    s_gs_ready = 1;
}

static void parse_shogi(const uint8_t* d)
{
    memcpy(s_shogi.board, d + 4, 81);
    // hand[2][8] = 16 bytes starting at d[85]
    memcpy(s_shogi.hand, d + 85, 16);
    s_shogi.turn      = d[101];
    s_shogi.cursor_r  = d[102];
    s_shogi.cursor_c  = d[103];
    s_shogi.sel_r     = d[104];
    s_shogi.sel_c     = d[105];
    s_shogi.game_over = d[106];
    s_shogi.winner    = d[107];
    s_shogi.move_count = (uint16_t)(d[108] | (d[109] << 8));
    s_shogi_have = 1;
    s_gs_ready = 1;
}

static void wifi_espnow_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(GMK_CH, WIFI_SECOND_CHAN_NONE);
    esp_now_init();
    esp_now_register_recv_cb(on_recv);
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "C3 mac %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ================= 游戏画面渲染(Cardputer 布局放大版) =================
#define CELL 12
#define BPD  (CELL * 15)   // 180
#define BSX  70
#define BSY  30
#define LX   4             // 左栏
#define RX   (BSX + BPD + 6)  // 256 右栏
#define RW   (LCD_W - RX)     // 64

static const char* kPN[3] = { "稳健", "凶狠", "诡诈" };
static const color_t kPC[3] = { C_GREEN, C_RED, C_MAGENTA };

static void draw_trend(int x, int y, int w, int h)
{
    fb_rect(x, y, w, h, C_TREND);
    int mid = y + h / 2;
    fb_hline(x + 1, mid, w - 2, C_TMID);
    int cnt = s_gs.trend_count;
    if (cnt < 1) return;
    if (cnt > 24) cnt = 24;
    int px_ = -1, py_ = -1;
    for (int i = 0; i < cnt; i++) {
        int idx = (s_gs.trend_head - cnt + i + 24) % 24;
        int v = s_gs.trend[idx];
        int xx = x + 1 + i * (w - 2) / 23;
        int yy = mid - v * (h / 2 - 2) / 5;
        color_t col = (v >= 0) ? C_GREEN : C_RED;
        if (px_ >= 0) {
            fb_line(px_, py_, xx, yy, col);
            fb_line(px_, py_ + 1, xx, yy + 1, col);
        }
        fb_fill(xx - 1, yy - 1, 3, 3, col);
        px_ = xx; py_ = yy;
    }
}

static void draw_panel(void)
{
    int x = RX, w = RW;
    fb_text(F12, x, 4, "你还剩", C_WHITE);

    char buf[24];
    if (s_gs.danger >= 10) snprintf(buf, sizeof(buf), "%d", s_gs.danger);
    else if (s_gs.danger > 0) snprintf(buf, sizeof(buf), "%d", s_gs.danger);
    else snprintf(buf, sizeof(buf), "--");
    color_t dc = s_gs.danger > 0
        ? (s_gs.danger <= 3 ? C_RED : (s_gs.danger <= 10 ? C_ORANGE : C_YELLOW))
        : C_DGRAY;
    fb_text(F14, x + 2, 18, buf, dc);
    int tw = u8g2_str_width(F14, buf);
    fb_text(F12, x + 6 + tw, 24, "步", C_DGRAY);

    // 性格竖排(右侧竖条)
    int pid = s_gs.persona < 3 ? s_gs.persona : 0;
    int vx = x + w - 18;
    for (int i = 0; i < 2; i++) {
        char ch[4] = { kPN[pid][i * 3], kPN[pid][i * 3 + 1], kPN[pid][i * 3 + 2], 0 };
        fb_text(F14, vx, 4 + i * 18, ch, kPC[pid]);
    }

    draw_trend(x, 42, w, 26);

    snprintf(buf, sizeof(buf), "MOVE %d", s_gs.move_count);
    fb_text(F12, x, 72, buf, C_LGRAY);

    snprintf(buf, sizeof(buf), "作弊%d %s", s_gs.cheat,
             s_gs.mood == 1 ? "急" : (s_gs.mood == 2 ? "浪" : "稳"));
    fb_text(F12, x, 86, buf, C_MAGENTA);

    // 胜利嘲讽(3 行)时 Balance 区让位(Cardputer 同款逻辑)
    if (!((s_gs.flags0 & F0_TAUNT) && s_gs.taunt_kind == 3)) {
        int32_t s1 = s_gs.score1 > 0 ? s_gs.score1 : 0;
        int32_t s2 = s_gs.score2 > 0 ? s_gs.score2 : 0;
        int split = (int)((int64_t)(s1 + 1) * w / (s1 + s2 + 2));
        fb_fill(x, 132, split, 4, C_GREEN);
        fb_fill(x + split, 132, w - split, 4, C_RED);

        int32_t adv = s_gs.score2 - s_gs.score1;
        int ai_rate = 50 - (int)(adv / 400);
        if (ai_rate < 5) ai_rate = 5;
        if (ai_rate > 95) ai_rate = 95;
        snprintf(buf, sizeof(buf), "你%d%%", 100 - ai_rate);
        fb_text(F12, x + 2, 140, buf, (100 - ai_rate) >= 50 ? C_GREEN : C_RED);
    }
}

static void draw_overlays(void)
{
    // 胜利动画(本地计时复刻 Cardputer 三阶段)
    if (s_gs.flags0 & F0_ANIM) {
        uint32_t el = now_ms() - s_anim_ms;
        int bx = BSX + 8, by = BSY + BPD / 2 - 22, bw = BPD - 16;
        fb_fill(bx, by, bw, 44, C_BLACK);
        fb_rect(bx, by, bw, 44, C_YELLOW);
        if (el < 600) {
            fb_text_cx(bx, bw, by + 6, "你赢了！", F12, C_GREEN);
            fb_text_cx(bx, bw, by + 26, "WIN", F12, C_WHITE);
        } else if (el < 1200) {
            fb_text_cx(bx, bw, by + 6, "难度提升", F12, C_YELLOW);
            char stars[16];
            int n = s_gs.difficulty < 10 ? s_gs.difficulty + 1 : 10;
            if (n > 15) n = 15;
            for (int i = 0; i < n; i++) stars[i] = '*';
            stars[n] = 0;
            fb_text_cx(bx, bw, by + 26, stars, F12, C_YELLOW);
        } else {
            char b[32];
            snprintf(b, sizeof(b), "第 %d 星 开新局", s_gs.difficulty < 10 ? s_gs.difficulty + 1 : 10);
            fb_text_cx(bx, bw, by + 16, b, F12, C_CYAN);
        }
    }

    // R 重开确认
    if (s_gs.flags1 & F1_RCONF) {
        const char* tip = "再按R确认重开";
        int tw = u8g2_str_width(F10B, tip) + 8;
        int bx = BSX + (BPD - tw) / 2;
        fb_fill(bx, BSY + 3, tw, 14, C_BLACK);
        fb_rect(bx, BSY + 3, tw, 14, C_RED);
        fb_text(F10B, bx + 4, BSY + 5, tip, C_RED);
    }

    // 回放横幅
    if (s_gs.flags0 & F0_REPLAY) {
        char rb[24];
        snprintf(rb, sizeof(rb), "回放 %d/%d", s_gs.replay_idx, s_gs.replay_total);
        int tw = u8g2_str_width(F10B, rb) + 8;
        int bx = BSX + (BPD - tw) / 2;
        fb_fill(bx, BSY + 3, tw, 14, C_BLACK);
        fb_rect(bx, BSY + 3, tw, 14, C_CYAN);
        fb_text(F10B, bx + 4, BSY + 5, rb, C_CYAN);
    }

    // 战报结算
    if (s_gs.flags0 & F0_RESULT) {
        int w = 280, h = 160, x = 20, y = 40;
        fb_fill(x, y, w, h, C_PANEL);
        fb_rect(x, y, w, h, C_WHITE);
        fb_hline(x + 2, y + 28, w - 4, C_TREND);

        const char* title = s_gs.winner == 0 ? "平局"
            : (s_gs.flags1 & F1_PVP) ? (s_gs.winner == 1 ? "黑方胜" : "白方胜")
            : (s_gs.winner == 1 ? "你赢了" : "你输了");
        fb_text_cx(x, w, y + 8, title, F14,
                   s_gs.winner == 0 ? C_YELLOW : (s_gs.winner == 1 ? C_GREEN : C_RED));

        char b[48];
        snprintf(b, sizeof(b), "手数 %d  用时 %d:%02d", s_gs.move_count, s_gs.game_sec / 60, s_gs.game_sec % 60);
        fb_text(F12, x + 10, y + 36, b, C_WHITE);
        snprintf(b, sizeof(b), "AI作弊 %d 次  最险 %d 步", s_gs.cheat, s_gs.min_danger == 255 ? 0 : s_gs.min_danger);
        fb_text(F12, x + 10, y + 54, b, C_WHITE);
        snprintf(b, sizeof(b), "连胜 %d   最高 %d", s_gs.streak, s_gs.best);
        fb_text(F12, x + 10, y + 72, b, C_WHITE);
        if (s_gs.worst_no > 0) snprintf(b, sizeof(b), "失误 %d 手  最大第 %d 手", s_gs.blunders, s_gs.worst_no);
        else snprintf(b, sizeof(b), "失误 0 手  全程无破绽");
        fb_text(F12, x + 10, y + 90, b, s_gs.blunders == 0 ? C_GREEN : C_LGRAY);

        static const char* kAchv[6] = { "首胜", "反杀", "屠龙", "三连", "零作弊", "无失误" };
        int ax = x + 10, ay = y + 116;
        int any = 0;
        for (int i = 0; i < 6; i++) {
            if (!(s_gs.achv_new & (1 << i))) continue;
            int tw = u8g2_str_width(F12, kAchv[i]);
            if (ax + tw > x + w - 10) { ax = x + 10; ay += 18; }
            fb_text(F12, ax, ay, kAchv[i], C_YELLOW);
            ax += tw + 10;
            any = 1;
        }
        if (!any) fb_text(F12, x + 10, y + 120, "R 重开  V 回放  L 难度", C_DGRAY);
    }

    // 帮助
    if (s_gs.flags0 & F0_HELP) {
        int w = 264, h = 150, x = 28, y = 45;
        fb_fill(x, y, w, h, C_BLACK);
        fb_rect(x, y, w, h, C_YELLOW);
        fb_text(F14, x + 10, y + 6, "按键帮助", C_YELLOW);
        static const char* kHelp[] = {
            "方向/回车  走子落子",
            "X 危险标记  K AI性格",
            "G 面板      L 难度",
            "M 辅助      U 悔棋",
            "P 双人对战  V 回放",
            "T 限时60秒  R 重开",
            "H 本帮助",
        };
        for (int i = 0; i < 7; i++)
            fb_text(F12, x + 10, y + 28 + i * 16, kHelp[i], C_WHITE);
    }
}

// 底部信息带: 左侧胜率条(左栏下方) + 棋盘下方事件日志
// 只在联机对战时显示(用户要求), 平时留给 AI 的误/险/成就
static void draw_bottom_band(bool pvp_mode)
{
    if (!pvp_mode) return;

    const int wx = LX, ww = 62;
    int32_t s1 = s_gs.score1 > 0 ? s_gs.score1 : 0;
    int32_t s2 = s_gs.score2 > 0 ? s_gs.score2 : 0;
    int split = (int)((int64_t)(s1 + 1) * ww / (s1 + s2 + 2));
    int pct = (int)((int64_t)(s1 + 1) * 100 / (s1 + s2 + 2));

    fb_text(F10B, wx, 203, "胜率", C_DGRAY);
    fb_fill(wx, 214, ww, 5, C_DGRAY);
    fb_fill(wx, 214, split, 5, C_GREEN);
    fb_fill(wx + split, 214, ww - split, 5, C_RED);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    fb_text(F10B, wx, 222, buf, C_LGRAY);

    // 棋盘正下方: 事件日志(只有联机对战有远端事件)
    if (s_net_flags & 0x01) fb_text(F10B, BSX, 214, s_evt, C_LGRAY);
}

// 顶部滚动横幅: 对方开启作弊辅助时, 文字从左边缘进入, 一直向右滑出再重来
static void draw_cheat_banner(void)
{
    if (!(s_gs.flags1 & F1_CHEAT)) return;
    const int h = 13;
    fb_fill(0, 0, LCD_W, h, C_BLACK);
    const char* t = "对方玩家正在启用作弊模式  ★  ";
    int tw = u8g2_str_width(F10B, t);
    if (tw <= 0) return;
    // 单向循环: x 从 -tw(完全在左边看不见) 走到 LCD_W(完全滑出右边), 再回到 -tw
    int span = LCD_W + tw;
    int off = (int)((now_ms() / 22) % (uint32_t)span);
    int x = -tw + off;
    fb_text(F10B, x, 1, t, C_YELLOW);
}

static void render_pong(void);
static void render_reversi(void);
static void render_tank(void);
static void render_uno(void);
static void render_connect4(void);
static void render_checkers(void);
static void render_2048(void);
static void render_tetris(void);
static void render_snake(void);
static void render_breakout(void);
static void render_minesweeper(void);
static void render_flappy(void);
static void render_invaders(void);
static void render_race(void);
static void render_fps_monitor(void);
static void render_shogi(void);
static void render_appinfo(void);

static void render_state(void)
{
    // 多游戏分派: Pong / Reversi / Tank / UNO / app图标+名字 / 五子棋
    if (s_game_id == GID_PONG) { render_pong(); return; }
    if (s_game_id == GID_REVERSI) { render_reversi(); return; }
    if (s_game_id == GID_TANK) { render_tank(); return; }
    if (s_game_id == GID_UNO) { render_uno(); return; }
    if (s_game_id == GID_CONNECT4) { render_connect4(); return; }
    if (s_game_id == GID_CHECKERS) { render_checkers(); return; }
    if (s_game_id == GID_2048) { render_2048(); return; }
    if (s_game_id == GID_TETRIS) { render_tetris(); return; }
    if (s_game_id == GID_SNAKE) { render_snake(); return; }
    if (s_game_id == GID_BREAKOUT) { render_breakout(); return; }
    if (s_game_id == GID_MINESWEEPER) { render_minesweeper(); return; }
    if (s_game_id == GID_FLAPPY) { render_flappy(); return; }
    if (s_game_id == GID_INVADERS) { render_invaders(); return; }
    if (s_game_id == GID_RACE) { render_race(); return; }
    if (s_game_id == GID_FPS) { render_fps_monitor(); return; }
    if (s_game_id == GID_SHOGI) { render_shogi(); return; }
    if (s_game_id != GID_GOMOKU) { render_appinfo(); return; }

    extern uint32_t g_render_cnt;
    g_render_cnt++;
    static int n = 0;
    if (++n == 1 || n % 100 == 0) {
        ESP_LOGI(TAG, "render #%d board_r0=%d,%d,%d mv=%d over=%d",
                 n, s_gs.board[7][6], s_gs.board[7][7], s_gs.board[7][8],
                 s_gs.move_count, s_gs.flags0 & F0_OVER);
    }
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BG);

    // 棋盘底 + 网格
    fb_fill(BSX, BSY, BPD, BPD, C_WOOD);
    for (int i = 0; i < 15; i++) {
        fb_hline(BSX + CELL / 2, BSY + i * CELL + CELL / 2, BPD - CELL, C_BLACK);
        fb_vline(BSX + i * CELL + CELL / 2, BSY + CELL / 2, BPD - CELL, C_BLACK);
    }

    // 棋子
    for (int r = 0; r < 15; r++) {
        for (int c = 0; c < 15; c++) {
            int cx = BSX + c * CELL + CELL / 2;
            int cy = BSY + r * CELL + CELL / 2;
            if (s_gs.board[r][c] == 1) {
                fb_disc(cx, cy, CELL / 2 - 1, C_BLACK);
            } else if (s_gs.board[r][c] == 2) {
                fb_disc(cx, cy, CELL / 2 - 1, C_WHITE);
                fb_circle(cx, cy, CELL / 2 - 1, C_BLACK);
            }
        }
    }

    // 威胁标记(红=AI成五 橙=AI成四 绿框=你成五)
    if ((s_gs.flags0 & F0_THREAT) && !(s_gs.flags0 & F0_REPLAY)) {
        for (int r = 0; r < 15; r++) {
            for (int c = 0; c < 15; c++) {
                if (s_gs.board[r][c] || s_gs.threat[r][c] == 0) continue;
                int mx = BSX + c * CELL + CELL / 2;
                int my = BSY + r * CELL + CELL / 2;
                if (s_gs.threat[r][c] == 3) fb_fill(mx - 3, my - 3, 7, 7, C_RED);
                else if (s_gs.threat[r][c] == 2) fb_fill(mx - 3, my - 3, 6, 6, C_ORANGE);
                else fb_rect(mx - 3, my - 3, 7, 7, C_GREEN);
            }
        }
    }

    // 最后一步高亮
    if (s_gs.last_r < 15 && s_gs.last_c < 15) {
        fb_rect(BSX + s_gs.last_c * CELL, BSY + s_gs.last_r * CELL, CELL, CELL, C_CYAN);
    }

    // 作弊提示点: 手柄方(本机)开了作弊, 主机算好的落点用黄圈标出来
    if ((s_gs.flags1 & F1_PVP) && s_cheat_mine && s_gs.hint_r < 15 && s_gs.hint_c < 15 &&
        s_gs.board[s_gs.hint_r][s_gs.hint_c] == 0) {
        int hx = BSX + s_gs.hint_c * CELL + CELL / 2;
        int hy = BSY + s_gs.hint_r * CELL + CELL / 2;
        fb_circle(hx, hy, CELL / 2 - 1, C_YELLOW);
        fb_circle(hx, hy, CELL / 2 - 3, C_YELLOW);
    }

    // 光标(回放时不画)
    if (!(s_gs.flags0 & F0_REPLAY)) {
        int cx = BSX + s_gs.cursor_x * CELL + CELL / 2;
        int cy = BSY + s_gs.cursor_y * CELL + CELL / 2;
        fb_rect(cx - CELL / 2, cy - CELL / 2, CELL, CELL, C_RED);
    }

    // ---------- 左栏 ----------
    char b[40];
    fb_text(F12, LX, 4, "五子棋", C_CYAN);

    bool pvp_mode = (s_gs.flags1 & F1_PVP) != 0;

    if (pvp_mode) {
        // 双人对战: 单人那套 Lv/星级/YOU/AI/WIN 全部隐藏
        fb_text(F12, LX, 20, "PVP", C_MAGENTA);
        fb_text(F12, LX, 38, (s_gs.flags0 & F0_OVER)
                    ? (s_gs.winner == 0 ? "平局" : (s_gs.winner == 1 ? "黑胜" : "白胜"))
                    : (s_gs.cur_player == 1 ? "黑方" : "白方"),
                C_WHITE);
        if (s_net_flags & 0x01) {
            char pb[24];
            if (!(s_net_flags & 0x08)) {
                snprintf(pb, sizeof(pb), "我%s 他%s", (s_net_flags & 0x04) ? "OK" : "--",
                         (s_net_flags & 0x02) ? "OK" : "--");
                fb_text(F10B, LX, 58, pb, ((s_net_flags & 0x06) == 0x06) ? C_GREEN : C_YELLOW);
                fb_text(F10B, LX, 72, "A:准备 X:骂", C_DGRAY);
            } else {
                fb_text(F10B, LX, 58, (s_cur_player == 2) ? ">>> 我方" : "对方回合",
                        (s_cur_player == 2) ? C_GREEN : C_LGRAY);
                fb_text(F10B, LX, 72, "X:嘲讽", C_MAGENTA);
            }
        } else {
            fb_text(F10B, LX, 58, "同机双人", C_DGRAY);
        }
        // 作弊辅助: 本机(手柄方)开关状态
        fb_text(F10B, LX, 88, s_cheat_mine ? "作弊中" : "Y:作弊",
                s_cheat_mine ? C_YELLOW : C_DGRAY);
    } else {
        snprintf(b, sizeof(b), "Lv.%d", s_gs.difficulty);
        fb_text(F12, LX, 20, b, C_WHITE);

        for (int row = 0; row < 2; row++) {
            char st[6];
            for (int i = 0; i < 5; i++) st[i] = (row * 5 + i < s_gs.difficulty) ? '*' : ' ';
            st[5] = 0;
            fb_text(F10B, LX, 36 + row * 14, st, C_YELLOW);
        }

        int total = s_gs.pw + s_gs.aw;
        int ai_rate = total > 0 ? s_gs.aw * 100 / total : 0;
        snprintf(b, sizeof(b), "YOU%2d", s_gs.pw);
        fb_text(F12, LX, 66, b, C_GREEN);
        snprintf(b, sizeof(b), "AI %2d", s_gs.aw);
        fb_text(F12, LX, 82, b, C_ORANGE);
        snprintf(b, sizeof(b), "WIN%2d%%", ai_rate);
        fb_text(F12, LX, 98, b, C_RED);

        const char* st;
        if (s_gs.flags0 & F0_REPLAY) st = "REPLAY";
        else if (s_gs.flags0 & F0_OVER) {
            if (s_gs.winner == 0) st = "DRAW";
            else if (s_gs.flags1 & F1_ASSIST) st = s_gs.winner == 2 ? "AI WIN" : "OPP WIN";
            else st = s_gs.winner == 1 ? "YOU WIN" : "AI WIN";
        } else {
            if (s_gs.flags1 & F1_ASSIST) st = s_gs.cur_player == 1 ? "INPUT P1" : "THINK..";
            else st = s_gs.cur_player == 1 ? "YOURTURN" : "THINK..";
        }
        fb_text(F12, LX, 114, st, C_WHITE);
        fb_text(F12, LX, 128, (s_gs.flags1 & F1_ASSIST) ? "Assist" : "Normal", C_LGRAY);
    }

    // ---------- 左下角密集状态区 ----------
    int hy = 166;
    fb_text(F10B, LX, hy, "[H]帮助", C_YELLOW);

    if (!pvp_mode) {
        snprintf(b, sizeof(b), "连胜%d 最%d", s_gs.streak, s_gs.best);
        fb_text(F10B, LX, hy + 13, b, s_gs.streak > 0 ? C_GREEN : C_DGRAY);
    }

    if (s_gs.flags1 & F1_BLITZ) {
        int cs = (s_gs.flags0 & F0_OVER) ? 0 : (s_gs.cur_player == 1 ? s_gs.clk1 : s_gs.clk2);
        snprintf(b, sizeof(b), "CLK %d", cs);
        fb_text(F10B, LX, hy + 26, b, (!(s_gs.flags0 & F0_OVER) && cs < 10) ? C_RED : C_CYAN);
    } else {
        snprintf(b, sizeof(b), "T %d:%02d", s_gs.game_sec / 60, s_gs.game_sec % 60);
        fb_text(F10B, LX, hy + 26, b, C_CYAN);
    }

    // AI 的误/险只在单人模式显示
    if (!pvp_mode) {
        snprintf(b, sizeof(b), "误%d 险%d", s_gs.blunders, s_gs.min_danger == 255 ? 0 : s_gs.min_danger);
        fb_text(F10B, LX, hy + 39, b, C_LGRAY);
    }

    // ---------- 右栏 ----------
    int ty0 = 10;
    if (s_gs.flags0 & F0_PANEL) {
        draw_panel();
        ty0 = 160;   // 面板占 y4..154, 嘲讽从 160 开始(避免压住胜率条)
    }

    // 嘲讽(胜利动画时不画; 自己发出去的嘲讽不显示在自己屏幕上)
    if ((s_gs.flags0 & F0_TAUNT) && !(s_gs.flags0 & F0_ANIM) &&
        !(s_gs.flags1 & F1_TAUNT_REMOTE)) {
        color_t tc = s_gs.taunt_kind == 4 ? C_CYAN
            : (s_gs.taunt_kind == 2 ? C_MAGENTA : (s_gs.taunt_kind == 1 ? C_ORANGE : C_RED));
        int nl = s_gs.taunt_kind == 3 ? 3 : 2;
        for (int i = 0; i < nl; i++) {
            if (!s_gs.taunt[i][0]) break;
            fb_text_cx(RX, RW, ty0 + i * 16, s_gs.taunt[i], F14, tc);
        }
    }

    // 右下角: 作弊提示(该下哪)
    if ((s_gs.flags1 & F1_PVP) && s_cheat_mine) {
        char hb[16];
        if (s_gs.hint_r < 15 && s_gs.hint_c < 15)
            snprintf(hb, sizeof(hb), "下 %c%d", 'A' + s_gs.hint_c, s_gs.hint_r + 1);
        else
            snprintf(hb, sizeof(hb), "等对方");
        fb_text(F12, RX, 192, hb, C_YELLOW);
    }

    draw_overlays();
    draw_bottom_band(pvp_mode);
    draw_signal_right(318, 204);
    draw_link_stats();
    draw_batt_row();
    draw_cheat_banner();
    toast_draw();
    push_screen();
}

// ================= Pong / app 图标画面 =================
// 图标与 Cardputer 同一份(40x40 RGB565, gomoku_c3/main/icons.c)
extern const uint16_t icon_generic_generated[1600];
extern const uint16_t icon_gomoku_generated[1600];
extern const uint16_t icon_pong_generated[1600];
extern const uint16_t icon_reversi_generated[1600];
extern const uint16_t icon_tank_generated[1600];
extern const uint16_t icon_uno_generated[1600];
extern const uint16_t icon_connect4_generated[1600];
extern const uint16_t icon_checkers_generated[1600];
static const uint16_t* const kIcons[8] = {
    icon_generic_generated, icon_gomoku_generated, icon_pong_generated,
    icon_reversi_generated, icon_tank_generated, icon_uno_generated,
    icon_connect4_generated, icon_checkers_generated
};

// 图标 565 色 → 帧缓冲调色板索引(先精确匹配, 再最近色)
static color_t pal_of(uint16_t c)
{
    for (int i = 0; i < 21; i++) if (kPal[i] == c) return (color_t)i;
    int best = 0, bd = 1 << 30;
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    for (int i = 0; i < 21; i++) {
        uint16_t p = kPal[i];
        int pr = (p >> 11) & 31, pg = (p >> 5) & 63, pb = p & 31;
        int d = (r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
        if (d < bd) { bd = d; best = i; }
    }
    return (color_t)best;
}

// 40x40 图标放大 2 倍画进帧缓冲(80x80)
static void fb_icon2x(int x, int y, const uint16_t* img)
{
    for (int j = 0; j < 40; j++)
        for (int i = 0; i < 40; i++)
            fb_fill(x + i * 2, y + j * 2, 2, 2, pal_of(img[j * 40 + i]));
}

// 非游戏 app(桌面等): 图标 + 名字
static void render_appinfo(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    fb_icon2x((LCD_W - 80) / 2, 36, kIcons[s_icon_id < 7 ? s_icon_id : 0]);
    fb_text_cx(0, LCD_W, 136, s_app_name, F14, C_CYAN);
    fb_text_cx(0, LCD_W, 166, "已连接 Cardputer", F12, C_DGRAY);
    toast_draw();
    push_screen();
}

// ===== 10 款新游戏 render 函数 =====

// --- 2048 ---
static color_t _tile_color_2048(int tile_val)
{
    // tile_val 是 2 的幂次(2,4,8,...2048) 或 0=空
    switch (tile_val) {
    case 2:    return C_WHITE;
    case 4:    return C_LGRAY;
    case 8:    return C_YELLOW;
    case 16:   return C_ORANGE;
    case 32:   return C_RED;
    case 64:   return C_RED;
    case 128:  return C_YELLOW;
    case 256:  return C_YELLOW;
    case 512:  return C_YELLOW;
    case 1024: return C_ORANGE;
    case 2048: return C_ORANGE;
    default:   return C_LGRAY;
    }
}

static void render_2048(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BG);
    char sc[16]; snprintf(sc, sizeof(sc), "Score: %d", s2048.score);
    fb_text_cx(0, LCD_W, 4, sc, F14, C_WHITE);

    // 4×4 网格, 每格 50px, 中央对齐: x=60, y=30
    const int cx = 60, cy = 30, cs = 50;
    fb_rect(cx - 2, cy - 2, cs * 4 + 4, cs * 4 + 4, C_DGRAY);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = y * 4 + x;
            int byte_off = idx / 2;
            // 主机打包: idx偶数→高nibble(shift=4), idx奇数→低nibble(shift=0)
            int shift = (idx & 1) ? 0 : 4;
            int nibble = (s2048.grid[byte_off] >> shift) & 0xF;
            int val = nibble >= 1 ? (1 << nibble) : 0;
            int px = cx + x * cs, py = cy + y * cs;
            fb_fill(px + 2, py + 2, cs - 4, cs - 4, _tile_color_2048(val));
            if (val > 0) {
                char txt[8]; snprintf(txt, sizeof(txt), "%d", val);
                fb_text_cx(px + 2, cs - 4, py + cs / 2 - 6, txt, F14, C_BLACK);
            }
        }
    }
    if (s2048.flags & 1) {
        fb_fill(0, 0, LCD_W, LCD_H, C_DGRAY);
        fb_text_cx(0, LCD_W, LCD_H / 2 - 10, "GAME OVER", F14, C_RED);
    }
    toast_draw(); push_screen();
}

// --- Tetris ---
static color_t _tetris_color(int c)
{
    switch (c) {
    case 1: return C_CYAN;   // I
    case 2: return C_YELLOW; // O
    case 3: return C_MAGENTA; // T
    case 4: return C_GREEN;   // S
    case 5: return C_RED;     // Z
    case 6: return C_BLUE;    // J
    case 7: return C_ORANGE;  // L
    default: return C_BLACK;
    }
}

static void render_tetris(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    // 10×20 board, cell 11px wide → 110px, x=105, cell 10px tall → 200px, y=20
    const int bx = 105, by = 20, cw = 11, ch = 10;
    fb_rect(bx - 2, by - 2, cw * 10 + 4, ch * 20 + 4, C_DGRAY);
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 10; x++) {
            int col = s_tetris.board[y * 10 + x];
            if (col > 0) {
                fb_fill(bx + x * cw + 1, by + y * ch + 1, cw - 2, ch - 2, _tetris_color(col));
            }
        }
    }
    // 当前方块 overlay (简化: 只画中心一个格)
    if (s_tetris.cur_color > 0) {
        fb_fill(bx + s_tetris.cur_x * cw + 1, by + s_tetris.cur_y * ch + 1,
                cw - 2, ch - 2, _tetris_color(s_tetris.cur_color));
    }
    // 右侧信息
    char sc[16]; snprintf(sc, sizeof(sc), "%d", s_tetris.score);
    fb_text(F14, bx + cw * 10 + 10, 30, sc, C_WHITE);
    fb_text(F12, bx + cw * 10 + 10, 50, "TETRIS", C_CYAN);
    if (s_tetris.flags & 1) fb_text(F14, bx + cw * 10 + 10, 100, "OVER", C_RED);
    toast_draw(); push_screen();
}

// --- Snake ---
static void render_snake(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    // 24×13 grid, cell 13px → 312宽, y=20
    const int gx = 4, gy = 20, cw = 13, ch = 13;
    fb_rect(gx - 1, gy - 1, cw * 24 + 2, ch * 13 + 2, C_DGRAY);
    for (int i = 0; i < 312; i++) {
        if (s_snake.grid_mask[i / 8] & (1 << (i % 8))) {
            int x = i % 24, y = i / 24;
            fb_fill(gx + x * cw + 1, gy + y * ch + 1, cw - 2, ch - 2, C_GREEN);
        }
    }
    // 食物
    fb_disc(gx + s_snake.food_x * cw + cw / 2, gy + s_snake.food_y * ch + ch / 2, 5, C_RED);
    // 方向箭头
    const char* arr[] = { "^", ">", "v", "<" };
    fb_text(F12, gx + 314, 30, arr[s_snake.dir & 3], C_CYAN);
    // 分数
    char sc[16]; snprintf(sc, sizeof(sc), "%d", s_snake.score);
    fb_text(F14, 260, 4, sc, C_YELLOW);
    if (s_snake.flags & 1) fb_text_cx(0, LCD_W, LCD_H / 2, "GAME OVER", F14, C_RED);
    toast_draw(); push_screen();
}

// --- Breakout ---
// 主机画布 240×135 → C3 320×240 等比例缩放
// 主机 brick: bw=29 (x*bw+2), 宽25, 高8, y=y*10+20
// 主机 paddleY=120 (height-15), paddleW≈40
static void render_breakout(void)
{
    s_last_render = now_ms();
    const float sx = (float)LCD_W / 240.0f;   // 1.333
    const float sy = (float)LCD_H / 135.0f;   // 1.778

    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    // 砖块 5行 × 8列 (按主机实际参数等比缩放)
    const int HOST_BW = 29;   // 主机 brick 间距
    const int HOST_BRICK_W = 25;
    const int HOST_BRICK_H = 8;
    const int HOST_BRICK_Y0 = 20;
    const int HOST_BRICK_DY = 10;
    int brick_w = (int)(HOST_BRICK_W * sx);
    int brick_h = (int)(HOST_BRICK_H * sy);
    int col_gap = (int)(HOST_BW * sx);
    int brick_x0 = (int)(2.0f * sx);
    color_t bcol[] = { C_DGRAY, C_CYAN, C_GREEN, C_YELLOW, C_RED };
    for (int r = 0; r < 5; r++) {
        int ry = (int)((HOST_BRICK_Y0 + r * HOST_BRICK_DY) * sy);
        for (int c = 0; c < 8; c++) {
            int idx = r * 8 + c;
            int byte_off = idx / 8, bit = idx % 8;
            if (byte_off < 6 && (s_breakout.brick_mask[byte_off] & (1 << bit))) {
                int rx = brick_x0 + c * col_gap;
                fb_fill(rx, ry, brick_w, brick_h, bcol[r]);
            }
        }
    }

    // paddle: Q8.8 → 主机像素 → × sx; Y 固定 host=120 → × sy
    float hpx = (float)(int16_t)s_breakout.paddleX / 256.0f;
    int paddlex = (int)(hpx * sx);
    int paddley = (int)(120.0f * sy);
    int paddle_w = (int)(40.0f * sx);
    fb_fill(paddlex, paddley, paddle_w, (int)(6.0f * sy), C_WHITE);

    // ===== 球位置预测 =====
    // host 编码 ballDY = int8(_ballDY * 4), 实际每帧移动 dy/4 主机像素
    // paddle 是固定的 (不跟球一起预测, 球单独前推)
    float hbx = (float)(int16_t)s_breakout.ballX / 256.0f;
    float hby = (float)(int16_t)s_breakout.ballY / 256.0f;
    // 主机像素下前推 3 帧 (约 60ms 延迟补偿)
    int dx_host = (int)(int8_t)s_breakout.ballDX / 4;  // 主机每帧速度
    int dy_host = (int)(int8_t)s_breakout.ballDY / 4;
    hbx += dx_host * 3;
    hby += dy_host * 3;
    // 主机范围裁剪
    if (hbx < 0) hbx = 0;
    if (hbx > 240) hbx = 240;
    if (hby < 0) hby = 0;
    if (hby > 135) hby = 135;
    int bx = (int)(hbx * sx);
    int by = (int)(hby * sy);
    fb_disc(bx, by, (int)(4.0f * sx), C_WHITE);

    // 顶部 HUD
    fb_text(F12, 4, 4, "Lives:", C_WHITE);
    char ls[8]; snprintf(ls, sizeof(ls), "%d", s_breakout.lives);
    fb_text(F12, 36, 4, ls, C_RED);
    char sc[16]; snprintf(sc, sizeof(sc), "%d", s_breakout.score);
    fb_text(F14, 260, 2, sc, C_YELLOW);
    if (s_breakout.flags & 1) fb_text_cx(0, LCD_W, LCD_H / 2, "GAME OVER", F14, C_RED);
    toast_draw(); push_screen();
}

// --- Minesweeper ---
static color_t _ms_digit_color(int n)
{
    switch (n) {
    case 1: return C_BLUE;
    case 2: return C_GREEN;
    case 3: return C_RED;
    case 4: return C_BLUE;
    case 5: return C_RED;
    case 6: return C_CYAN;
    case 7: return C_BLACK;
    case 8: return C_DGRAY;
    default: return C_BLACK;
    }
}

static void render_minesweeper(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_DGRAY);

    // 20×11, cell 15×15, x=10, y=30
    const int gx = 10, gy = 30, cs = 15;
    for (int y = 0; y < 11; y++) {
        for (int x = 0; x < 20; x++) {
            int v = s_ms.grid[y * 20 + x];
            int px = gx + x * cs, py = gy + y * cs;
            if (v == 0) {
                fb_fill(px + 1, py + 1, cs - 2, cs - 2, C_GRAY);
            } else if (v >= 1 && v <= 8) {
                fb_fill(px + 1, py + 1, cs - 2, cs - 2, C_WHITE);
                char d[2]; d[0] = '0' + v; d[1] = 0;
                fb_text_cx(px + 1, cs - 2, py + 2, d, F10B, _ms_digit_color(v));
            } else if (v == 9) {
                fb_fill(px + 1, py + 1, cs - 2, cs - 2, C_RED);
                fb_text_cx(px + 1, cs - 2, py + 2, "*", F10B, C_BLACK);
            } else if (v == 10) {
                fb_fill(px + 1, py + 1, cs - 2, cs - 2, C_GRAY);
                fb_text_cx(px + 1, cs - 2, py + 2, "F", F10B, C_YELLOW);
            }
        }
    }
    // 光标
    {
        int cx = gx + s_ms.cursorX * cs, cy = gy + s_ms.cursorY * cs;
        fb_rect(cx, cy, cs, cs, C_YELLOW);
    }
    char mc[12]; snprintf(mc, sizeof(mc), "Mines:%d", s_ms.mines_count);
    fb_text(F12, 4, 4, mc, C_WHITE);
    if (s_ms.flags & 1) fb_text(F12, 120, 4, "WIN!", C_GREEN);
    if (s_ms.flags & 2) fb_text_cx(0, LCD_W, LCD_H - 16, "GAME OVER", F12, C_RED);
    toast_draw(); push_screen();
}

// --- Flappy Bird ---
// 主机画布 240×135 → C3 320×240 等比例缩放
static void render_flappy(void)
{
    s_last_render = now_ms();
    const float sx = (float)LCD_W / 240.0f;   // 1.333
    const float sy = (float)LCD_H / 135.0f;   // 1.778

    // 天空蓝 + 地面棕 (主机: 地面 h=5 at y=130..135)
    int ground_y = (int)((135 - 5) * sy);
    fb_fill(0, 0, LCD_W, ground_y, C_CYAN);
    fb_fill(0, ground_y, LCD_W, LCD_H - ground_y, C_WOOD);

    // 水管: 主机 pipe.w=20, gap=40 (固定)
    const int HOST_PW = 20;
    const int HOST_GAP = 40;
    int pw = (int)(HOST_PW * sx);
    int gap_px = (int)(HOST_GAP * sy);
    for (int i = 0; i < s_flappy.pipe_count; i++) {
        // pipe.x: signed int 像素值 (C3 (int16_t) 还原), 直接乘 sx
        int px = (int)((int16_t)s_flappy.pipes[i].x * sx);
        if (px < -pw - 10 || px > LCD_W + 10) continue;
        // gapY: 主机整数 → C3 像素 (int8_t 还原 signed)
        int gapY = (int)((int8_t)s_flappy.pipes[i].gapY * sy);
        int h_h = (int)(135.0f * sy);  // C3 主机画布对应高度
        int pipe_bottom_y = gapY + gap_px;
        int bottom_h = h_h - pipe_bottom_y;
        if (gapY > 0) fb_fill(px, 0, pw, gapY, C_GREEN);
        if (bottom_h > 0) fb_fill(px, pipe_bottom_y, pw, bottom_h, C_GREEN);
    }

    // 小鸟: 主机固定 x=20, 尺寸 10×10
    int bird_x = (int)(20.0f * sx);
    float by_f = (float)(int16_t)s_flappy.birdY / 256.0f;
    int by = (int)(by_f * sy);
    int bw = (int)(10.0f * sx);   // 鸟宽度
    int bh = (int)(10.0f * sy);   // 鸟高度
    fb_fill(bird_x, by, bw, bh, C_YELLOW);
    // 简单三角形嘴
    fb_fill(bird_x + bw, by + bh / 2 - 1, (int)(3 * sx), (int)(3 * sy), C_ORANGE);

    // 分数
    char sc[8]; snprintf(sc, sizeof(sc), "%d", s_flappy.score);
    fb_text_cx(0, LCD_W, 4, sc, F14, C_WHITE);
    if (s_flappy.flags & 1) fb_text_cx(0, LCD_W, LCD_H / 2, "GAME OVER", F14, C_RED);
    toast_draw(); push_screen();
}

// --- Space Invaders ---
// 注意: alive_mask 由主机按 resetGame push_back 顺序(行优先)发送
// 主机 rows=4, cols = w/30 - 2 ≈ 6, 总共 24 个外星人
// 每个 alien 间距固定 col=25, row=15, 从第一个 alive 的 baseX/rowY 重建
static int _invader_alive_host(int r, int c)
{
    int idx = r * 6 + c;
    if (idx >= 55) return 0;
    int byte_off = idx / 8, bit = idx % 8;
    if (byte_off >= 7) return 0;
    return (s_inv.alive_mask[byte_off] >> bit) & 1;
}

static void render_invaders(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    // 主机 240×135 → C3 320×240 等比例缩放
    const float sx = (float)LCD_W / 240.0f;   // 1.333
    const float sy = (float)LCD_H / 135.0f;   // 1.778

    // Q8.8 定点还原
    float baseX = (float)(int16_t)s_inv.alien_baseX / 256.0f;
    float rowY  = (float)(int16_t)s_inv.alien_rowY  / 256.0f;

    // 主机外星人群固定参数
    const int HOST_ROWS = 4;
    const int HOST_COLS = 6;
    const float COL_STEP = 25.0f;
    const float ROW_STEP = 15.0f;

    color_t row_cols[] = { C_CYAN, C_MAGENTA, C_YELLOW, C_RED };

    for (int r = 0; r < HOST_ROWS; r++) {
        for (int c = 0; c < HOST_COLS; c++) {
            if (!_invader_alive_host(r, c)) continue;
            float hx = baseX + c * COL_STEP;
            float hy = rowY + r * ROW_STEP;
            int px = (int)(hx * sx);
            int py = (int)(hy * sy);
            color_t col = row_cols[r % 4];
            fb_disc(px + 5, py + 4, 5, col);
            fb_fill(px + 2, py + 9, 12, 4, col);
        }
    }

    // 玩家飞船 (底部, 主机 y = height - 10 = 125)
    float hpx = (float)(uint16_t)s_inv.playerX / 256.0f;
    int px = (int)(hpx * sx);
    int py = (int)((135 - 10) * sy);
    fb_fill(px, py, 20, 4, C_CYAN);
    fb_fill(px + 4, py - 5, 12, 5, C_CYAN);
    fb_fill(px + 8, py - 10, 4, 5, C_CYAN);

    // 玩家子弹
    for (int i = 0; i < s_inv.bullet_count && i < 5; i++) {
        int bx = (int)s_inv.bullets[i][0] * sx;
        int by = (int)s_inv.bullets[i][1] * sy;
        fb_fill(bx, by, 2, 5, C_WHITE);
    }

    // HUD
    char sc[16]; snprintf(sc, sizeof(sc), "SCORE: %d", s_inv.score);
    fb_text(F12, 4, 4, sc, C_CYAN);
    // 方向指示
    if (s_inv.alien_dir > 0) fb_text(F12, 270, 4, "->", C_DGRAY);
    else fb_text(F12, 270, 4, "<-", C_DGRAY);

    if (s_inv.flags & 1) fb_text_cx(0, LCD_W, LCD_H / 2, "GAME OVER", F14, C_RED);
    toast_draw(); push_screen();
}

// --- Race (2D 俯视简洁版) ---
static void render_race(void)
{
    s_last_render = now_ms();

    // 背景 + 上下黑边 (HUD 区域)
    fb_fill(0, 0, LCD_W, LCD_H, C_DGRAY);      // 草地基底
    fb_fill(0, 0, LCD_W, 40, C_BLACK);         // 顶部 HUD 黑条
    fb_fill(0, 205, LCD_W, 35, C_BLACK);       // 底部黑边

    // 路面: 中间灰色宽条 + 白色路沿
    fb_fill(60, 40, 200, 165, C_GRAY);
    fb_rect(60, 40, 200, 165, C_WHITE);

    // 中心虚线 (用 playerZ 做动态滚动感)
    {
        uint32_t pz = ((uint32_t)s_race.playerZ[0] << 16)
                    | ((uint32_t)s_race.playerZ[1] << 8)
                    | s_race.playerZ[2];
        int segL = 200;
        int offset = (pz / segL) % 16;
        for (int y = 40 - offset + 16; y < 205; y += 16)
            fb_hline(158, y, 4, C_WHITE);

        // 弯道指示 (用 cur_curve Q8.8)
        int curve = (int)(int16_t)s_race.cur_curve;
        if (curve > 64)       fb_text_cx(60, 200, 215, ">>>", F12, C_YELLOW);
        else if (curve < -64) fb_text_cx(60, 200, 215, "<<<", F12, C_YELLOW);
    }

    // 玩家车 (底部偏中央, 根据 playerX 轻微偏移)
    {
        int16_t px = (int16_t)s_race.playerX;   // host 存的 signed offset
        // px 范围 ±2000 (roadW), 映射到路面 ±100 像素
        int car_x = LCD_W / 2 + px * 30 / 2000 - 10;
        if (car_x < 70) car_x = 70;
        if (car_x > 230) car_x = 230;
        fb_fill(car_x, 190, 20, 16, C_CYAN);      // 青色车身
        fb_fill(car_x + 2, 192, 4, 4, C_RED);     // 左尾灯
        fb_fill(car_x + 14, 192, 4, 4, C_RED);    // 右尾灯
        fb_fill(car_x + 6, 190, 8, 3, C_WHITE);   // 挡风玻璃
    }

    // HUD - 速度 / 档位 / 圈数
    {
        int speed_kmh = (int)(s_race.speed / 20);
        char sp[16]; snprintf(sp, sizeof(sp), "%d km/h", speed_kmh);
        fb_text_cx(0, 100, 4, sp, F12, C_CYAN);

        char ge[8]; snprintf(ge, sizeof(ge), "S%d", s_race.gear);
        fb_text_cx(100, 40, 4, ge, F14, C_YELLOW);

        char lp[16]; snprintf(lp, sizeof(lp), "Lap %d/4", s_race.laps + 1);
        fb_text_cx(140, 110, 4, lp, F12, C_WHITE);
    }

    // 右上角小地图 (闭环 + 当前位置点)
    {
        int mx = 255, my = 44, mr = 16;
        fb_rect(mx - mr, my - mr, mr * 2, mr * 2, C_CYAN);
        int cx = mx, cy = my;
        uint32_t pz = ((uint32_t)s_race.playerZ[0] << 16)
                    | ((uint32_t)s_race.playerZ[1] << 8)
                    | s_race.playerZ[2];
        int total = 300 * 200;
        float t = (float)(pz % total) / (float)total;
        float ang = t * 6.28318f - 1.5708f;
        int dx = cx + (int)(cosf(ang) * (mr - 3));
        int dy = cy + (int)(sinf(ang) * (mr - 3));
        fb_fill(dx - 2, dy - 2, 4, 4, C_RED);
        fb_text(F10B, mx - 6, my + mr + 2, "MAP", C_LGRAY);
    }

    // 结束覆盖层
    if (s_race.flags[0] & 1) {
        fb_fill(0, 110, LCD_W, 20, C_BLACK);
        fb_text_cx(0, LCD_W, 112, "GAME OVER", F14, C_RED);
    } else if (s_race.flags[0] & 2) {
        fb_fill(0, 110, LCD_W, 20, C_BLACK);
        fb_text_cx(0, LCD_W, 112, "FINISH! YOU WIN!", F14, C_GREEN);
    }

    toast_draw(); push_screen();
}

// --- FPS Monitor ---
static void render_fps_monitor(void)
{
    s_last_render = now_ms();
    // 地平线: 上半天空/天花板深灰, 下半地板棕色
    fb_fill(0, 0, LCD_W, 120, C_DGRAY);
    fb_fill(0, 120, LCD_W, 120, C_WOOD);
    // 地平线横线
    fb_hline(0, 120, LCD_W, C_BLACK);
    // 准星 + 枪
    fb_text(F10B, 157, 120, "+", C_CYAN);
    // 画敌人 (简化): 距离估算 + 水平投影
    for (int i = 0; i < s_fps.enemy_count; i++) {
        if (!s_fps.enemy_alive[i]) continue;
        float dx = s_fps.enemy_x[i] - s_fps.posX;
        float dy = s_fps.enemy_y[i] - s_fps.posY;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < 0.1f) continue;
        // 投影: 用 dir/plane 计算相对位置 (简化版)
        float relx = dx * s_fps.dirY - dy * s_fps.dirX; // 垂直于视线方向
        float relz = dx * s_fps.dirX + dy * s_fps.dirY; // 沿视线方向
        if (relz < 0.1f) continue; // 太近或身后
        float screen_x = 160.0f + relx * 100.0f / relz;
        float size = 60.0f / relz;
        int sx = (int)screen_x, sy = (int)(120.0f - size);
        int w = (int)(size * 0.6f), h = (int)size;
        if (w < 2) w = 2;
        if (h < 2) h = 2;
        if (sx < -w || sx > LCD_W + w) continue;
        // 根据距离选色
        color_t ec = (dist < 3.0f) ? C_RED : (dist < 6.0f) ? C_ORANGE : C_YELLOW;
        fb_fill(sx - w / 2, sy, w, h, ec);
        // 血条
        if (s_fps.enemy_health[i] > 0) {
            fb_hline(sx - w / 2, sy - 3, w, C_DGRAY);
            fb_hline(sx - w / 2, sy - 3, (w * s_fps.enemy_health[i]) / 100, C_GREEN);
        }
    }
    // 武器指示
    fb_fill(220, 180, 10, 20, C_BLACK);
    fb_fill(220, 180 - s_fps.weapon_frame * 2, 10, 2, C_ORANGE);
    // 血条
    fb_fill(4, 220, 100, 10, C_DGRAY);
    fb_fill(4, 220, s_fps.health, 10, (s_fps.health > 50) ? C_GREEN : (s_fps.health > 20) ? C_YELLOW : C_RED);
    // 等级
    char lv[10]; snprintf(lv, sizeof(lv), "LV%d", s_fps.level);
    fb_text(F12, 110, 220, lv, C_CYAN);
    if (s_fps.flags & 1) fb_text_cx(0, LCD_W, 100, "GAME OVER", F14, C_RED);
    toast_draw(); push_screen();
}

// --- Shogi (将棋) ---
// 主机棋子编码:
//   1..8   黑方(玩家,owner=1) 普通
//   9..16  白方(AI,owner=2)   普通
//   17..24 黑方(owner=1)       成子
//   25..32 白方(owner=2)       成子
// kindOf: v<=8→v, v<=16→v-8, v<=24→v-16, else→v-24
static int _shogi_owner(uint8_t v)
{
    if (v == 0) return 0;
    if (v <= 8) return 1;
    if (v <= 16) return 2;
    if (v <= 24) return 1;
    return 2;
}
static int _shogi_kind(uint8_t v)
{
    if (v == 0) return 0;
    if (v <= 8) return v;
    if (v <= 16) return v - 8;
    if (v <= 24) return v - 16;
    return v - 24;
}
static bool _shogi_promoted(uint8_t v) { return v > 16; }

static const char* _shogi_glyph_cn(uint8_t v)
{
    int k = _shogi_kind(v);
    switch (k) {
    case 1: return _shogi_promoted(v) ? "と" : "歩";  // PAWN
    case 2: return _shogi_promoted(v) ? ""   : "香";    // LANCE (成香简化)
    case 3: return _shogi_promoted(v) ? ""   : "桂";    // KNIGHT
    case 4: return _shogi_promoted(v) ? ""   : "銀";    // SILVER
    case 5: return "金";                                 // GOLD
    case 6: return _shogi_promoted(v) ? "馬" : "角";   // BISHOP
    case 7: return _shogi_promoted(v) ? "龍" : "飛";   // ROOK
    case 8: return (_shogi_owner(v) == 1) ? "王" : "玉"; // KING
    default: return "";
    }
}

static color_t _shogi_piece_color(uint8_t v)
{
    int owner = _shogi_owner(v);
    if (owner == 1) return C_BLUE;   // 玩家(黑方) — 蓝色字
    if (owner == 2) return C_RED;    // AI(白方) — 红色字
    return C_BLACK;
}

static void render_shogi(void)
{
    s_last_render = now_ms();
    // 木色背景
    fb_fill(0, 0, LCD_W, LCD_H, C_WOOD);

    // 9×9 棋盘: cs=24 → 216px 总宽, bx=(320-216)/2=52, by=(240-216)/2=12
    const int cs = 24;
    const int bx = (LCD_W - cs * 9) / 2;
    const int by = (LCD_H - cs * 9) / 2;
    const int bw = cs * 9;

    // 棋盘外框
    fb_rect(bx - 1, by - 1, bw + 2, bw + 2, C_BLACK);
    // 网格线
    for (int i = 1; i < 9; i++) {
        fb_vline(bx + i * cs, by, bw, C_BLACK);
        fb_hline(bx, by + i * cs, bw, C_BLACK);
    }
    // 星位 (3,3)(3,6)(6,3)(6,6)
    static const int stars[4][2] = {{3, 3}, {3, 6}, {6, 3}, {6, 6}};
    for (int i = 0; i < 4; i++) {
        fb_fill(bx + stars[i][1] * cs + cs / 2 - 1,
                by + stars[i][0] * cs + cs / 2 - 1, 2, 2, C_BLACK);
    }

    // 棋子
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            uint8_t v = s_shogi.board[r * 9 + c];
            if (v == 0) continue;
            int px = bx + c * cs + 1, py = by + r * cs + 1;
            // 棋子底色 (owner=1黑方用米色, owner=2白方用白色 — 简化: 都用白底加黑框)
            fb_fill(px, py, cs - 2, cs - 2, C_WHITE);
            fb_rect(px, py, cs - 2, cs - 2, C_BLACK);
            const char* g = _shogi_glyph_cn(v);
            if (g[0]) {
                fb_text_cx(px, cs - 2, py + 4, g, F10B, _shogi_piece_color(v));
            }
        }
    }

    // 光标 (黄色框)
    if (s_shogi.cursor_r < 9 && s_shogi.cursor_c < 9) {
        int cr_px = bx + s_shogi.cursor_c * cs;
        int cr_py = by + s_shogi.cursor_r * cs;
        fb_rect(cr_px, cr_py, cs, cs, C_YELLOW);
    }
    // 选中格 (红色框)
    if (s_shogi.sel_r < 9 && s_shogi.sel_c < 9 && (s_shogi.sel_r | s_shogi.sel_c) != 0xFF) {
        int sr_px = bx + s_shogi.sel_c * cs;
        int sr_py = by + s_shogi.sel_r * cs;
        fb_rect(sr_px + 1, sr_py + 1, cs - 2, cs - 2, C_RED);
    }

    // 左侧持驹区 (玩家 + AI 简化显示数量)
    fb_text(F10B, 2, 4, "AI", C_ORANGE);
    fb_text(F10B, 2, LCD_H - 14, "Me", C_CYAN);
    // 简化: 显示总件数
    {
        int ai_total = 0, me_total = 0;
        for (int i = 0; i < 8; i++) { ai_total += s_shogi.hand[0][i]; me_total += s_shogi.hand[1][i]; }
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", ai_total);
        fb_text(F12, 4, 18, buf, C_ORANGE);
        snprintf(buf, sizeof(buf), "%d", me_total);
        fb_text(F12, 4, LCD_H - 28, buf, C_CYAN);
    }

    // 右侧状态
    int rx = bx + bw + 4;
    if (rx + 60 > LCD_W) rx = bx - 58;  // 空间不够时移到左侧
    fb_text(F10B, rx, 4, "将棋", C_CYAN);
    char st[24];
    snprintf(st, sizeof(st), "Lv%d", 0);  // C3 没有 level 信息, 跳过
    fb_text(F10B, rx, 20, st, C_WHITE);
    fb_text(F10B, rx, 36, s_shogi.turn ? "AI" : "YOU",
            s_shogi.turn ? C_RED : C_GREEN);

    if (s_shogi.game_over) {
        fb_fill(0, 0, LCD_W, LCD_H, C_DGRAY);
        fb_text_cx(0, LCD_W, LCD_H / 2 - 10, "GAME OVER", F14, C_RED);
        if (s_shogi.winner == 1) fb_text_cx(0, LCD_W, LCD_H / 2 + 10, "玩家胜!", F12, C_GREEN);
        else fb_text_cx(0, LCD_W, LCD_H / 2 + 10, "AI 胜!", F12, C_ORANGE);
    }
    toast_draw(); push_screen();
}

// Pong: 逻辑 240x135 → 320x180 等比映射(y+30 居中, 上下黑条放比分/状态)
static void render_pong(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    #define POY 30
    fb_rect(0, POY, LCD_W, 180, C_DGRAY);
    for (int y = POY + 4; y < POY + 176; y += 12) fb_vline(160, y, 7, C_DGRAY);

    // 挡板 4x20 → 5x27, 联机时右挡板(手柄方)青色
    fb_fill(5, POY + s_pong.p1y * 4 / 3, 5, 27, C_WHITE);
    fb_fill(310, POY + s_pong.p2y * 4 / 3, 5, 27, (s_pong.flags & 1) ? C_CYAN : C_WHITE);

    // 球 4x4 → 5x5
    if (s_pong.flags & 2) // 游戏中才画球
        fb_fill(s_pong.ball_x * 4 / 3, POY + s_pong.ball_y * 4 / 3, 5, 5, C_WHITE);

    // 顶部比分
    char sc[16];
    snprintf(sc, sizeof(sc), "%d : %d", s_pong.score1, s_pong.score2);
    fb_text_cx(0, LCD_W, 7, sc, F14, C_WHITE);

    // 底部状态行
    if (!(s_pong.flags & 2)) {
        fb_text_cx(0, LCD_W, 216, "PONG - 选择模式中", F12, C_GRAY);
    } else if (s_pong.flags & 4) {
        const char* wt;
        if (s_pong.flags & 1) wt = (s_pong.winner == 1) ? "键盘方获胜! Enter/A 重开" : "手柄方获胜! Enter/A 重开";
        else wt = (s_pong.winner == 1) ? "你赢了! Enter 重开" : "AI 获胜! Enter 重开";
        fb_text_cx(0, LCD_W, 216, wt, F12, C_YELLOW);
    } else {
        fb_text_cx(0, LCD_W, 216, (s_pong.flags & 1) ? "键盘(左) vs 手柄(右)" : "单人 vs AI", F12, C_GRAY);
    }
    toast_draw();
    push_screen();
}

// ================= Reversi 镜像渲染 =================
// 规则核与 Cardputer app_reversi 同一套: 重算当前方合法点(纯函数, 无同步风险)
static uint64_t rv_legal(const uint8_t* b, int p)
{
    static const int8_t kD[8][2] = {
        {-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}
    };
    uint64_t m = 0;
    int opp = (p == 1) ? 2 : 1;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (b[r * 8 + c]) continue;
            for (int k = 0; k < 8; k++) {
                int i = r + kD[k][0], j = c + kD[k][1], n = 0;
                while (i >= 0 && i < 8 && j >= 0 && j < 8 && b[i * 8 + j] == opp) {
                    i += kD[k][0]; j += kD[k][1]; n++;
                }
                if (n > 0 && i >= 0 && i < 8 && j >= 0 && j < 8 && b[i * 8 + j] == p) {
                    m |= 1ULL << (r * 8 + c);
                    break;
                }
            }
        }
    }
    return m;
}

#define RV_CELL 24
#define RV_BPD  192
#define RV_BSX  8
#define RV_BSY  24
#define RV_RX   (RV_BSX + RV_BPD + 8)   // 208 右栏

static void render_reversi(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    int started = (s_rv.flags & 0x02) != 0;
    int over = (s_rv.flags & 0x04) != 0;

    // 棋盘: 绿底+网格
    fb_fill(RV_BSX - 2, RV_BSY - 2, RV_BPD + 4, RV_BPD + 4, C_DGREEN);
    for (int i = 0; i <= 8; i++) {
        int off = i * RV_CELL - (i == 8 ? 1 : 0);
        fb_hline(RV_BSX, RV_BSY + off, RV_BPD, C_BLACK);
        fb_vline(RV_BSX + off, RV_BSY, RV_BPD, C_BLACK);
    }

    uint64_t legal = (started && !over) ? rv_legal(s_rv.board, s_rv.cur) : 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int px = RV_BSX + c * RV_CELL + RV_CELL / 2;
            int py = RV_BSY + r * RV_CELL + RV_CELL / 2;
            uint8_t v = s_rv.board[r * 8 + c];
            if (v == 1) {
                fb_disc(px, py, RV_CELL / 2 - 2, C_BLACK);
            } else if (v == 2) {
                fb_disc(px, py, RV_CELL / 2 - 2, C_WHITE);
                fb_circle(px, py, RV_CELL / 2 - 2, C_BLACK);
            } else if ((legal >> (r * 8 + c)) & 1) {
                fb_disc(px, py, 2, C_YELLOW);  // 合法点提示
            }
        }
    }

    // 最后一步 + 光标
    if (s_rv.last_r < 8 && s_rv.last_c < 8)
        fb_rect(RV_BSX + s_rv.last_c * RV_CELL, RV_BSY + s_rv.last_r * RV_CELL, RV_CELL, RV_CELL, C_CYAN);
    if (started && !over)
        fb_rect(RV_BSX + s_rv.cx * RV_CELL + 2, RV_BSY + s_rv.cy * RV_CELL + 2,
                RV_CELL - 4, RV_CELL - 4, C_RED);

    // ---------- 右栏 ----------
    char b[24];
    fb_text(F14, RV_RX, 8, "黑白棋", C_CYAN);
    fb_text(F12, RV_RX, 34, (s_rv.flags & 1) ? "双人联机" : "单人 AI",
            (s_rv.flags & 1) ? C_MAGENTA : C_DGRAY);
    snprintf(b, sizeof(b), "黑 %2d", s_rv.black);
    fb_text(F14, RV_RX, 62, b, C_LGRAY);
    snprintf(b, sizeof(b), "白 %2d", s_rv.white);
    fb_text(F14, RV_RX, 84, b, C_WHITE);

    if (!started) {
        if (s_rv.flags & 1) {
            // 联机准备中
            snprintf(b, sizeof(b), "我%s 他%s", (s_rv.flags & 8) ? "OK" : "--",
                     (s_rv.flags & 0x10) ? "OK" : "--");
            fb_text(F12, RV_RX, 116, b, ((s_rv.flags & 0x18) == 0x18) ? C_GREEN : C_YELLOW);
            fb_text(F10B, RV_RX, 138, "A:准备", C_DGRAY);
        } else {
            fb_text(F12, RV_RX, 116, "选择模式中", C_GRAY);
        }
    } else if (over) {
        const char* wt = (s_rv.winner == 3) ? "平局!"
                         : (s_rv.winner == 1 ? "黑方胜!" : "白方胜!");
        fb_text(F14, RV_RX, 116, wt, C_YELLOW);
        fb_text(F10B, RV_RX, 140, (s_rv.flags & 1) ? "双方准备重开" : "Enter重开", C_DGRAY);
    } else {
        fb_text(F12, RV_RX, 116, (s_rv.cur == 1) ? ">>> 黑方走" : "白方走",
                (s_rv.cur == 1) ? C_GREEN : C_LGRAY);
        if (s_rv.flags & 0x20) {
            // pass: 当前方保持不变, 被 pass 的是另一方
            fb_text(F10B, RV_RX, 138, (s_rv.cur == 1) ? "白方pass" : "黑方pass", C_ORANGE);
        }
    }
    toast_draw();
    push_screen();
}

// ================= Connect4 镜像渲染 =================
// 棋盘: 7x6 格 x 30px = 210x180, 居中 (55,38)
#define C4_CELL 30
#define C4_BX   55
#define C4_BY   38

static void render_connect4(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    int started = (s_c4.flags & 0x02) != 0;
    int over = (s_c4.flags & 0x04) != 0;

    // ---- 顶栏 ----
    char b[32];
    fb_text(F14, 8, 4, "四子棋", C_CYAN);
    fb_text(F10B, 76, 8, (s_c4.flags & 1) ? "双人联机" : "单人 AI",
            (s_c4.flags & 1) ? C_MAGENTA : C_DGRAY);
    snprintf(b, sizeof(b), "红 %d : %d 黄", s_c4.score[0], s_c4.score[1]);
    fb_text(F14, 200, 4, b, C_LGRAY);

    // ---- 选列箭头 ----
    if (started && !over && s_c4.sel < 7) {
        int ax = C4_BX + s_c4.sel * C4_CELL + C4_CELL / 2;
        color_t ac = (s_c4.cur == 1) ? C_RED : C_YELLOW;
        for (int i = 0; i < 6; i++)
            fb_hline(ax - 5 + i, C4_BY - 11 + i, 11 - 2 * i, ac);
    }
    // 作弊提示: 绿箭头(AI 建议列)
    if (started && !over && s_cheat_mine && s_c4.hint_col < 7) {
        int ax = C4_BX + s_c4.hint_col * C4_CELL + C4_CELL / 2;
        for (int i = 0; i < 4; i++)
            fb_hline(ax - 3 + i, C4_BY - 24 + i, 7 - 2 * i, C_GREEN);
        fb_text(F10B, 4, C4_BY - 26, "AI", C_GREEN);
    }

    // ---- 蓝色板体 + 圆孔 ----
    fb_fill(C4_BX - 3, C4_BY - 3, C4_CELL * 7 + 6, C4_CELL * 6 + 6, C_BLUE);
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 7; c++) {
            int px = C4_BX + c * C4_CELL + C4_CELL / 2;
            int py = C4_BY + r * C4_CELL + C4_CELL / 2;
            uint8_t v = s_c4.board[r * 7 + c];
            if (v == 1) {
                fb_disc(px, py, 12, C_RED);
                fb_disc(px - 3, py - 3, 3, C_ORANGE);
            } else if (v == 2) {
                fb_disc(px, py, 12, C_YELLOW);
                fb_disc(px - 3, py - 3, 3, C_WHITE);
            } else {
                fb_disc(px, py, 12, C_BLACK);
            }
        }
    }

    // ---- 胜利线 / 最后一步 ----
    if (over && s_c4.win_idx < 42 && s_c4.winner != 3) {
        static const int8_t D[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
        int dr = D[s_c4.win_dir & 3][0], dc = D[s_c4.win_dir & 3][1];
        int r0 = s_c4.win_idx / 7, c0 = s_c4.win_idx % 7;
        for (int k = 0; k < 4; k++) {
            int px = C4_BX + (c0 + dc * k) * C4_CELL + C4_CELL / 2;
            int py = C4_BY + (r0 + dr * k) * C4_CELL + C4_CELL / 2;
            fb_circle(px, py, 13, C_WHITE);
            fb_circle(px, py, 12, C_WHITE);
        }
    } else if (s_c4.last_c < 7 && s_c4.last_r < 6) {
        int px = C4_BX + s_c4.last_c * C4_CELL + C4_CELL / 2;
        int py = C4_BY + s_c4.last_r * C4_CELL + C4_CELL / 2;
        fb_circle(px, py, 13, C_CYAN);
    }

    // ---- 底栏 ----
    if (!started) {
        if (s_c4.flags & 1) {
            snprintf(b, sizeof(b), "我%s 他%s", (s_c4.flags & 8) ? "OK" : "--",
                     (s_c4.flags & 0x10) ? "OK" : "--");
            fb_text_cx(0, LCD_W, 222, b, F12,
                       ((s_c4.flags & 0x18) == 0x18) ? C_GREEN : C_YELLOW);
        } else {
            fb_text_cx(0, LCD_W, 222, "选择模式中", F12, C_GRAY);
        }
    } else if (over) {
        const char* wt = (s_c4.winner == 3) ? "平局!"
                         : (s_c4.winner == 1 ? "红方胜!" : "黄方胜!");
        fb_text_cx(0, LCD_W, 222, wt, F14, C_YELLOW);
    } else {
        fb_text_cx(0, LCD_W, 224, (s_c4.cur == 1) ? ">>> 红方落子" : ">>> 黄方落子",
                   F12, (s_c4.cur == 1) ? C_RED : C_YELLOW);
    }
    toast_draw();
    push_screen();
}

// ================= Checkers 镜像渲染 =================
// 棋盘: 8x8 格 x 25px = 200x200, 左上 (8,18); 右侧留白放状态
#define CK_CELL 25
#define CK_BX   8
#define CK_BY   18

// 本地推算"必须吃子"(与主机规则一致: 兵只向前吃, 王四向吃)
static int ck_c3_capture_at(const uint8_t* b, int r, int c)
{
    uint8_t v = b[r * 8 + c];
    if (!v) return 0;
    int me = (v == 1 || v == 2) ? 1 : 2;
    int8_t d[4][2];
    int nd;
    if (v == 1) { d[0][0] = -1; d[0][1] = -1; d[1][0] = -1; d[1][1] = 1; nd = 2; }
    else if (v == 3) { d[0][0] = 1; d[0][1] = -1; d[1][0] = 1; d[1][1] = 1; nd = 2; }
    else {
        d[0][0] = -1; d[0][1] = -1; d[1][0] = -1; d[1][1] = 1;
        d[2][0] = 1; d[2][1] = -1; d[3][0] = 1; d[3][1] = 1;
        nd = 4;
    }
    for (int i = 0; i < nd; i++) {
        int r1 = r + d[i][0], c1 = c + d[i][1];
        int r2 = r + 2 * d[i][0], c2 = c + 2 * d[i][1];
        if ((unsigned)r2 >= 8 || (unsigned)c2 >= 8 || (unsigned)r1 >= 8 || (unsigned)c1 >= 8) continue;
        uint8_t mv = b[r1 * 8 + c1];
        int ms = (mv == 1 || mv == 2) ? 1 : ((mv == 3 || mv == 4) ? 2 : 0);
        if (ms && ms != me && b[r2 * 8 + c2] == 0) return 1;
    }
    return 0;
}

static int ck_c3_side_capture(const uint8_t* b, int side)
{
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            uint8_t v = b[r * 8 + c];
            int s = (v == 1 || v == 2) ? 1 : ((v == 3 || v == 4) ? 2 : 0);
            if (s == side && ck_c3_capture_at(b, r, c)) return 1;
        }
    return 0;
}

static void render_checkers(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    int started = (s_ck.flags & 0x02) != 0;
    int over = (s_ck.flags & 0x04) != 0;

    // ---- 顶栏 ----
    char b[40];
    fb_text(F14, 8, 2, "西洋跳棋", C_CYAN);
    fb_text(F10B, 100, 6, (s_ck.flags & 1) ? "双人联机" : "单人 AI",
            (s_ck.flags & 1) ? C_MAGENTA : C_DGRAY);
    int n1 = 0, n2 = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t v = s_ck.board[i];
        if (v == 1 || v == 2) n1++;
        else if (v == 3 || v == 4) n2++;
    }
    snprintf(b, sizeof(b), "红%d %d:%d %d白", n1, s_ck.score[0], s_ck.score[1], n2);
    fb_text(F10B, 214, 6, b, C_LGRAY);

    // ---- 棋盘格 ----
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            fb_fill(CK_BX + c * CK_CELL, CK_BY + r * CK_CELL, CK_CELL, CK_CELL,
                    ((r + c) & 1) ? C_BRICK : C_WOOD);

    // ---- 最后一步: 橙框 ----
    if (s_ck.last_from < 64 && s_ck.last_to < 64) {
        fb_rect(CK_BX + (s_ck.last_from % 8) * CK_CELL, CK_BY + (s_ck.last_from / 8) * CK_CELL,
                CK_CELL, CK_CELL, C_ORANGE);
        fb_rect(CK_BX + (s_ck.last_to % 8) * CK_CELL, CK_BY + (s_ck.last_to / 8) * CK_CELL,
                CK_CELL, CK_CELL, C_ORANGE);
    }

    // ---- 合法落点: 绿点 ----
    if (s_ck.flags & 0x40) {
        for (int i = 0; i < 64; i++) {
            if (!(s_ck.dest_mask[i / 8] & (1 << (i % 8)))) continue;
            int px = CK_BX + (i % 8) * CK_CELL + CK_CELL / 2;
            int py = CK_BY + (i / 8) * CK_CELL + CK_CELL / 2;
            fb_disc(px, py, 4, C_GREEN);
        }
    }

    // ---- 棋子 ----
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t v = s_ck.board[r * 8 + c];
            if (!v) continue;
            int px = CK_BX + c * CK_CELL + CK_CELL / 2;
            int py = CK_BY + r * CK_CELL + CK_CELL / 2;
            color_t body = (v == 1 || v == 2) ? C_RED : C_WHITE;
            fb_disc(px, py, 10, body);
            fb_circle(px, py, 10, C_BLACK);
            fb_disc(px - 3, py - 3, 2, (v == 1 || v == 2) ? C_ORANGE : C_LGRAY);  // 高光
            if (v == 2 || v == 4) {  // 王: 环 + K
                color_t kc = (v == 2) ? C_YELLOW : C_DGRAY;
                fb_circle(px, py, 6, kc);
                fb_circle(px, py, 5, kc);
                fb_text(F10B, px - 3, py - 5, "K", kc);
            }
        }
    }

    // ---- 选中框(连跳锁=品红) ----
    if (s_ck.sel < 64) {
        color_t sc = (s_ck.flags & 0x80) ? C_MAGENTA : C_YELLOW;
        fb_rect(CK_BX + (s_ck.sel % 8) * CK_CELL, CK_BY + (s_ck.sel / 8) * CK_CELL,
                CK_CELL, CK_CELL, sc);
        fb_rect(CK_BX + (s_ck.sel % 8) * CK_CELL + 1, CK_BY + (s_ck.sel / 8) * CK_CELL + 1,
                CK_CELL - 2, CK_CELL - 2, sc);
    }
    // ---- 光标 ----
    if (started && !over && s_ck.cursor < 64)
        fb_rect(CK_BX + (s_ck.cursor % 8) * CK_CELL, CK_BY + (s_ck.cursor / 8) * CK_CELL,
                CK_CELL, CK_CELL, C_CYAN);

    // ---- 作弊: AI 建议(from→to 绿线) ----
    if (started && !over && s_cheat_mine && s_ck.hint_from < 64 && s_ck.hint_to < 64) {
        int x0 = CK_BX + (s_ck.hint_from % 8) * CK_CELL + CK_CELL / 2;
        int y0 = CK_BY + (s_ck.hint_from / 8) * CK_CELL + CK_CELL / 2;
        int x1 = CK_BX + (s_ck.hint_to % 8) * CK_CELL + CK_CELL / 2;
        int y1 = CK_BY + (s_ck.hint_to / 8) * CK_CELL + CK_CELL / 2;
        fb_line(x0, y0, x1, y1, C_GREEN);
        fb_circle(x1, y1, 7, C_GREEN);
        fb_text(F10B, x0 + 4, y0 - 12, "AI", C_GREEN);
    }

    // ---- 底栏 ----
    if (!started) {
        if (s_ck.flags & 1) {
            snprintf(b, sizeof(b), "我%s 他%s", (s_ck.flags & 8) ? "OK" : "--",
                     (s_ck.flags & 0x10) ? "OK" : "--");
            fb_text_cx(0, LCD_W, 226, b, F12,
                       ((s_ck.flags & 0x18) == 0x18) ? C_GREEN : C_YELLOW);
        } else {
            fb_text_cx(0, LCD_W, 226, "选择模式中", F12, C_GRAY);
        }
    } else if (over) {
        const char* wt = (s_ck.winner == 3) ? "和棋!"
                         : (s_ck.winner == 1 ? "红方胜!" : "白方胜!");
        fb_text_cx(0, LCD_W, 226, wt, F14, C_YELLOW);
    } else if (s_ck.flags & 0x80) {
        fb_text_cx(0, LCD_W, 226, "连跳! 继续吃", F12, C_MAGENTA);
    } else if (ck_c3_side_capture(s_ck.board, s_ck.cur)) {
        fb_text_cx(0, LCD_W, 226, (s_ck.cur == 1) ? "红方走 · 必须吃子" : "白方走 · 必须吃子",
                   F12, C_YELLOW);
    } else {
        fb_text_cx(0, LCD_W, 226, (s_ck.cur == 1) ? ">>> 红方走" : ">>> 白方走",
                   F12, (s_ck.cur == 1) ? C_RED : C_WHITE);
    }
    toast_draw();
    push_screen();
}

// ================= Tank 镜像渲染 =================
// 场区: 13x13 格 x 16px = 208x208, 居中 (56,16)
#define TK_TILE 16
#define TK_FIELD 208
#define TK_FX 56
#define TK_FY 6

static void render_tank(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    int started = (s_tk.flags & 0x02) != 0;
    int over = (s_tk.flags & 0x04) != 0;

    // 墙体
    for (int r = 0; r < 13; r++) {
        for (int c = 0; c < 13; c++) {
            int px = TK_FX + c * TK_TILE, py = TK_FY + r * TK_TILE;
            if (tank_steel(r, c)) {
                fb_fill(px, py, TK_TILE, TK_TILE, C_STEEL);
                fb_rect(px, py, TK_TILE, TK_TILE, C_DGRAY);
            } else if (tank_brick(s_tk.brick, r, c)) {
                fb_fill(px, py, TK_TILE, TK_TILE, C_BRICK);
                fb_hline(px, py + TK_TILE / 2, TK_TILE, C_RED);
            }
        }
    }

    // 子弹 (4x4)
    if (started && !over) {
        for (int i = 0; i < s_tk.nbullet && i < 6; i++) {
            int bx = s_tk.bullet[i][0], by = s_tk.bullet[i][1];
            if (bx == 255) continue;
            fb_fill(TK_FX + bx * 2, TK_FY + by * 2, 4, 4, C_YELLOW);
        }
    }

    // 坦克 (16x16)
    if (started) {
        for (int i = 0; i < 2; i++) {
            if (!s_tk.t[i][3]) continue;
            int px = TK_FX + s_tk.t[i][0] * 2, py = TK_FY + s_tk.t[i][1] * 2;
            color_t body = (i == 0) ? C_CYAN : C_MAGENTA;
            fb_fill(px, py, TK_TILE, TK_TILE, body);
            fb_rect(px, py, TK_TILE, TK_TILE, C_BLACK);
            // 炮管 4x10 方向指示
            int d = s_tk.t[i][2];
            int cx = px + TK_TILE / 2 - 2, cy = py + TK_TILE / 2 - 2;
            if (d == 0) fb_fill(cx, py - 6, 4, 10, C_WHITE);
            else if (d == 1) fb_fill(cx, py + TK_TILE - 4, 4, 10, C_WHITE);
            else if (d == 2) fb_fill(px - 6, cy, 10, 4, C_WHITE);
            else fb_fill(px + TK_TILE - 4, cy, 10, 4, C_WHITE);
        }
    }

    // ---------- 底部信息(单行, 避免超出 240) ----------
    int y = TK_FY + TK_FIELD + 6;
    char b[40];
    snprintf(b, sizeof(b), "P1 %d命 %d杀", s_tk.lives[0], s_tk.kills[0]);
    fb_text(F10B, 4, y, b, C_CYAN);
    snprintf(b, sizeof(b), "P2 %d命 %d杀", s_tk.lives[1], s_tk.kills[1]);
    fb_text(F10B, LCD_W - 4 - u8g2_str_width(F10B, b), y, b, C_MAGENTA);

    if (over) {
        const char* wt = (s_tk.winner == 1) ? "P1 胜利!" : "P2 胜利!";
        fb_text_cx(0, LCD_W, y, wt, F14, C_YELLOW);
    } else if (!started) {
        fb_text_cx(0, LCD_W, y, (s_tk.flags & 1) ? "等待手柄.." : "", F10B, C_GRAY);
    }

    toast_draw();
    push_screen();
}

// ================= UNO =================
// 牌 id: 0..99 = 4色x25张(color=id/25+1), 100..103=变色, 104..107=罚4变色
static int uno_card_color(int id) { return id < 100 ? id / 25 + 1 : 0; }
static int uno_card_value(int id)
{
    if (id >= 100) return id >= 104 ? 14 : 13;
    int r = id % 25;
    if (r == 0) return 0;
    return (r - 1) / 2 + 1;   // 1..9, 10=跳过 11=反转 12=罚2
}

static const color_t kUnoColor[5] = { CI_DGRAY, C_RED, C_YELLOW, C_GREEN, C_BLUE };

static void uno_draw_card(int x, int y, int w, int h, int id, int hidden)
{
    if (hidden) {
        fb_fill(x, y, w, h, C_DGRAY);
        fb_rect(x, y, w, h, C_WHITE);
        fb_text_cx(x, w, y + h / 2 - 6, "?", F12, C_WHITE);
        return;
    }
    int col = uno_card_color(id);
    if (col == 0) {
        int hw = w / 2, hh = h / 2;
        fb_fill(x, y, hw, hh, C_RED);
        fb_fill(x + hw, y, w - hw, hh, C_YELLOW);
        fb_fill(x, y + hh, hw, h - hh, C_GREEN);
        fb_fill(x + hw, y + hh, w - hw, h - hh, C_BLUE);
    } else {
        fb_fill(x, y, w, h, kUnoColor[col]);
    }
    fb_rect(x, y, w, h, C_BLACK);
    if (w > 8 && h > 8) fb_fill(x + 2, y + 2, w - 4, h - 4, C_WHITE);

    int v = uno_card_value(id);
    char t[8];
    if (v <= 9) { t[0] = (char)('0' + v); t[1] = 0; }
    else if (v == 10) snprintf(t, sizeof(t), "S");
    else if (v == 11) snprintf(t, sizeof(t), "R");
    else if (v == 12) snprintf(t, sizeof(t), "+2");
    else if (v == 13) snprintf(t, sizeof(t), "W");
    else snprintf(t, sizeof(t), "+4");

    color_t fg = (col == 0) ? C_BLACK : kUnoColor[col];
    const uint8_t* f = (h >= 42 && w >= 22) ? F12 : F10B;
    fb_text_cx(x, w, y + h / 2 - 6, t, f, fg);

    if ((v == 13 || v == 14) && w >= 14 && h >= 20) {
        int bw = w - 8, q = bw / 4;
        fb_fill(x + 4, y + h - 7, q, 3, C_RED);
        fb_fill(x + 4 + q, y + h - 7, q, 3, C_YELLOW);
        fb_fill(x + 4 + q * 2, y + h - 7, q, 3, C_GREEN);
        fb_fill(x + 4 + q * 3, y + h - 7, bw - q * 3, 3, C_BLUE);
    }
}

static void render_uno(void)
{
    s_last_render = now_ms();
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    char b[48];
    int net = (s_uno.flags & 0x01) != 0;
    int over = (s_uno.flags & 0x08) != 0;
    int p1_peek = (s_uno.flags & 0x40) != 0;   // 键盘方正在偷窥本机手牌
    int p2_peek = (s_uno.flags & 0x80) != 0;   // 本机(手柄方)正在偷窥对手手牌

    // ---------- 顶栏 ----------
    int cur_col = (s_uno.color >= 1 && s_uno.color <= 4) ? s_uno.color : 0;
    fb_fill(4, 3, 16, 16, kUnoColor[cur_col]);
    fb_rect(4, 3, 16, 16, C_WHITE);
    if (s_uno.pending > 0) {
        snprintf(b, sizeof(b), "罚 %d 张", s_uno.pending);
        fb_text(F10B, 26, 5, b, C_ORANGE);
    } else {
        snprintf(b, sizeof(b), "牌堆 %d", s_uno.deck);
        fb_text(F10B, 26, 5, b, C_DGRAY);
    }
    snprintf(b, sizeof(b), "第%d局 %d:%d", s_uno.round, s_uno.score[0], s_uno.score[1]);
    fb_text(F10B, LCD_W - 4 - u8g2_str_width(F10B, b), 5, b, C_CYAN);

    // ---------- 弃牌堆 ----------
    fb_fill(18, 28, 74, 92, C_PANEL);
    fb_rect(18, 28, 74, 92, C_TREND);
    uno_draw_card(26, 34, 58, 80, s_uno.top, 0);

    // ---------- 右栏信息 ----------
    int ix = 104;
    snprintf(b, sizeof(b), "对手 %d 张", s_uno.hn[0]);
    fb_text(F12, ix, 30, b, C_MAGENTA);
    snprintf(b, sizeof(b), "你   %d 张", s_uno.hn[1]);
    fb_text(F12, ix, 50, b, C_CYAN);
    fb_text(F12, ix, 70, net ? "双人联机" : "单人 vs AI", C_DGRAY);
    if (over) {
        fb_text(F12, ix, 90, s_uno.winner == 1 ? "你赢了" : "对方赢了", C_YELLOW);
    } else if (s_uno.flags & 0x20) {
        fb_text(F12, ix, 90, "你已喊 UNO!", C_YELLOW);
    } else if (s_uno.flags & 0x10) {
        fb_text(F12, ix, 90, "对方喊了 UNO!", C_ORANGE);
    } else {
        fb_text(F12, ix, 90, s_uno.cur == 1 ? "轮到你出牌" : "等对方出牌",
                s_uno.cur == 1 ? C_GREEN : C_MAGENTA);
    }

    // ---------- 本机(P2)手牌 ----------
    int n = s_uno.hn[1];
    if (n > 0) {
        int gap = 2;
        int cw = (LCD_W - 8) / n - gap;
        if (cw > 46) cw = 46;
        if (cw < 14) cw = 14;
        int chh = cw * 3 / 2;
        if (chh > 74) chh = 74;
        int total = n * (cw + gap) - gap;
        int x0 = (LCD_W - total) / 2;
        int y0 = LCD_H - 6 - chh;
        for (int i = 0; i < n; i++) {
            int x = x0 + i * (cw + gap);
            int sel = (i == s_uno.sel && s_uno.cur == 1 && !over && net);
            int y = sel ? y0 - 6 : y0;
            uno_draw_card(x, y, cw, chh, s_uno.hand[1][i], !net);
            if (sel) fb_rect(x - 2, y - 2, cw + 4, chh + 4, C_YELLOW);
        }
    }

    // ---------- 变色选色 ----------
    if (s_uno.choosing == 1) {
        fb_fill(104, 112, 172, 50, C_BLACK);
        fb_rect(104, 112, 172, 50, C_WHITE);
        fb_text(F10B, 110, 114, "选颜色: 左/右 + A 确认", C_WHITE);
        for (int c = 0; c < 4; c++) {
            int x = 112 + c * 40;
            fb_fill(x, 132, 30, 20, kUnoColor[c + 1]);
            fb_rect(x, 132, 30, 20, C_BLACK);
            if (c == s_uno.color_sel) fb_rect(x - 3, 129, 36, 26, C_YELLOW);
        }
    } else if (s_uno.choosing == 0) {
        fb_text(F12, 108, 120, "对方正在选颜色..", C_YELLOW);
    }

    // ---------- 偷窥(本机按住 Y): 看对手手牌 ----------
    if (p2_peek && net) {
        fb_fill(96, 24, 220, 90, C_PANEL);
        fb_rect(96, 24, 220, 90, C_YELLOW);
        fb_text(F10B, 102, 26, "偷窥中: 对手手牌", C_YELLOW);
        int m = s_uno.hn[0];
        int show = m > 9 ? 9 : m;
        for (int i = 0; i < show; i++)
            uno_draw_card(102 + i * 23, 40, 21, 62, s_uno.hand[0][i], 0);
        if (m > 9) {
            snprintf(b, sizeof(b), "+%d", m - 9);
            fb_text(F10B, 300 - u8g2_str_width(F10B, b), 40, b, C_DGRAY);
        }
    }

    // ---------- 被偷窥警告 ----------
    if (p1_peek) {
        int on = (int)((now_ms() / 300) % 2);
        fb_fill(0, 0, LCD_W, 22, on ? C_RED : C_ORANGE);
        fb_text_cx(0, LCD_W, 4, "对方正在偷窥你的手牌!", F12, C_WHITE);
    }

    // ---------- 局末 ----------
    if (over) {
        int wx = 74, wy = 104, ww = 172, wh = 60;
        fb_fill(wx, wy, ww, wh, C_BLACK);
        fb_rect(wx, wy, ww, wh, C_YELLOW);
        const char* wt;
        if (s_uno.match_end) wt = (s_uno.winner == 1) ? "你赢得比赛!" : "对方赢得比赛!";
        else wt = (s_uno.winner == 1) ? "本局你赢!" : "本局对方赢!";
        fb_text_cx(wx, ww, wy + 8, wt, F14, C_YELLOW);
        fb_text_cx(wx, ww, wy + 32, s_uno.match_end ? "Enter 重开新赛" : "Enter 下一局", F10B, C_GRAY);
    }

    toast_draw();
    push_screen();
}

// ================= 界面状态 =================
static int s_settings = 0;      // 设置菜单开着
static int s_sel = 0;           // 菜单选中项
static int s_bl_on = 1;
static uint32_t s_search_anim = 0;

#define MENU_N 5

static const char* xbox_state_text(void)
{
    switch (xbox_state()) {
    case XBOX_ST_SCANNING: return "搜寻中";
    case XBOX_ST_FOUND:    return "已找到";
    case XBOX_ST_CONNECTING: return "连接中";
    case XBOX_ST_CONNECTED:  return "已连接";
    case XBOX_ST_FAILED:   return "失败";
    default:               return "未连接";
    }
}

static color_t xbox_state_color(void)
{
    switch (xbox_state()) {
    case XBOX_ST_CONNECTED: return C_GREEN;
    case XBOX_ST_CONNECTING:
    case XBOX_ST_FOUND:     return C_CYAN;
    case XBOX_ST_SCANNING:  return C_YELLOW;
    case XBOX_ST_FAILED:    return C_RED;
    default:                return C_DGRAY;
    }
}

static void draw_status(void)
{
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    // 标题
    fb_text_cx(0, LCD_W, 34, "五子棋监视器", F14, C_CYAN);
    fb_fill(120, 54, 80, 1, C_TREND);

    // 主机连接状态框
    int bx = 40, bw = LCD_W - 80, by = 70, bh = 54;
    fb_rect(bx, by, bw, bh, s_linked ? C_GREEN : C_DGRAY);
    if (s_linked) {
        fb_disc(bx + 14, by + 18, 4, C_GREEN);
        fb_text(F12, bx + 26, by + 10, "已连接 Cardputer", C_GREEN);
        fb_text(F12, bx + 26, by + 30, view_ready() ? "画面实时镜像中" : "等待画面数据...", C_GRAY);
    } else {
        fb_disc(bx + 14, by + 18, 4, C_YELLOW);
        fb_text(F12, bx + 26, by + 10, "正在搜寻 Cardputer", C_YELLOW);
        int nd = (int)((now_ms() / 400) % 4);
        char dots[5] = "    ";
        for (int i = 0; i < nd; i++) dots[i] = '.';
        fb_text(F12, bx + 26, by + 30, dots, C_YELLOW);
    }

    // Xbox 手柄状态
    char xb[40];
    // 只加密上但一条报告都没收到时, 老实显示"无数据", 别骗人说连好了
    const char* stx = (xbox_state() == XBOX_ST_CONNECTED && xbox_get_input()->reports == 0)
                          ? "已连接·无数据" : xbox_state_text();
    snprintf(xb, sizeof(xb), "Xbox 手柄: %s", stx);
    fb_text_cx(0, LCD_W, 142, xb, F12, xbox_state_color());
    if (xbox_is_connected()) {
        if (xbox_battery() == 255)
            snprintf(xb, sizeof(xb), "电量 --  按键数 %u", (unsigned)(xbox_get_input()->reports & 0xFFFF));
        else
            snprintf(xb, sizeof(xb), "电量 %d%%  按键数 %u", xbox_battery(),
                     (unsigned)(xbox_get_input()->reports & 0xFFFF));
        fb_text_cx(0, LCD_W, 160, xb, F10B, C_LGRAY);
        // 实时按键: 按下去这里立刻变(用来确认链路真的通了)
        const xbox_input_t* in = xbox_get_input();
        char kb[48] = "";
        if (in->a) strcat(kb, "A ");
        if (in->b) strcat(kb, "B ");
        if (in->x) strcat(kb, "X ");
        if (in->y) strcat(kb, "Y ");
        if (in->lb) strcat(kb, "LB ");
        if (in->rb) strcat(kb, "RB ");
        if (in->up) strcat(kb, "上");
        if (in->down) strcat(kb, "下");
        if (in->left) strcat(kb, "左");
        if (in->right) strcat(kb, "右");
        if (in->menu) strcat(kb, "菜单 ");
        if (in->view) strcat(kb, "视图 ");
        fb_text_cx(0, LCD_W, 176, kb[0] ? kb : "按键: 无输入", F10B, kb[0] ? C_GREEN : C_DGRAY);
    } else if (xbox_need_pair_button()) {
        fb_text_cx(0, LCD_W, 160, "手柄灯快闪: 长按配对键 3 秒", F12, C_YELLOW);
    } else if (xbox_has_saved()) {
        fb_text_cx(0, LCD_W, 160, "已记住配对, 手柄开机自动连", F10B, C_DGRAY);
    }

    fb_text_cx(0, LCD_W, 192, "短按按键: 打开设置", F12, C_GRAY);

    draw_signal_right(318, 204);
    draw_link_stats();

    draw_batt_row();
    toast_draw();
    push_screen();
}

static void draw_settings(void)
{
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    fb_text_cx(0, LCD_W, 18, "设置", F14, C_CYAN);
    fb_fill(120, 40, 80, 1, C_TREND);

    char xb[40];
    snprintf(xb, sizeof(xb), "Xbox: %s", xbox_state_text());
    const char* items[MENU_N] = {
        "重新搜索主机",
        xb,
        s_bl_on ? "背光: 开" : "背光: 关",
        "关于系统",
        "返回",
    };
    for (int i = 0; i < MENU_N; i++) {
        int y = 52 + i * 30;
        bool sel = (i == s_sel);
        if (sel) {
            fb_fill(36, y - 1, LCD_W - 72, 26, C_SELBG);
            fb_rect(36, y - 1, LCD_W - 72, 26, C_YELLOW);
            fb_fill(36, y - 1, 3, 26, C_YELLOW);
        }
        color_t fg = sel ? C_YELLOW : C_WHITE;
        if (i == 1) fg = sel ? C_YELLOW : xbox_state_color();
        fb_text_cx(0, LCD_W, y + 4, items[i], F12, fg);
    }

    fb_text_cx(0, LCD_W, 206, "短按: 下一项   双击: 确认", F10B, C_GRAY);
    fb_text_cx(0, LCD_W, 222, "三击: 返回", F10B, C_DGRAY);
    toast_draw();
    push_screen();
}

// ---------------- 关于系统 ----------------
static void draw_about(void)
{
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

    // 盾牌图标
    int cx = LCD_W / 2, cy = 52;
    int w = 26, h = 30;
    fb_line(cx - w, cy - h, cx + w, cy - h, C_CYAN);
    fb_line(cx + w, cy - h, cx + w, cy + 2, C_CYAN);
    fb_line(cx + w, cy + 2, cx, cy + h, C_CYAN);
    fb_line(cx, cy + h, cx - w, cy + 2, C_CYAN);
    fb_line(cx - w, cy + 2, cx - w, cy - h, C_CYAN);
    fb_line(cx - w + 6, cy - 8, cx, cy - 20, C_CYAN);
    fb_line(cx, cy - 20, cx + w - 6, cy - 8, C_CYAN);
    fb_line(cx, cy - 20, cx, cy + 12, C_CYAN);

    fb_text_cx(0, LCD_W, 90, OS_NAME, F14, C_WHITE);
    fb_text_cx(0, LCD_W, 110, OS_NAME_CN " · 战术终端  " OS_VERSION, F10B, C_GRAY);
    fb_fill(90, 126, 140, 1, C_TREND);

    char buf[48];
    snprintf(buf, sizeof(buf), "硬件: ESP32-C3   屏幕: 320x240");
    fb_text_cx(0, LCD_W, 136, buf, F10B, C_LGRAY);
    snprintf(buf, sizeof(buf), "内存: 剩余 %d KB", (int)(esp_get_free_heap_size() / 1024));
    fb_text_cx(0, LCD_W, 152, buf, F10B, C_LGRAY);

    char mac[24];
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(buf, sizeof(buf), "MAC: %s", mac);
    fb_text_cx(0, LCD_W, 168, buf, F10B, C_DGRAY);

    uint32_t up = now_ms() / 1000;
    snprintf(buf, sizeof(buf), "已运行 %02u:%02u:%02u", (unsigned)(up / 3600), (unsigned)((up / 60) % 60),
             (unsigned)(up % 60));
    fb_text_cx(0, LCD_W, 184, buf, F10B, C_GRAY);

    fb_text_cx(0, LCD_W, 208, "本机为副屏监视器", F10B, C_GRAY);
    fb_text_cx(0, LCD_W, 224, "三击: 返回", F10B, C_DGRAY);
    toast_draw();
    push_screen();
}

static void leave_menu(void)
{
    s_settings = 0;
    if (s_linked && view_ready()) render_state();
    else draw_status();
}

static void settings_action(void)
{
    if (s_sel == 0) {
        // 重新搜索主机
        if (s_linked) {
            esp_now_send(s_host, (const uint8_t*)"GMKBYE", 6);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        esp_now_del_peer(s_host);
        s_linked = 0;
        s_have_state = 0;
        s_pong_have = 0; s_reversi_have = 0; s_tank_have = 0; s_uno_have = 0; s_c4_have = 0; s_ck_have = 0;
        s_settings = 0;
        draw_status();
    } else if (s_sel == 1) {
        // Xbox 手柄: 未连 → 开始搜寻配对; 已连 → 断开
        if (xbox_is_connected()) {
            xbox_disconnect();
            toast_set("已断开 Xbox 手柄", C_ORANGE, 1500);
        } else {
            toast_set("正在搜寻 Xbox 手柄...", C_YELLOW, 0);
            xbox_start_pairing();
        }
        draw_settings();
    } else if (s_sel == 2) {
        s_bl_on = !s_bl_on;
        gpio_set_level(PIN_BL, s_bl_on);
        draw_settings();
    } else if (s_sel == 3) {
        draw_about();
    } else {
        leave_menu();
    }
}

// ---------------- 按键 ----------------
static int btn_down = 0;
static uint32_t btn_press_ms = 0;
static int tap_count = 0;
static uint32_t tap_deadline = 0;

static void poll_button(void)
{
    int lv = gpio_get_level(PIN_BTN);
    uint32_t now = now_ms();
    {
        static int last_lv = 1;
        if (lv != last_lv) {
            last_lv = lv;
            ESP_LOGI(TAG, "btn level=%d", lv);
        }
    }
    if (!btn_down && lv == 0) {
        btn_down = 1;
        btn_press_ms = now;
    } else if (btn_down && lv == 1) {
        btn_down = 0;
        if (now - btn_press_ms < 500) {
            tap_count++;
            tap_deadline = now + 350;
        }
    }
    if (tap_count > 0 && now > tap_deadline) {
        if (!s_settings) {
            // 任何短按: 打开设置
            s_settings = 1;
            s_sel = 0;
            draw_settings();
        } else {
            if (tap_count == 1) {
                s_sel = (s_sel + 1) % MENU_N;
                draw_settings();
            } else if (tap_count == 2) {
                settings_action();
            } else {
                leave_menu();
            }
        }
        tap_count = 0;
    }
}

// ---------------- 开机动画 ----------------
// 盾牌轮廓: (cx,cy) 为中心, w 半宽, h 半高, 上平下尖
static void fb_shield(int cx, int cy, int w, int h, color_t c)
{
    int top = cy - h, bot = cy + h;
    int shoulder = cy + h / 3;
    fb_line(cx - w, top, cx + w, top, c);
    fb_line(cx + w, top, cx + w, shoulder, c);
    fb_line(cx + w, shoulder, cx, bot, c);
    fb_line(cx, bot, cx - w, shoulder, c);
    fb_line(cx - w, shoulder, cx - w, top, c);
    // 内部: 一柄向下的剑
    fb_line(cx, top + 8, cx, bot - 12, c);
    fb_line(cx - w / 2, cy - h / 4, cx, top + 8, c);
    fb_line(cx, top + 8, cx + w / 2, cy - h / 4, c);
    fb_line(cx - 6, bot - 18, cx + 6, bot - 18, c);
}

static void boot_animation(void)
{
    const uint32_t DUR = 2400;
    const int bar_x = 60, bar_w = 200;
    uint32_t t0 = now_ms();
    char buf[48];

    while (1) {
        uint32_t t = now_ms() - t0;
        if (t > DUR) break;

        fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);

        // 网格背景
        for (int x = 0; x < LCD_W; x += 40) fb_vline(x, 0, LCD_H, C_TMID);
        for (int y = 0; y < LCD_H; y += 40) fb_hline(0, y, LCD_W, C_TMID);

        // 盾牌: 0~600ms 由小到大展开
        int grow = t < 600 ? (int)(t * 52 / 600) : 52;
        if (grow > 4) {
            fb_shield(LCD_W / 2, 60, grow, grow * 5 / 6, C_CYAN);
            if (t > 600) {
                // 呼吸高亮
                int pulse = (int)((t / 120) % 2);
                if (pulse) fb_shield(LCD_W / 2, 60, grow - 6, (grow - 6) * 5 / 6, C_BLUE);
            }
        }

        // 标题逐字出现 (600ms 后)
        if (t > 600) {
            int n = (int)((t - 600) / 60);
            if (n > 8) n = 8;
            char title[10];
            memcpy(title, OS_NAME, n);
            title[n] = '\0';
            fb_text_cx(0, LCD_W, 108, title, F14, C_WHITE);
        }
        if (t > 1100) {
            fb_text_cx(0, LCD_W, 132, OS_NAME_CN " · 战术终端  " OS_VERSION, F10B, C_LGRAY);
        }

        // 进度条
        int pct = (int)((t - 300) * 100 / (DUR - 300));
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        fb_rect(bar_x, 166, bar_w, 10, C_DGRAY);
        fb_fill(bar_x + 2, 168, (bar_w - 4) * pct / 100, 6, C_CYAN);

        snprintf(buf, sizeof(buf), "正在启动系统 %d%%", pct);
        fb_text_cx(0, LCD_W, 184, buf, F10B, C_GRAY);
        fb_text_cx(0, LCD_W, 214, "ESP32-C3  ·  320x240", F10B, C_DGRAY);

        push_screen();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 完成帧
    fb_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    fb_shield(LCD_W / 2, 76, 52, 43, C_CYAN);
    fb_text_cx(0, LCD_W, 138, OS_NAME "  " OS_VERSION, F14, C_WHITE);
    fb_text_cx(0, LCD_W, 166, "系统就绪", F12, C_GREEN);
    push_screen();
    vTaskDelay(pdMS_TO_TICKS(400));
}

void app_main(void)
{
    gpio_config_t io = { 0 };
    io.pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_DC) | (1ULL << PIN_BL);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_BL, 0);

    spi_bus_config_t bus = { 0 };
    bus.mosi_io_num = PIN_MOSI;
    bus.miso_io_num = PIN_MISO;
    bus.sclk_io_num = PIN_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = LCD_W * 8 * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = { 0 };
    dev.clock_speed_hz = 40 * 1000 * 1000;
    dev.mode = 0;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    st7789_init();
    gpio_set_level(PIN_BL, 1);

    // 开机动画
    boot_animation();

    gpio_config_t bin = { 0 };
    bin.pin_bit_mask = (1ULL << PIN_BTN);
    bin.mode = GPIO_MODE_INPUT;
    bin.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&bin);

    draw_status();
    batt_init();
    wifi_espnow_init();
    xbox_init();
    // 配对过就按地址直连; 没配对过则开机扫 30 秒, 手柄进配对模式即可连上
    toast_set(xbox_has_saved() ? "正在自动连接 Xbox 手柄..." : "正在搜寻 Xbox 手柄...",
              C_YELLOW, 0);
    xbox_start_pairing();
    uint32_t boot_scan_at = now_ms();
    draw_status();

    ESP_LOGI(TAG, "monitor start, free heap=%d", (int)esp_get_free_heap_size());

    xbox_state_t prev_xbox = xbox_state();
    uint32_t batt_last = 0;

    while (1) {
        poll_button();

        // 诊断心跳: 主循环是否还在跑
        {
            static uint32_t hb = 0;
            if (now_ms() - hb > 2000) {
                hb = now_ms();
                int fkhz = 0;
                spi_device_get_actual_freq(s_spi, &fkhz);
                ESP_LOGI(TAG, "hb heap=%d link=%d pkt=%u xb=%d btn=%d push=%u rnd=%u err=%u spi=%dkHz rep=%u",
                         (int)esp_get_free_heap_size(), s_linked,
                         (unsigned)s_last_pkt_ms, (int)xbox_state(), gpio_get_level(PIN_BTN),
                         (unsigned)g_push_cnt, (unsigned)g_render_cnt,
                         (unsigned)g_push_err, fkhz,
                         (unsigned)xbox_get_input()->reports);
            }
        }

        // 连接状态变化: 刷新状态页
        {
            static int prev_linked = 0;
            if (s_linked != prev_linked) {
                prev_linked = s_linked;
                if (!s_linked) { s_have_state = 0; s_pong_have = 0; s_reversi_have = 0; s_tank_have = 0; s_uno_have = 0; s_c4_have = 0; s_ck_have = 0; }
                if (!s_settings) draw_status();
            }
        }

        // 每秒回执, 让主机用收包超时快速判定掉线
        {
            static uint32_t ack_last = 0;
            if (s_linked && now_ms() - ack_last > 1000) {
                ack_last = now_ms();
                esp_now_send(s_host, (const uint8_t*)"GMKACK", 6);
            }
        }

        // 手柄 → 主机: 按键掩码(变化才发) + 手柄在线位 + 嘲讽快捷键
        {
            static uint16_t prev_mask = 0;
            static uint32_t btn_last = 0;
            static uint8_t prev_f = 0xFF;
            static uint32_t f_last = 0;
            static uint16_t prev_tb = 0;

            uint16_t m = 0;
            if (xbox_is_connected()) {
                const xbox_input_t* in = xbox_get_input();
                if (in->up) m |= 0x0001;
                if (in->down) m |= 0x0002;
                if (in->left) m |= 0x0004;
                if (in->right) m |= 0x0008;
                if (in->a) m |= 0x0010;
                if (in->b) m |= 0x0020;
                if (in->x) m |= 0x0040;
                if (in->y) m |= 0x0080;
                if (in->lb) m |= 0x0100;
                if (in->rb) m |= 0x0200;
                if (in->lt > 0x200) m |= 0x0400;
                if (in->rt > 0x200) m |= 0x0800;
                if (in->menu) m |= 0x1000;
                if (in->view) m |= 0x2000;
            }

            if (s_linked) {
                // 手柄 Y: 本机(手柄方)作弊辅助开关, 和键盘方的 X 各自独立(仅五子棋/四子棋/跳棋)
                static uint16_t prev_y = 0;
                uint16_t yb = (uint16_t)(m & 0x0080);
                if (yb && !prev_y && (s_game_id == GID_GOMOKU || s_game_id == GID_CONNECT4 || s_game_id == GID_CHECKERS)) {
                    s_cheat_mine = !s_cheat_mine;
                    toast_set(s_cheat_mine ? "作弊辅助: 开" : "作弊辅助: 关",
                              s_cheat_mine ? C_YELLOW : C_DGRAY, 1200);
                }
                prev_y = yb;

                // 还没开局就按方向键 → 提示先准备(仅五子棋; 玩家会以为是方向键坏了)
                static uint16_t prev_dir = 0;
                uint16_t dir = (uint16_t)(m & 0x000F);
                if (dir && !(prev_dir & dir) && s_game_id == GID_GOMOKU && s_have_state &&
                    (s_net_flags & 0x01) && !(s_net_flags & 0x08)) {
                    toast_set("当前还未准备, 请按 A 准备后再动", C_YELLOW, 1600);
                }
                prev_dir = dir;

                if (m != prev_mask && now_ms() - btn_last > 30) {
                    uint8_t pkt[8];
                    memcpy(pkt, "GMKBTN", 6);
                    pkt[6] = (uint8_t)(m & 0xFF);
                    pkt[7] = (uint8_t)(m >> 8);
                    esp_now_send(s_host, pkt, sizeof(pkt));
                    prev_mask = m;
                    btn_last = now_ms();
                }
                // bit0=手柄在线  bit1: 五子棋=作弊辅助开启 / UNO=按住 Y 偷窥
                uint8_t b1 = (s_game_id == GID_UNO) ? (uint8_t)((m & 0x0080) ? 2 : 0)
                                                    : (uint8_t)(s_cheat_mine ? 2 : 0);
                uint8_t f = (uint8_t)((xbox_is_connected() ? 1 : 0) | b1);
                if (f != prev_f || now_ms() - f_last > 1000) {
                    uint8_t pkt[7];
                    memcpy(pkt, "GMKPVP", 6);
                    pkt[6] = f;
                    esp_now_send(s_host, pkt, sizeof(pkt));
                    prev_f = f;
                    f_last = now_ms();
                }
                // 嘲讽: X / LB / RB 按下沿
                uint16_t tb = (uint16_t)(m & 0x0340);
                if (tb && !(prev_tb & tb))
                    esp_now_send(s_host, (const uint8_t*)"GMKTAU", 6);
                prev_tb = tb;
            } else {
                prev_mask = m;
                prev_f = 0xFF;
                prev_tb = 0;
                s_cheat_mine = 0;
            }
        }

        // 电量每 5 秒读一次, 有变化就刷新
        if (now_ms() - batt_last > 5000) {
            batt_last = now_ms();
            int old = s_batt_pct;
            batt_update();
            if (s_batt_pct != old) {
                if (!s_settings) {
                    if (s_linked && view_ready()) render_state();
                    else draw_status();
                } else {
                    draw_settings();
                }
            }
        }

        // Xbox 状态变化: 连贯提示
        {
            xbox_state_t st = xbox_state();
            if (st != prev_xbox) {
                switch (st) {
                case XBOX_ST_SCANNING:
                    if (prev_xbox != XBOX_ST_IDLE) toast_set("正在搜寻 Xbox 手柄...", C_YELLOW, 0);
                    break;
                case XBOX_ST_FOUND:
                    toast_set("已搜索到 Xbox 手柄", C_CYAN, 900);
                    toast_push("正在尝试连接...", C_ORANGE, 0);
                    break;
                case XBOX_ST_CONNECTED:
                    toast_set("已连接上 Xbox 手柄", C_GREEN, 2000);
                    break;
                case XBOX_ST_FAILED:
                    toast_set("连接失败, 请让手柄进入配对模式", C_RED, 2500);
                    break;
                default:
                    break;
                }
                prev_xbox = st;
                if (!s_settings) {
                    if (s_linked && view_ready()) render_state();
                    else draw_status();
                } else {
                    draw_settings();
                }
            }
        }

        // 手柄按键实时反馈: 按下去屏幕立刻弹名字, 证明链路真的通了
        if (xbox_is_connected()) {
            const xbox_input_t* in = xbox_get_input();
            static uint32_t s_prev_mask = 0;
            uint32_t mask = 0;
            if (in->a) mask |= 1u << 0;
            if (in->b) mask |= 1u << 1;
            if (in->x) mask |= 1u << 2;
            if (in->y) mask |= 1u << 3;
            if (in->lb) mask |= 1u << 4;
            if (in->rb) mask |= 1u << 5;
            if (in->up) mask |= 1u << 6;
            if (in->down) mask |= 1u << 7;
            if (in->left) mask |= 1u << 8;
            if (in->right) mask |= 1u << 9;
            if (in->menu) mask |= 1u << 10;
            if (in->view) mask |= 1u << 11;
            uint32_t nu = mask & ~s_prev_mask;
            s_prev_mask = mask;
            // 联机对战时不弹按键提示, 免得盖住事件日志
            if (nu && !(s_net_flags & 0x01)) {
                char tb[24] = "手柄 ";
                const char* nm = "";
                if (nu & (1u << 0)) nm = "A";
                else if (nu & (1u << 1)) nm = "B";
                else if (nu & (1u << 2)) nm = "X";
                else if (nu & (1u << 3)) nm = "Y";
                else if (nu & (1u << 4)) nm = "LB";
                else if (nu & (1u << 5)) nm = "RB";
                else if (nu & (1u << 6)) nm = "上";
                else if (nu & (1u << 7)) nm = "下";
                else if (nu & (1u << 8)) nm = "左";
                else if (nu & (1u << 9)) nm = "右";
                else if (nu & (1u << 10)) nm = "菜单";
                else if (nu & (1u << 11)) nm = "视图";
                strcat(tb, nm);
                toast_set(tb, C_GREEN, 600);
                if (!s_settings) {
                    if (s_linked && view_ready()) render_state();
                    else draw_status();
                } else {
                    draw_settings();
                }
            }
        }

        // 看到手柄但广播不可连接: 提示长按配对键(只提示一次)
        {
            static int pair_btn_tip = 0;
            if (xbox_need_pair_button() && !xbox_is_connected()) {
                if (!pair_btn_tip) {
                    pair_btn_tip = 1;
                    toast_set("检测到手柄, 请长按配对键", C_ORANGE, 4000);
                    if (!s_settings) {
                        if (s_linked && view_ready()) render_state();
                        else draw_status();
                    } else {
                        draw_settings();
                    }
                }
            } else {
                pair_btn_tip = 0;
            }
        }

        // 开机扫描超时就停, 省电(可在设置里重试); 已看到手柄时多给 2 分钟
        if (boot_scan_at) {
            uint32_t limit = xbox_need_pair_button() ? 120000 : 30000;
            if (xbox_is_connected()) {
                boot_scan_at = 0;
            } else if (now_ms() - boot_scan_at > limit) {
                boot_scan_at = 0;
                xbox_disconnect();
                toast_set("未发现 Xbox 手柄, 可在设置中重试", C_DGRAY, 2500);
                if (!s_settings) {
                    if (s_linked && view_ready()) render_state();
                    else draw_status();
                } else {
                    draw_settings();
                }
            }
        }

        // 提示队列推进
        if (toast_tick()) {
            if (!s_settings) {
                if (s_linked && view_ready()) render_state();
                else draw_status();
            } else {
                draw_settings();
            }
        }

        // 新状态包渲染
        if (s_gs_ready) {
            s_gs_ready = 0;
            if (s_linked && !s_settings) render_state();
        }

        // 胜利动画: 本地 130ms 刷新复刻阶段切换(仅五子棋)
        if (s_linked && !s_settings && s_game_id == GID_GOMOKU && s_have_state && (s_gs.flags0 & F0_ANIM)
            && now_ms() - s_last_render > 130) {
            render_state();
        }

        // 作弊横幅滚动: 80ms 重绘一次(仅五子棋)
        if (s_linked && !s_settings && s_game_id == GID_GOMOKU && s_have_state && (s_gs.flags1 & F1_CHEAT)
            && now_ms() - s_last_render > 80) {
            render_state();
        }

        // 掉线检测(3 秒无包)
        if (s_linked && now_ms() - s_last_pkt_ms > 3000) {
            s_linked = 0;
            s_have_state = 0;
            s_pong_have = 0; s_reversi_have = 0; s_tank_have = 0;
            s_uno_have = 0; s_c4_have = 0; s_ck_have = 0;
            s2048_have = 0; s_tetris_have = 0; s_snake_have = 0; s_breakout_have = 0;
            s_ms_have = 0; s_flappy_have = 0; s_inv_have = 0; s_race_have = 0;
            s_fps_have = 0; s_shogi_have = 0;
            if (!s_settings) draw_status();
        }

        // 搜寻/等待动画: 未连上或还没收到画面数据时持续刷新, 避免看起来像卡死
        if ((!s_linked || !view_ready()) && !s_settings && now_ms() - s_search_anim > 400) {
            s_search_anim = now_ms();
            draw_status();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

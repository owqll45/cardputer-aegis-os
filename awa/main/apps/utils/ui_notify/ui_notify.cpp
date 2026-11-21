#include <apps/utils/ui_notify/ui_notify.h>

#include <apps/utils/glink/zdlink.h>
#include <apps/utils/theme.h>
#include <apps/utils/ztrace/ztrace.h>
#include <hal/hal.h>
#include <M5GFX.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace ui_notify {

static constexpr const char* TAG = "NTF";

/* 停留 4 秒；期间每帧由 pushSystemBar 叠加，过期后恢复状态栏 */
static constexpr uint32_t kShowMs = 4000;
/* 堆低于这个值就降级成一行提示（不提示用户，静默降级） */
static constexpr uint32_t kLowHeap = 46000;

static const uint16_t C_BG    = 0x0841;
static const uint16_t C_FG    = 0xE7BE;
static const uint16_t C_DIM   = 0x7BEF;
static const uint16_t C_WARN  = 0xFD20;
static const uint16_t C_LINK_E = (uint16_t)THEME_COLOR_LINK_ESPNOW;  /* ESP-NOW 直连 */
static const uint16_t C_LINK_L = (uint16_t)THEME_COLOR_LINK_LORA;    /* LoRa 空口 */
static const uint16_t C_LINK_R = 0xF81F;                             /* 经 C3 桥 */

struct Info {
    uint32_t src;
    uint8_t  via;
    int8_t   rssi;
    uint16_t msgid;
    uint16_t seq;
    uint32_t rx_ms;   /* 进钩子的时刻, 用于算"收到→提示"延迟 */
    char     text[64];
};

/* 跨任务只传这一份 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_pending = false;
static Info s_pend;
static volatile uint16_t s_unread = 0;
static volatile bool s_open_req = false;

/* 以下只在 app 任务里读写 */
static bool s_active = false;
static bool s_simple = false;
static bool s_suppress = false;
static uint32_t s_low_heap = kLowHeap;
static uint32_t s_start_ms = 0;
static uint32_t s_last_push = 0;
static Info s_cur;

/* 对账计数: 收到多少条 / 真正提示了几次 / 因面板在前台跳过几次 */
static uint32_t s_n_rx = 0, s_n_shown = 0, s_n_skip = 0, s_n_simple = 0;

static uint16_t link_color(uint8_t via)
{
    if (via == ZD_VIA_L) return C_LINK_L;
    if (via == ZD_VIA_R) return C_LINK_R;
    return C_LINK_E;
}

static const char* link_name(uint8_t via)
{
    if (via == ZD_VIA_L) return "LoRa";
    if (via == ZD_VIA_R) return "桥";
    return "直连";
}

/* 链路单字母, 与协议里的 E/L/R 一致 */
static char link_tag()
{
    if (s_cur.via == ZD_VIA_L) return 'L';
    if (s_cur.via == ZD_VIA_R) return 'R';
    return 'E';
}

/* UTF-8 安全裁剪，超宽以 ".." 收尾 */
static void draw_clip(lgfx::LGFX_Sprite& cv, const char* s, int x, int y, int maxw)
{
    if (!s || !*s) return;
    char buf[72];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = 0;

    if ((int)cv.textWidth(buf) <= maxw) {
        cv.drawString(buf, x, y);
        return;
    }
    size_t best = 0, lo = 0, hi = n;
    while (lo <= hi) {
        size_t mid = (lo + hi) / 2;
        buf[mid] = 0;
        if ((int)cv.textWidth(buf) <= maxw - 12) {
            best = mid;
            if (mid == n) break;
            lo = mid + 1;
        } else {
            if (mid == 0) break;
            hi = mid - 1;
        }
    }
    while (best > 0 && ((uint8_t)buf[best] & 0xC0) == 0x80) best--;
    buf[best] = 0;
    strncat(buf, "..", sizeof(buf) - strlen(buf) - 1);
    cv.drawString(buf, x, y);
}

/* zdlink 事件钩子：跑在 WiFi 任务里，只拷贝 */
static void on_zd_event(const ZdEvent& ev)
{
    if (ev.kind != ZD_EV_TEXT && ev.kind != ZD_EV_LTEXT) return;
    /* 自己发出去又被空口收回来的那条不提示 */
    if (ev.kind == ZD_EV_LTEXT && ev.loopback) return;

    portENTER_CRITICAL(&s_mux);
    s_pend.src = ev.src;
    s_pend.via = ev.via;
    s_pend.rssi = ev.rssi;
    s_pend.msgid = ev.msgid;
    s_pend.seq = ev.seq;
    s_pend.rx_ms = zdlink_now_ms();
    /* 摘要只要一句话, 按 UTF-8 边界截到 63B(完整正文仍进消息列表) */
    size_t n = strnlen(ev.text, sizeof(s_pend.text) - 1);
    while (n > 0 && ((uint8_t)ev.text[n] & 0xC0) == 0x80) n--;
    memcpy(s_pend.text, ev.text, n);
    s_pend.text[n] = 0;
    uint16_t u = s_unread;
    if (u < 99) s_unread = (uint16_t)(u + 1);
    s_n_rx++;
    s_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

/* 通知消失后由 ui_sysbar 画状态栏(idle bar 已在那边统一), 这里只负责把顶栏重新推上去。
 * 全屏 app 的顶栏是内容画布上方那 kInlineH 条, 由 ui_sysbar::drawInline 重画,
 * 这里如果推 26px 系统栏会把 app 自己的顶部内容盖掉。 */
/* 前台画布是否为"全屏 app 画布"(高度 = 屏高 - 顶部内联条) */
static bool inline_canvas()
{
    auto& cv = GetHAL().canvas();
    return cv.getBuffer() && cv.height() >= (int)GetHAL().display().height() - 14;
}

static void restore_bar()
{
    if (inline_canvas()) return;
    GetHAL().pushCanvasSystemBar();
}

void init()
{
    zdlink_set_event_hook(on_zd_event);
    /* 系统栏叠加层的唯一所有者是 ui_sysbar: 它会在通知活跃时转调 draw(),
     * 所以这里不要再注册, 否则谁后 init 谁生效, 状态栏会时有时无。 */
}

bool active()
{
    return s_active;
}

uint16_t unread()
{
    return s_unread;
}

void clear_unread()
{
    s_unread = 0;
}

void request_open()
{
    s_open_req = true;
}

bool take_open_request()
{
    if (!s_open_req) return false;
    s_open_req = false;
    return true;
}

void draw(lgfx::LGFX_Sprite& bar)
{
    if (!s_active) return;
    const int w = (int)bar.width();
    const int h = (int)bar.height();
    if (!bar.getBuffer() || w <= 0 || h <= 0) return;

    const uint16_t lc = link_color(s_cur.via);

    bar.fillScreen(C_BG);
    bar.setTextWrap(false);
    bar.setFont(&fonts::efontCN_12);
    bar.setTextDatum(top_left);

    char tag[32];
    snprintf(tag, sizeof(tag), "%s %04lX", link_name(s_cur.via),
             (unsigned long)(s_cur.src & 0xFFFF));

    if (s_simple) {
        /* 降级：一行，只有来源 + 未读数 */
        bar.setTextDatum(middle_left);
        bar.setTextColor(lc);
        bar.drawString(tag, 6, h / 2);
        char b[16];
        snprintf(b, sizeof(b), "新 %u", (unsigned)unread());
        bar.setTextColor(C_FG);
        bar.setTextDatum(middle_right);
        bar.drawString(b, w - 6, h / 2);
        bar.setTextDatum(top_left);
        bar.drawFastHLine(0, h - 1, w, lc);
        return;
    }

    /* 第一行：来源 + 信号 + 右侧操作提示 */
    bar.setTextColor(lc);
    bar.drawString(tag, 6, 0);
    int x = 6 + (int)bar.textWidth(tag) + 6;
    if (s_cur.rssi) {
        char rb[16];
        snprintf(rb, sizeof(rb), "%ddBm", s_cur.rssi);
        bar.setTextColor(C_DIM);
        if (x + (int)bar.textWidth(rb) < w - 80) bar.drawString(rb, x, 0);
    }
    bar.setTextColor(C_DIM);
    bar.setTextDatum(top_right);
    bar.drawString("按 = 打开", w - 6, 0);

    /* 第二行：一句话摘要 */
    bar.setTextDatum(top_left);
    bar.setTextColor(C_FG);
    draw_clip(bar, s_cur.text, 6, 13, w - 12);
    bar.drawFastHLine(0, h - 1, w, lc);
}

/* 全屏 app: 通知只占顶部 kInlineH 条(和状态栏同一块地方, 不压游戏内容)。
 * 一行放下: [链路 来源] 摘要 ... 新N/= */
void draw_inline(lgfx::LGFX_Sprite& bar)
{
    if (!s_active) return;
    const int w = (int)bar.width();
    const int h = (int)bar.height();
    if (!bar.getBuffer() || w <= 0 || h <= 0) return;

    const uint16_t lc = link_color(s_cur.via);

    bar.fillRect(0, 0, w, h, C_BG);
    bar.setTextWrap(false);
    bar.setFont(&fonts::efontCN_10);
    bar.setTextDatum(top_left);

    char tag[24];
    snprintf(tag, sizeof(tag), "%c %04lX", link_tag(), (unsigned long)(s_cur.src & 0xFFFF));
    bar.setTextColor(lc);
    bar.drawString(tag, 2, 0);
    int x = 2 + (int)bar.textWidth(tag) + 4;

    /* 右侧先占位: 有正文显示 "=" 提示, 降级/空正文显示未读数 */
    bar.setTextDatum(top_right);
    bar.setTextColor(C_DIM);
    char rb[12];
    if (s_simple) snprintf(rb, sizeof(rb), "新%u", (unsigned)unread());
    else snprintf(rb, sizeof(rb), "=");
    bar.drawString(rb, w - 3, 0);
    int rw = (int)bar.textWidth(rb) + 6;

    bar.setTextDatum(top_left);
    bar.setTextColor(C_FG);
    /* 中文字形盒是 12 行(10px 宽), 必须从 y=0 起画, 从 y=1 起底部会被 14px 边界切掉 */
    if (s_cur.text[0]) draw_clip(bar, s_cur.text, x, 0, w - x - rw);
    bar.drawFastHLine(0, h - 1, w, lc);
    bar.setTextDatum(top_left);
}

void set_suppressed(bool on)
{
    s_suppress = on;
}

void set_low_heap(uint32_t bytes)
{
    s_low_heap = bytes ? bytes : kLowHeap;
}

bool simple_mode() { return s_simple; }

const char* current_text() { return s_cur.text; }

uint32_t count_rx() { return s_n_rx; }

uint32_t count_shown() { return s_n_shown; }

uint32_t count_skip() { return s_n_skip; }

uint32_t count_simple() { return s_n_simple; }

void update()
{
    uint32_t now = GetHAL().millis();

    bool got = false;
    portENTER_CRITICAL(&s_mux);
    if (s_pending) {
        s_cur = s_pend;
        s_pending = false;
        got = true;
    }
    portEXIT_CRITICAL(&s_mux);

    /* 消息面板在前台: 它自己的顶栏已经显示"新 N", 不再盖一层。
     * 直接丢弃这次提示, 面板每帧自绘, 不需要我们恢复状态栏。 */
    if (s_suppress) {
        if (got) {
            s_n_skip++;
            ZT("ntf", "mode=skip unread=%u via=%c from=%04lX msgid=%u why=panel_open",
               (unsigned)unread(), link_tag(), (unsigned long)(s_cur.src & 0xFFFF),
               (unsigned)s_cur.msgid);
        }
        s_active = false;
        return;
    }

    if (got) {
        s_active = true;
        s_start_ms = now;
        s_last_push = 0;
        /* 堆紧张或没有正文时, 静默降级成一行 */
        s_simple = (esp_get_free_heap_size() < s_low_heap) || (s_cur.text[0] == 0);
        s_n_shown++;
        if (s_simple) s_n_simple++;
        uint32_t lat = (uint32_t)(now - s_cur.rx_ms);
        ZT("rx_msg", "via=%c from=%04lX msgid=%u seq=%u rssi=%d fg=%s heap=%u lat=%ums text=\"%s\"",
           link_tag(), (unsigned long)(s_cur.src & 0xFFFF), (unsigned)s_cur.msgid,
           (unsigned)s_cur.seq, (int)s_cur.rssi, zd_fg_app(),
           (unsigned)esp_get_free_heap_size(), (unsigned)lat, s_cur.text);
        ZT("ntf", "mode=%s unread=%u heap=%u show_ms=%u simple_th=%u",
           s_simple ? "simple" : "full", (unsigned)unread(),
           (unsigned)esp_get_free_heap_size(), (unsigned)kShowMs, (unsigned)s_low_heap);
    }

    if (!s_active) return;

    if (now - s_start_ms >= kShowMs) {
        s_active = false;
        restore_bar();
        ZT("ntf", "mode=idle unread=%u held=%ums", (unsigned)unread(),
           (unsigned)(now - s_start_ms));
        return;
    }

    /* 全屏 app 走内联条: 一行通知由 ui_sysbar::drawInline 画进顶部 kInlineH,
     * 这里绝不能再推 26px 系统栏(那会把游戏自己的前 12px 盖掉)。 */
    if (inline_canvas()) return;

    /* 每个 app 帧重压一次（系统栏只有 26px，代价很小） */
    if ((uint32_t)(now - s_last_push) >= 16) {
        s_last_push = now;
        GetHAL().pushCanvasSystemBar();  /* pushSystemBar 内部会调 draw() 叠加 */
    }
}

}  // namespace ui_notify

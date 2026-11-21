#include <apps/utils/ui_sysbar/ui_sysbar.h>

#include <apps/utils/theme.h>
#include <apps/utils/glink/zdlink.h>
#include <apps/utils/ui_notify/ui_notify.h>
#include <hal/hal.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <ctime>

/* main.cpp 里的两个全局查询: 面板是否前台 / 当前前台 app 名 */
extern bool zdmsg_is_open();
extern const char* zd_fg_app();

namespace ui_sysbar {

/* ===== 顶栏配色（链路两色与三台设备/ADV 同口径，改 theme.h 即可）===== */
static const uint16_t C_BG   = 0x0841;
static const uint16_t C_FG   = 0xE7BE;
static const uint16_t C_DIM  = 0x4208;
static const uint16_t C_BATT = 0x07E0;
static const uint16_t C_LOW  = 0xF800;

/* 链路多久没动静就算不活跃 */
static const uint32_t kLinkWindowMs = 5000;

static uint8_t s_link = 0;
static int8_t s_rssi = 0;
static uint8_t s_bat = 100;
static uint32_t s_last_refresh = 0;
static uint32_t s_pushes = 0;
static uint32_t s_inline_draws = 0;
static uint32_t s_status_draws = 0;
static uint32_t s_last_push = 0;
static bool s_dirty = true;
static bool s_notice_was = false;
static bool s_was_desktop = true;
static bool s_inline = false;
static bool s_inline_notice = false;
static int s_clock_min = -2;
static uint8_t s_force_link = 0;
static uint32_t s_force_until = 0;

static const char* link_name(uint8_t l)
{
    if (l == ZD_VIA_E) return "ESP-NOW";
    if (l == ZD_VIA_L) return "LoRa";
    return "无链路";
}

static uint16_t link_color(uint8_t l)
{
    if (l == ZD_VIA_E) return (uint16_t)THEME_COLOR_LINK_ESPNOW;
    if (l == ZD_VIA_L) return (uint16_t)THEME_COLOR_LINK_LORA;
    return (uint16_t)THEME_COLOR_LINK_NONE;
}

static void clock_hm(char* out, size_t n)
{
    time_t t = ::time(nullptr);
    if ((int64_t)t < 1700000000LL) { /* 2023-11 之前 = 没对过时 */
        snprintf(out, n, "--:--");
        return;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* 当前分钟数, 用来在分钟变化时重画顶栏(时间要跳); 没对时返回 -1 */
static int clock_min()
{
    time_t t = ::time(nullptr);
    if ((int64_t)t < 1700000000LL) return -1;
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

static int signal_bars(uint8_t l)
{
    if (!l) return 0;
    if (s_rssi >= -60) return 4;
    if (s_rssi >= -75) return 3;
    if (s_rssi >= -90) return 2;
    return 1;
}

/* 前台全屏画布登记（共享画布或 app 自己的 sprite 都算） */
static const void* s_app_cv = nullptr;
static int s_app_h = 0;
static void draw_inline(bool force);

void noteAppCanvas(const void* sprite, int h)
{
    s_app_cv = sprite;
    s_app_h = h;
    s_dirty = true;

    /* 进入全屏画布的这一瞬间: 先把顶部内联条画好推上去, 再让 app 推自己的内容。
     * app_task 的顺序是"app 先推内容 → ui_sysbar 后推内联条", 如果等到那时才画,
     * 中间会露出一帧"上半截残留旧桌面栏、下半截已被 app 内容盖住"的混绘。
     * 这里强推一次, 换栏瞬间就只有一个终态。 */
    if (h == (int)GetHAL().display().height() - kInlineH) draw_inline(true);
}

void clearAppCanvas(const void* sprite)
{
    if (s_app_cv != sprite) return;
    s_app_cv = nullptr;
    s_app_h = 0;
    s_dirty = true;
}

const void* appCanvasSprite() { return s_app_cv; }
int appCanvasHeight() { return s_app_h; }

bool inlineMode()
{
    /* 高度必须正好是"屏高 - 内联条"(121): 135/109 都不算, 避免把消息面板这类
     * 自己占满整屏的页面当成内联画布。 */
    if (!s_app_cv) return false;
    if (s_app_h != (int)GetHAL().display().height() - kInlineH) return false;
    /* 兜底: app 关掉时若没来得及 clearAppCanvas, 前台已经是桌面/直画屏 app,
     * 这时必须回到 26px 系统栏, 否则桌面的顶栏就没人画了。 */
    const char* fg = zd_fg_app();
    if (!fg) return false;
    if (strcmp(fg, "Launcher") == 0) return false;
    if (strcmp(fg, "LegacyApp") == 0 || strcmp(fg, "BrainServer") == 0) return false;
    return true;
}

/* 画一整条状态栏：只写 240x26 的系统栏画布, 不新增缓冲, 不碰 app 画布 */
static void draw_status(lgfx::LGFX_Sprite& bar)
{
    const int w = (int)bar.width();
    const int h = (int)bar.height();
    if (!bar.getBuffer() || w <= 0 || h <= 0) return;

    const uint8_t l = s_link;
    const uint16_t col = link_color(l);

    /* 桌面: 信息区升级成实时链路面板(两行 10px)。
     * 数据全部直读 zdlink 的真实收发打点, 不写死、不估算。 */
    if (strcmp(zd_fg_app(), "Launcher") == 0 && h >= 24) {
        ZdLinkInfo in;
        memset(&in, 0, sizeof(in));
        zdlink_link_info(&in);
        const uint32_t now = zdlink_now_ms();

        bar.fillScreen(C_BG);
        bar.setTextWrap(false);
        bar.setFont(&fonts::efontCN_10);
        bar.setTextDatum(top_left);

        /* 第一行: 链路来源(青/橙) + RSSI + SNR; 右侧电量 */
        bar.fillRect(2, 2, 12, 10, col);
        bar.setTextDatum(middle_center);
        bar.setTextColor(TFT_BLACK, col);
        bar.drawString(l == ZD_VIA_E ? "E" : (l == ZD_VIA_L ? "L" : "-"), 8, 7);
        bar.setTextDatum(top_left);

        char b1[24];
        bar.setTextColor(col);
        bar.drawString(link_name(l), 17, 1);

        if (l && in.rssi) snprintf(b1, sizeof(b1), "%ddBm", (int)in.rssi);
        else snprintf(b1, sizeof(b1), "--dBm");
        bar.setTextColor(C_DIM);
        bar.drawString(b1, 64, 1);

        if (l == ZD_VIA_L && in.snr4 != 127) {
            int v = in.snr4;
            int a = v < 0 ? -v : v;
            snprintf(b1, sizeof(b1), "SNR%s%d.%d", v < 0 ? "-" : "", a / 4, (a % 4) * 10 / 4);
        } else {
            snprintf(b1, sizeof(b1), "SNR --");
        }
        bar.drawString(b1, 108, 1);

        char buf[12];
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)s_bat);
        bar.setTextDatum(top_right);
        bar.setTextColor(s_bat < 20 ? C_LOW : C_FG);
        bar.drawString(buf, 208, 1);
        const int ix = 212, iy = 2;
        bar.drawRect(ix, iy, 18, 10, C_FG);
        bar.fillRect(ix + 18, iy + 3, 2, 4, C_FG);
        int fw = (int)(14 * s_bat / 100);
        if (fw > 0) bar.fillRect(ix + 2, iy + 2, fw, 6, s_bat < 20 ? C_LOW : C_BATT);
        bar.setTextDatum(top_left);

        /* 第二行: 最近收/发、ACK 延迟、在线节点/桥、刷新时间。
         * 10px 中文在实机上笔画糊在一起(收发延节点都认不出), 这里改 12px 并
         * 按 textWidth 顺序排, 不再写死 x 坐标, 避免字数变化时互相压字。 */
        bar.setFont(&fonts::efontCN_12);
        char age[8], line[24];
        auto put_age = [&](uint32_t t) {
            if (!t) { snprintf(age, sizeof(age), "--"); return; }
            uint32_t d = now - t;
            if (d < 60000) snprintf(age, sizeof(age), "%us", (unsigned)(d / 1000));
            else snprintf(age, sizeof(age), "%um", (unsigned)(d / 60000));
        };
        int x = 2;
        auto put = [&](const char* s, uint16_t c) {
            bar.setTextColor(c);
            bar.drawString(s, x, 12);
            x += bar.textWidth(s) + 6;
        };
        put_age(in.last_rx_ms);
        snprintf(line, sizeof(line), "收%s", age);
        put(line, C_FG);
        put_age(in.last_tx_ms);
        snprintf(line, sizeof(line), "发%s", age);
        put(line, C_FG);
        if (in.ack_rtt_ms) snprintf(line, sizeof(line), "延%ums", (unsigned)in.ack_rtt_ms);
        else snprintf(line, sizeof(line), "延--");
        put(line, C_DIM);
        snprintf(line, sizeof(line), "节点%u", (unsigned)in.nodes);
        put(line, col);
        if (in.bridges) {
            snprintf(line, sizeof(line), "桥%u", (unsigned)in.bridges);
            put(line, col);
        }

        char hm[8];
        clock_hm(hm, sizeof(hm));
        if (strcmp(hm, "--:--") == 0) {
            /* 没对时(RTC 未设置/NTP 未同步): 退回开机分钟:秒, 不要显示假时间 */
            uint32_t up = GetHAL().millis() / 1000;
            snprintf(line, sizeof(line), "刷%u:%02u", (unsigned)(up / 60), (unsigned)(up % 60));
        } else {
            snprintf(line, sizeof(line), "刷%s", hm);
        }
        bar.setTextDatum(top_right);
        bar.setTextColor(C_DIM);
        bar.drawString(line, 238, 12);
        bar.setTextDatum(top_left);

        bar.drawFastHLine(0, h - 1, w, col);
        s_status_draws++;
        ESP_LOGI("SYSBAR", "panel link=%c rssi=%d snr4=%d rx=%lu tx=%lu rtt=%lu nodes=%u bridges=%u",
                 l == ZD_VIA_E ? 'E' : (l == ZD_VIA_L ? 'L' : '-'), (int)in.rssi, (int)in.snr4,
                 (unsigned long)in.last_rx_ms, (unsigned long)in.last_tx_ms,
                 (unsigned long)in.ack_rtt_ms, (unsigned)in.nodes, (unsigned)in.bridges);
        return;
    }

    bar.fillScreen(C_BG);
    bar.setTextWrap(false);
    bar.setFont(&fonts::efontCN_12);
    bar.setTextDatum(top_left);

    /* 左: 链路色块 + 名称, 一眼分清 ESP-NOW(青) / LoRa(橙) */
    bar.fillRect(2, 5, 18, 16, col);
    bar.setTextDatum(middle_center);
    bar.setTextColor(TFT_BLACK, col);
    bar.drawString(l == ZD_VIA_E ? "E" : (l == ZD_VIA_L ? "L" : "-"), 11, 13);
    bar.setTextDatum(top_left);
    bar.setTextColor(col);
    bar.drawString(link_name(l), 24, 7);

    /* 中: 信号格(按 dBm 分档)。dBm 数值不再显示(监视器那边看) */
    int bars = signal_bars(l);
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 3;
        bar.fillRect(88 + i * 5, 19 - bh, 4, bh, (i < bars) ? col : C_DIM);
    }

    /* 右: 时间 + 电量 + 电池图标 */
    char buf[12];
    char hm[8];
    clock_hm(hm, sizeof(hm));
    bar.setTextDatum(top_right);
    bar.setTextColor(C_FG);
    bar.drawString(hm, 168, 7);

    snprintf(buf, sizeof(buf), "%u%%", (unsigned)s_bat);
    bar.setTextColor(s_bat < 20 ? C_LOW : C_FG);
    bar.drawString(buf, 208, 7);

    const int ix = 212, iy = 7;
    bar.drawRect(ix, iy, 18, 12, C_FG);
    bar.fillRect(ix + 18, iy + 4, 2, 4, C_FG);
    int fw = (int)(14 * s_bat / 100);
    if (fw > 0) bar.fillRect(ix + 2, iy + 2, fw, 8, s_bat < 20 ? C_LOW : C_BATT);

    /* 底边也用链路色, 与通知条同一套视觉语言 */
    bar.drawFastHLine(0, h - 1, w, col);
    bar.setTextDatum(top_left);

    /* 每次真正落笔都留一行日志: 桌面上如果出现第二套栏, 这里会看到非 ui_sysbar
     * 的内容与 draw 计数对不上; 连续多帧只有本函数在画 = 只有一个绘制者。 */
    s_status_draws++;
    ESP_LOGI("SYSBAR", "draw owner=ui_sysbar fg=%s link=%c bat=%u",
             zd_fg_app(), l == ZD_VIA_E ? 'E' : (l == ZD_VIA_L ? 'L' : '-'), (unsigned)s_bat);
}

/* 全屏 app 的顶部内联条：画进系统栏画布的前 kInlineH 行, 只推这 kInlineH 行。
 * 游戏内容画布从 y=kInlineH 开始, 两者不重叠, 所以这不是覆盖, 是排版让位。
 * 通知活跃时这条里画一行通知, 时间/电量暂时让位(通知结束再画回来)。
 * force=true: app 刚建好全屏画布时抢在它推内容之前强推一次(此时前台状态可能
 * 还是 Launcher, inlineMode() 尚未成立, 所以不能走常规判定)。 */
static void draw_inline(bool force)
{
    if (!force && !inlineMode()) return;

    auto& bar = GetHAL().canvasSystemBar();
    const int w = (int)bar.width();
    if (!bar.getBuffer() || w <= 0) return;

    /* 通知出现/收起的瞬间也必须重画, 否则条里会残留上一种内容 */
    const bool notice = ui_notify::active();
    if (!force && !s_dirty && notice == s_inline_notice) return;
    s_inline_notice = notice;

    /* 换栏那一瞬间: 上一屏幕是 26px 桌面栏, 它的 14..25 行马上会被 app 内容盖住。
     * 先刷黑, 避免在内容推上来之前露出"下半截旧栏"。 */
    if (force) {
        int old_h = (int)bar.height();
        if (old_h > kInlineH) {
            GetHAL().display().fillRect(0, kInlineH, w, old_h - kInlineH, TFT_BLACK);
        }
    }

    if (notice) {
        ui_notify::draw_inline(bar);
        GetHAL().display().pushImage(0, 0, w, kInlineH, (const uint16_t*)bar.getBuffer());
        s_inline_draws++;
        s_dirty = false;
        return;
    }

    const uint8_t l = s_link;
    const uint16_t col = link_color(l);

    bar.fillRect(0, 0, w, kInlineH, C_BG);
    bar.setTextWrap(false);
    bar.setTextDatum(top_left);

    /* 链路色块 + 单字母 */
    bar.fillRect(2, 2, 12, 10, col);
    bar.setTextDatum(middle_center);
    bar.setFont(&fonts::efontCN_10);
    bar.setTextColor(TFT_BLACK, col);
    bar.drawString(l == ZD_VIA_E ? "E" : (l == ZD_VIA_L ? "L" : "-"), 8, 7);

    /* 链路名 */
    bar.setTextDatum(top_left);
    bar.setFont(&fonts::efontCN_12);
    bar.setTextColor(col);
    bar.drawString(link_name(l), 17, 1);

    /* 信号格（只留格数, 不显示 dBm 数值） */
    int bars = signal_bars(l);
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 3;
        bar.fillRect(70 + i * 5, 12 - bh, 4, bh, (i < bars) ? col : C_DIM);
    }

    /* 时间 */
    char buf[12];
    char hm[8];
    clock_hm(hm, sizeof(hm));
    bar.setTextDatum(top_right);
    bar.setTextColor(C_FG);
    bar.drawString(hm, 158, 1);

    /* 电量百分比 + 电池图标 */
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)s_bat);
    bar.setTextColor(s_bat < 20 ? C_LOW : C_FG);
    bar.drawString(buf, 210, 1);

    const int ix = 214, iy = 2;
    bar.drawRect(ix, iy, 18, 10, C_FG);
    bar.fillRect(ix + 18, iy + 3, 2, 4, C_FG);
    int fw = (int)(14 * s_bat / 100);
    if (fw > 0) bar.fillRect(ix + 2, iy + 2, fw, 6, s_bat < 20 ? C_LOW : C_BATT);

    bar.drawFastHLine(0, kInlineH - 1, w, col);
    bar.setTextDatum(top_left);

    /* 只推这 kInlineH 行: 用 pushImage 而不是 pushSprite(0,0),
     * 否则 26 行会把游戏自己的前 12px 盖掉 */
    GetHAL().display().pushImage(0, 0, w, kInlineH, (const uint16_t*)bar.getBuffer());
    s_inline_draws++;
    s_dirty = false;
}

void drawInline()
{
    draw_inline(false);
}

/* 系统栏叠加层: 通知条优先, 其次是 109 高 app 的状态栏。
 * 全屏 app 一律不在这里画(顶栏由 drawInline 画在内容区上方)。 */
static void overlay(lgfx::LGFX_Sprite& bar)
{
    /* 全屏 app: 顶栏/通知都由 drawInline 画在 kInlineH 条里, 这里 26px 一律不碰 */
    if (inlineMode()) return;
    if (ui_notify::active()) {
        ui_notify::draw(bar);
        s_dirty = true; /* 通知走了要把顶栏重画回来 */
        return;
    }
    /* 消息面板自己画了两行顶栏(节点数/目标链路), 信息更全, 不覆盖 */
    if (zdmsg_is_open()) {
        s_dirty = true;
        return;
    }
    if (!s_dirty) return;
    draw_status(bar);
    s_dirty = false;
}

void init()
{
    GetHAL().setSystemBarOverlay(overlay);
    s_last_refresh = 0;
    s_dirty = true;
}

/* 桌面(Launcher)换屏/重建画布/通知收起后调用: 只置脏,
 * 真正的绘制在同一次 pushCanvasSystemBar → overlay 里完成, 保证"画"和"推"是同一个人。 */
void invalidate()
{
    s_dirty = true;
}

/* 采样链路/RSSI/电量/时间。数据 2Hz 刷新就够, 避免每帧读 ADC;
 * refresh_now() 会无视 2Hz 节流强制采一次(自测用)。 */
static void refresh_data(uint32_t now)
{
    s_last_refresh = now;
    int8_t rssi = 0;
    uint8_t bat = GetHAL().getBatLevel();
    uint8_t link = zdlink_active_link(&rssi, kLinkWindowMs);
    if (s_force_link && (int32_t)(now - s_force_until) < 0) {
        link = s_force_link;
        if (rssi == 0) rssi = -42;
    } else {
        s_force_link = 0;
    }
    if (link != s_link || rssi != s_rssi || bat != s_bat) s_dirty = true;
    s_link = link;
    s_rssi = rssi;
    s_bat = bat;
    int cm = clock_min();
    if (cm != s_clock_min) {
        s_clock_min = cm;
        s_dirty = true;
    }
}

void update()
{
    uint32_t now = GetHAL().millis();

    if (s_last_refresh == 0 || now - s_last_refresh >= 500) refresh_data(now);

    /* 通知收起后, 立刻把顶栏画回来(全屏时是内联条, 普通 app 是 26px 系统栏) */
    bool notice = ui_notify::active();
    if (notice != s_notice_was) {
        s_dirty = true; /* 出现/收起都要重画这条内联头 */
        if (!inlineMode() && GetHAL().canvas().getBuffer()) {
            GetHAL().pushCanvasSystemBar();
            s_pushes++;
        }
        s_notice_was = notice;
    }

    /* 前后台切换(109 <-> 121)时顶栏画法不同, 必须重画 */
    bool inl = inlineMode();
    if (inl != s_inline) {
        s_inline = inl;
        s_dirty = true;
        if (!inl && GetHAL().canvas().getBuffer()) {
            GetHAL().pushCanvasSystemBar();
            s_pushes++;
        }
    }

    if (inl) {
        /* 全屏游戏: 只画/推顶部 kInlineH(状态栏或一行通知), 绝不推 26px 系统栏盖内容 */
        drawInline();
        return;
    }

    if (notice) return; /* 非全屏: 通知层自己每帧推 */

    /* 直接画屏的 app(LegacyApp 等)没有共享画布, 不叠任何东西 */
    if (!GetHAL().canvas().getBuffer()) return;

    /* app/游戏都可能 pushSprite 盖住顶栏, 所以前台有共享画布时由这里统一把顶栏顶回去;
     * 内容没变时只推屏不重画。桌面(Launcher)的那套自绘已经删掉, 同样走这里,
     * 保证全工程只有 ui_sysbar 一个顶栏绘制者。
     * LegacyApp 直接画屏(不用共享画布), 也不能叠。 */
    const char* fg = zd_fg_app();
    const bool desktop = (strcmp(fg, "Launcher") == 0);
    const bool direct_draw = (strcmp(fg, "LegacyApp") == 0) || (strcmp(fg, "BrainServer") == 0);
    if (desktop != s_was_desktop) {
        /* 换前后台时顶栏画布里的内容可能是面板画的, 必须重画 */
        s_was_desktop = desktop;
        s_dirty = true;
    }
    if (direct_draw) return;

    /* 桌面顶栏也由这里统一画: Launcher 自绘的那套已经删掉, 全工程只剩这一个绘制者。
     * 只在脏(链路/电量/分钟变化、换屏、通知收起)时推一次, 不做 8ms 空推。
     * 以前这里是 return, 桌面交给 Launcher 画, 两套轮流覆盖同一块 240x26 画布,
     * 才会出现"两套栏来回闪 / 换栏时短暂混绘"。 */
    if (desktop) {
        /* 实时面板里的"收/发 Xs 前"每秒都在变, 1Hz 置脏让顶栏跟上;
         * 其余数据(链路/电量/节点)本就在 refresh_data 里按变化置脏。 */
        static uint32_t s_panel_tick = 0;
        if (now - s_panel_tick >= 1000) {
            s_panel_tick = now;
            s_dirty = true;
        }
        if (s_dirty && now - s_last_push >= 100) {
            s_last_push = now;
            GetHAL().pushCanvasSystemBar();
            s_pushes++;
        }
        return;
    }
    /* 8ms(约 120Hz)上限: 比常见 60fps 画面更密, 保证每帧之间都能把顶栏顶回去,
     * 同时避免 app 空转时把 SPI 刷爆。 */
    if (now - s_last_push >= 8) {
        s_last_push = now;
        GetHAL().pushCanvasSystemBar();
        s_pushes++;
    }
}

void afterFrame()
{
    /* 全屏 app: 内容推在 y=kInlineH 之下, 顶栏/通知在 kInlineH 条里, 互不重叠,
     * 这里不需要再压任何东西。只有降级成 109 画布(非 inline)时才用老的 26px 叠加。 */
    if (inlineMode()) return;
    if (!ui_notify::active()) return;
    GetHAL().pushCanvasSystemBar();
    s_pushes++;
    s_dirty = true; /* 通知结束后重画 */
}

void force_link(uint8_t link, uint32_t ms)
{
    s_force_link = link;
    s_force_until = GetHAL().millis() + ms;
    s_last_refresh = 0; /* 下一帧立刻生效 */
}

/* 自测用: 不等 2Hz 节流, 立刻采一次并把顶栏画出来。
 * 用于"真实空口帧刚注入 → 马上看来源标记"这种时序敏感断言,
 * 否则等 500ms 时对端又来了 ESP-NOW 帧, 显示的会是 E(那是对的)。 */
void refresh_now()
{
    refresh_data(GetHAL().millis());
    if (ui_notify::active()) return;
    if (inlineMode()) {
        drawInline();
        return;
    }
    if (GetHAL().canvas().getBuffer()) {
        GetHAL().pushCanvasSystemBar();
        s_pushes++;
    }
}

uint8_t link() { return s_link; }
int8_t rssi() { return s_rssi; }
uint8_t battery() { return s_bat; }
uint32_t pushes() { return s_pushes; }
uint32_t inlineDraws() { return s_inline_draws; }
uint32_t statusDraws() { return s_status_draws; }

}  // namespace ui_sysbar

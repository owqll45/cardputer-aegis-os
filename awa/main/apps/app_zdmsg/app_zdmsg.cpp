#include "app_zdmsg.h"

#include <apps/utils/glink/glink.h>
#include <apps/utils/glink/zdlink.h>
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <M5GFX.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <string>
#include <hal/utils/sys_time.h>
#include <apps/utils/ui_notify/ui_notify.h>
#include <apps/utils/ztrace/ztrace.h>
#include "serial_pump.h"

static const char* TAG = "LoRaMsg";

/* 面板是否在前台: 全局通知的 '=' 跳转靠它让位给输入页。
 * 被别的 app 顶到后台时 mooncake 不一定调 onClose, 所以按"最后一帧 onRunning"判断。 */
static uint32_t s_last_run_ms = 0;
/* 最近一次发送的时刻, 用于 ACK 耗时取证 */
static uint32_t s_tx_ms = 0;

bool zdmsg_is_open()
{
    return s_last_run_ms && (zdlink_now_ms() - s_last_run_ms) < 250;
}

/* ------------------------------------------------------------------ */
/*  配色(与系统 240x109 画布一致: 绿=收, 蓝=发, 橙=告警)               */
/* ------------------------------------------------------------------ */
static const uint16_t C_BG     = 0x0841;
static const uint16_t C_BAR    = 0x18E3;
static const uint16_t C_LINE   = 0x31A6;
static const uint16_t C_FG     = 0xE7BE;
static const uint16_t C_DIM    = 0x7BEF;
static const uint16_t C_IN     = 0x07E0;
static const uint16_t C_OUT    = 0x05BF;
static const uint16_t C_WARN   = 0xFD20;
static const uint16_t C_ERR    = 0xF800;
static const uint16_t C_ACCENT = 0x07FF;
static const uint16_t C_SEL    = 0x01CF;

/* ------------------------------------------------------------------ */
/*  键盘队列: 回调只入队, 处理/绘制都在 app 主任务                     */
/* ------------------------------------------------------------------ */
static portMUX_TYPE s_kq_mux = portMUX_INITIALIZER_UNLOCKED;
static Keyboard::KeyEvent_t s_kq[16];
static volatile int s_kq_head = 0;
static volatile int s_kq_tail = 0;

static void kq_push(const Keyboard::KeyEvent_t& e)
{
    portENTER_CRITICAL(&s_kq_mux);
    int next = (s_kq_tail + 1) % 16;
    if (next == s_kq_head) s_kq_head = (s_kq_head + 1) % 16;  /* 满则丢最旧, 绝不阻塞 */
    s_kq[s_kq_tail] = e;
    s_kq_tail = next;
    portEXIT_CRITICAL(&s_kq_mux);
}

static bool kq_pop(Keyboard::KeyEvent_t& e)
{
    bool ok = false;
    portENTER_CRITICAL(&s_kq_mux);
    if (s_kq_head != s_kq_tail) {
        e = s_kq[s_kq_head];
        s_kq_head = (s_kq_head + 1) % 16;
        ok = true;
    }
    portEXIT_CRITICAL(&s_kq_mux);
    return ok;
}

static void kq_clear()
{
    portENTER_CRITICAL(&s_kq_mux);
    s_kq_head = 0;
    s_kq_tail = 0;
    portEXIT_CRITICAL(&s_kq_mux);
}

/* ------------------------------------------------------------------ */
/*  会话列表(24 条, 关掉 app 再打开仍保留)                             */
/* ------------------------------------------------------------------ */
#define MSG_MAX 24

struct Line {
    uint8_t out;    /* 0=收 1=发 2=系统 */
    uint8_t via;    /* ZD_VIA_* */
    uint8_t loop;   /* 1 = LoRa 回环(自己发出去又被空口收回来) */
    uint8_t relay;  /* 1 = 该帧被中继过(flags & ZF_FORWARDED) */
    int8_t rssi;
    uint32_t src;
    char text[97];
};

static Line s_lines[MSG_MAX];
static int s_line_head = 0;
static int s_line_cnt = 0;

static void add_line(uint8_t out, uint8_t via, uint32_t src, int8_t rssi, const char* text,
                     bool loop = false, bool relay = false)
{
    Line* m = &s_lines[s_line_head];
    snprintf(m->text, sizeof(m->text), "%s", text ? text : "");
    m->out = out;
    m->via = via;
    m->loop = loop ? 1 : 0;
    m->relay = relay ? 1 : 0;
    m->src = src;
    m->rssi = rssi;
    s_line_head = (s_line_head + 1) % MSG_MAX;
    if (s_line_cnt < MSG_MAX) s_line_cnt++;
}

/* back=0 最新 */
static const Line* line_at(int back)
{
    if (back < 0 || back >= s_line_cnt) return nullptr;
    int idx = (s_line_head - 1 - back + MSG_MAX * 2) % MSG_MAX;
    return &s_lines[idx];
}

static const char* via_tag(uint8_t via)
{
    switch (via) {
        case ZD_VIA_L: return "L";
        case ZD_VIA_R: return "R";
        default: return "E";
    }
}

/* 自测用: 列表里是否已经有这条(定位校验), 只在 app 任务读 */
bool zdmsg_list_contains(const char* needle)
{
    if (!needle || !*needle) return false;
    for (int i = 0; i < s_line_cnt; i++) {
        const Line* m = line_at(i);
        if (m && strstr(m->text, needle)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  设备类型: 无 GPS = 橙, 带 GPS = 红, 判断不出 = 灰(未知)            */
/*  ZD v1.1 帧里没有 GPS 字段(BEAT 只有 mV/pct/role, LTEXT 只有        */
/*  rssi/snr/hops), 只能按"是否 ZD 网内设备"判断: 本机 ADV 与 C3 中继  */
/*  板实测都没有 GNSS 模块(d3/设备硬件与固件开发准备报告.md §2.6       */
/*  "No GNSS Module"), 判为无 GPS; 只经 LoRa 空口出现的节点无法判断,   */
/*  明确显示"未知", 不靠猜标红。                                        */
/* ------------------------------------------------------------------ */
enum DevKind : uint8_t { DEV_UNKNOWN = 0, DEV_NOGPS, DEV_GPS };

static DevKind dev_kind_of(uint32_t id)
{
    if (id == 0) return DEV_UNKNOWN;
    if (id == zdlink_my_id()) return DEV_NOGPS;
    ZdNodeInfo nd[8];
    int n = zdlink_get_nodes(nd, 8);
    for (int i = 0; i < n; i++) {
        if (nd[i].id == id) return nd[i].espnow ? DEV_NOGPS : DEV_UNKNOWN;
    }
    return DEV_UNKNOWN;
}

static uint16_t dev_color(DevKind k)
{
    if (k == DEV_GPS) return C_ERR;     /* 红: 带 GPS */
    if (k == DEV_NOGPS) return C_WARN;  /* 橙: 无 GPS */
    return C_DIM;                       /* 灰: 未知 */
}

static const char* dev_tag(DevKind k)
{
    if (k == DEV_GPS) return "GPS";
    if (k == DEV_NOGPS) return "无GPS";
    return "GPS?";
}

/* 与 C3 的 peer_label 保持一致, 便于在两端看到同一个名字 */
static void node_label(char* out, size_t n, uint32_t id)
{
    if (id == 0) { snprintf(out, n, "---"); return; }
    if ((id & 0xFFFF) == 0x3F68) snprintf(out, n, "3F68");
    else if ((id & 0xFFFFFF) == 0xAF0000) snprintf(out, n, "CPT");
    else snprintf(out, n, "%04lX", (unsigned long)(id & 0xFFFF));
}

/* LNACK reason → 界面文案(协议 §5.3) */
static const char* reason_text(uint8_t r)
{
    switch (r) {
        case ZD_LNACK_TX_DISABLED: return "未转发: 设备发射已禁用(无天线)";
        case ZD_LNACK_QUEUE_FULL:  return "未转发: 忙, 请稍后重发";
        case ZD_LNACK_AIR_TIMEOUT: return "未转发: 空口超时";
        case ZD_LNACK_UNSUPPORTED: return "未转发: 不支持的请求";
        default: return "未转发: 未知原因";
    }
}

/* 选桥: 统一走链路层的"就近 + 新鲜"打分(见 zdlink_pick_bridge) */
static uint32_t pick_bridge(uint32_t prefer, uint8_t* caps_out)
{
    return zdlink_pick_bridge(prefer, caps_out);
}

/* 在多座桥之间切换(只列具备桥能力的, 协议 §6: caps.bit0) */
static uint32_t cycle_bridge(uint32_t cur, int dir)
{
    ZdBridge br[4];
    int n = zdlink_get_bridges(br, 4);
    uint32_t ids[4];
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (br[i].caps & 0x01) ids[cnt++] = br[i].id;
    }
    if (cnt == 0) return 0;
    int idx = -1;
    for (int i = 0; i < cnt; i++) {
        if (ids[i] == cur) idx = i;
    }
    if (idx < 0) idx = 0;
    else idx = (idx + (dir > 0 ? 1 : cnt - 1)) % cnt;
    return ids[idx];
}

/* ADV 的键盘是 4x14 矩阵, 没有独立方向键(见 hal/keyboard/keyboard.cpp 的 keymap):
 * 方向码只在 fn(ctrl) + ; , . / 时由 HAL 合成。其它 app(贪吃蛇/扫雷等)都是直接
 * 把 ; , . / 当方向键用。面板的非输入页也照此别名, 否则裸按这四个键毫无反应;
 * 写消息页必须保留原字符, 所以只在非输入页做转换。 */
static KeScanCode_t nav_alias(KeScanCode_t c)
{
    switch (c) {
        case KEY_SEMICOLON: return KEY_UP;
        case KEY_COMMA:     return KEY_LEFT;
        case KEY_DOT:       return KEY_DOWN;
        case KEY_SLASH:     return KEY_RIGHT;
        default:            return c;
    }
}

/* 只有未按 Shift 的 ; , . / 才算"方向键"; ":" "<" ">" "?" 是可输入字符 */
static bool is_plain_nav_key(const Keyboard::KeyEvent_t& e)
{
    const char* nm = e.keyName;
    if (!nm || !nm[0] || nm[1]) return false;
    return nm[0] == ';' || nm[0] == ',' || nm[0] == '.' || nm[0] == '/';
}

/* UTF-8 安全裁剪: 只在字符边界截断, 超宽时以 ".." 收尾 */
static void draw_clip(lgfx::LGFX_Sprite& cv, const char* s, int x, int y, int maxw)
{
    if (!s || !*s) return;
    char buf[104];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = 0;

    if ((int)cv.textWidth(buf) <= maxw) {
        cv.drawString(buf, x, y);
        return;
    }
    /* 二分找最长可显示前缀(给 ".." 留 12px), 再退到 UTF-8 字符边界 */
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

static const char* fmt_ago(char* out, size_t n, uint32_t ms)
{
    uint32_t sec = ms / 1000;
    if (sec < 2) snprintf(out, n, "刚刚");
    else if (sec < 60) snprintf(out, n, "%lus前", (unsigned long)sec);
    else if (sec < 3600) snprintf(out, n, "%lum前", (unsigned long)(sec / 60));
    else snprintf(out, n, "%luh前", (unsigned long)(sec / 3600));
    return out;
}

/* 字号取证页: 同一串内容用 10/12/14px 各画一行, 只由 app 任务绘制 */
static volatile bool s_font_demo = false;

void zdmsg_font_demo(bool on)
{
    s_font_demo = on;
}

static void draw_font_demo(lgfx::LGFX_Sprite& cv)
{
    struct Item {
        const lgfx::IFont* f;
        const char* tag;
    };
    static const Item items[3] = {
        {&fonts::efontCN_10, "10"},
        {&fonts::efontCN_12, "12"},
        {&fonts::efontCN_14, "14"},
    };
    cv.fillScreen(C_BG);
    cv.setTextDatum(top_left);
    cv.setTextWrap(false);
    int y = 2;
    for (int i = 0; i < 3; i++) {
        cv.setFont(items[i].f);
        cv.setTextColor(C_FG);
        cv.drawString(items[i].tag, 2, y);
        cv.drawString("无线节点3F68", 26, y);
        cv.drawString("AVgc12", 126, y);
        cv.drawString("-72dBm", 178, y);
        y += 30;
    }
    cv.setFont(&fonts::efontCN_12);
}

/* ------------------------------------------------------------------ */
AppZdMsg::AppZdMsg()
{
    setAppInfo().name = "天链";
}

AppZdMsg::~AppZdMsg() {}

void AppZdMsg::load_settings()
{
    auto& st = GetHAL().getSettings();
    _mute     = st.GetBool("mute", false);
    _vol      = st.GetInt("sound_volume", 128);
    _bright   = st.GetInt("brightness", 100);
    _relay_on = st.GetBool("zd_relay", true);
    if (_vol < 0) _vol = 0;
    if (_vol > 255) _vol = 255;
    if (_bright < 10) _bright = 10;
    if (_bright > 100) _bright = 100;
    zdlink_set_relay(_relay_on);
}

/* ------------------------------------------------------------------ */
/*  时间: 顶栏常驻 HH:MM; 手动设置按本地时区落地;                     */
/*  C3 的时间广播(UTC + 来源)字段还没冻结, 收到后在这里接即可。        */
/* ------------------------------------------------------------------ */
static bool sys_time_valid()
{
    return (int64_t)time(nullptr) > 1700000000LL;  /* 2023-11 之后才算已对时 */
}

void AppZdMsg::clock_hm(char* out, size_t n) const
{
    if (!sys_time_valid()) {
        snprintf(out, n, "--:--");
        return;
    }
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

const char* AppZdMsg::time_src_name() const
{
    switch (_time_src) {
        case TS_MANUAL: return "手动";
        case TS_LORA: return "LoRa";
        case TS_GPS: return "GPS";
        case TS_NET: return "网络";
        default: return "未同步";
    }
}

void AppZdMsg::time_start_edit()
{
    struct tm tm;
    if (sys_time_valid()) {
        time_t t = time(nullptr);
        localtime_r(&t, &tm);
        _edit_h = tm.tm_hour;
        _edit_m = tm.tm_min;
    } else {
        _edit_h = 0;
        _edit_m = 0;
    }
    _time_edit = true;
    set_toast(";. 改小时  ,/ 改分钟  Enter 保存");
}

void AppZdMsg::time_apply_manual()
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (sys_time_valid()) {
        time_t t = time(nullptr);
        localtime_r(&t, &tm);  /* 有日期就保留日期, 只改时分 */
    } else {
        tm.tm_year = 2026 - 1900;
        tm.tm_mday = 1;
    }
    tm.tm_hour = _edit_h;
    tm.tm_min = _edit_m;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;

    struct timeval tv;
    tv.tv_sec = mktime(&tm);
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    _time_src = TS_MANUAL;
    auto& st = GetHAL().getSettings();
    st.SetInt("zd_time_src", TS_MANUAL);
    st.SetInt("zd_time_h", _edit_h);
    st.SetInt("zd_time_m", _edit_m);
    st.Sync();

    char b[40];
    snprintf(b, sizeof(b), "时间已设为 %02d:%02d (手动)", _edit_h, _edit_m);
    set_toast(b);
    ESP_LOGI(TAG, "time set manually %02d:%02d tz=%d src=manual", _edit_h, _edit_m, _tz);

    /* 协议 v1.2 §1.2: 手动时间直接以 src=2 广播出去, C3 采纳后反向扩散到 LoRa 网 */
    if (zdlink_ready()) zdlink_send_time((uint32_t)tv.tv_sec, 2, 5);
}

/* 协议 v1.2 §1.2 采纳规则(plen>=5 才到这里, utc=0/src=0 已在解析层丢弃) */
bool AppZdMsg::time_adopt(uint32_t utc, uint8_t src, uint8_t acc)
{
    if (!utc || !src) return false;
    const uint8_t want = (src == 1) ? TS_GPS : TS_MANUAL;  /* 协议: 1=GPS 2=手动 */

    /* 信任等级: 未同步 0 < 手动/LoRa/网络 2 < GPS 3; GPS 之间允许互相微调 */
    const int cur_rank = (_time_src == TS_GPS) ? 3 : (_time_src == TS_NONE ? 0 : 2);
    const int new_rank = (want == TS_GPS) ? 3 : 2;
    const bool gps_refine = (_time_src == TS_GPS && want == TS_GPS);

    if (!gps_refine) {
        if (new_rank < cur_rank) {
            ESP_LOGI(TAG, "time ignored: local %s vs src=%u", time_src_name(), (unsigned)src);
            return false;
        }
        if (new_rank == cur_rank) {
            /* 同级不覆盖, 免得对端每 30s 的广播把已对好的时钟反复改写 */
            ESP_LOGI(TAG, "time ignored: same trust %s vs src=%u", time_src_name(), (unsigned)src);
            return false;
        }
    }

    struct timeval tv;
    tv.tv_sec = (time_t)utc;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    _time_src = want;
    auto& st = GetHAL().getSettings();
    st.SetInt("zd_time_src", _time_src);
    st.Sync();

    char hm[8];
    clock_hm(hm, sizeof(hm));
    char b[40];
    snprintf(b, sizeof(b), "已由%s校准 %s", time_src_name(), hm);
    set_toast(b);
    ESP_LOGI(TAG, "time adopted utc=%lu src=%u acc=%u -> %s (%s)", (unsigned long)utc,
             (unsigned)src, (unsigned)acc, hm, time_src_name());
    return true;
}

void AppZdMsg::goto_page(Page p)
{
    _page = p;
    _list_scroll = 0;
    _time_edit = false;
    if (p == PG_LIST) {
        _unread = 0;
        ui_notify::clear_unread();  /* 顶栏未读与全局通知共用一份计数 */
    }
}

void AppZdMsg::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "open");
    _input.clear();
    _last_msgid = 0;
    _last_via = ZD_VIA_E;
    _bridge_id = 0;
    _lnack_txt[0] = 0;
    _lnack_ms = 0;
    _cursor_ms = zdlink_now_ms();
    _cursor_on = true;
    _last_draw_ms = 0;
    _page = PG_LIST;
    _list_scroll = 0;
    _node_scroll = 0;
    _set_sel = 0;
    _target = 0;
    _toast_ms = 0;
    _unread = 0;
    _time_edit = false;
    load_settings();
    ui_notify::clear_unread();
    ZT("panel", "state=open fg=%s page=list", zd_fg_app());

    auto& st = GetHAL().getSettings();
    _last_peer = (uint32_t)st.GetInt("zd_last_peer", 0);
    _tz = st.GetInt("tz_offset", 8);

    /* 断电后 RTC 丢失: 有手动时间就恢复, 否则按系统(NTP)状态显示来源 */
    _time_src = (uint8_t)st.GetInt("zd_time_src", TS_NONE);
    if (sys_time_valid()) {
        if (_time_src == TS_NONE) _time_src = TS_NET;
    } else if (_time_src == TS_MANUAL) {
        _edit_h = st.GetInt("zd_time_h", 0);
        _edit_m = st.GetInt("zd_time_m", 0);
        time_apply_manual();
    } else {
        _time_src = TS_NONE;
    }

    /* 回调里只入队; 真正的字符串处理在 onRunning */
    _key_slot = GetHAL().keyboard.onKeyEvent.connect(
        [](const Keyboard::KeyEvent_t& e) { kq_push(e); });
    kq_clear();

    /* 直接用系统 240x109 画布, 不改尺寸/色深, 面板进出不产生堆碎片 */
    glink_acquire("天链", GLINK_GID_LAUNCHER, GLINK_ICON_GENERIC);
}

void AppZdMsg::onRunning()
{
    s_last_run_ms = zdlink_now_ms();  /* 前台心跳: 全局通知据此判断是否让位 */
    if (GetHAL().goButton().wasClicked()) {
        if (_time_edit) {
            _time_edit = false;  /* 时间编辑中: G0 只取消编辑 */
            set_toast("已取消");
        } else if (_page == PG_LIST) {
            close();
            return;
        } else {
            goto_page(PG_LIST);  /* G0 一律返回首页; 首页再按才退出 */
            kq_clear();          /* 返回时丢掉还排在队列里的输入 */
        }
    }

    handle_keys();

    /* 自测：无键盘注入一条文本，走与真实发送完全相同的路径（含 1.5s 降级判定） */
    if (SerialPump::takePending("ZDAUTO")) {
        _input = "selftest auto";
        ESP_LOGI(TAG, "selftest ZDAUTO sending '%s'", _input.c_str());
        send_input();
    }

    drain_events();

    /* 与 Launcher 里的 glink_poll 互为兜底, 心跳/ACK 补发都是幂等的 */
    zdlink_poll();

    /* 自动优先的降级点：ESP-NOW 发出 1.5s 仍没 ACK 且桥可用，就同一文本走 LoRa。
     * 接收端（AA:D0/两台小屏）按 (from,text) 3s 去重，所以不会重复上屏。 */
    if (_auto_pending && zdlink_now_ms() >= _auto_deadline) {
        _auto_pending = false;
        uint8_t st = zdlink_tx_state(_auto_msgid);
        uint8_t caps = 0;
        uint32_t bid = pick_bridge(_bridge_id, &caps);
        uint16_t mid = 0;
        if (st == 1) {
            ZT("link", "pref=espnow|msgid=%u|ack=1|fallback=0|cost=1500ms", (unsigned)_auto_msgid);
        } else if (bid && (caps & 0x03) == 0x03 &&
                   zdlink_send_lora_text(_auto_text, (int)strlen(_auto_text), bid, &mid, nullptr)) {
            _last_via = ZD_VIA_R;
            _last_msgid = mid;
            _bridge_id = bid;
            ZT("link", "pref=espnow|msgid=%u|ack=0|fallback=lora|msgid2=%u|bridge=%08lx|cost=1500ms",
               (unsigned)_auto_msgid, (unsigned)mid, (unsigned long)bid);
            set_toast("ESP-NOW 无 ACK, 已转 LoRa");
        } else {
            ZT("link", "pref=espnow|msgid=%u|ack=0|fallback=none|why=no_bridge|cost=1500ms",
               (unsigned)_auto_msgid);
        }
    }

    uint32_t now = zdlink_now_ms();
    if (now - _last_draw_ms < 33) return;  /* 约 30fps 上限 */
    _last_draw_ms = now;

    if (now - _cursor_ms > 500) {
        _cursor_ms = now;
        _cursor_on = !_cursor_on;
    }

    /* 每 10 秒打一次链路统计, 方便串口核对收发是否真的在跑 */
    static uint32_t s_log_t = 0;
    static uint32_t s_last_rx = 0, s_last_ack = 0;
    if (now - s_log_t > 10000) {
        s_log_t = now;
        uint32_t rx = 0, bad = 0, tx = 0, ack = 0;
        zdlink_stats(&rx, &bad, &tx, &ack);
        if (rx != s_last_rx || ack != s_last_ack) {
            ESP_LOGI(TAG, "stats rx=%lu(+%lu) bad=%lu tx=%lu ack=%lu(+%lu)", (unsigned long)rx,
                     (unsigned long)(rx - s_last_rx), (unsigned long)bad, (unsigned long)tx,
                     (unsigned long)ack, (unsigned long)(ack - s_last_ack));
            s_last_rx = rx;
            s_last_ack = ack;
        }
    }

    draw();
    GetHAL().pushCanvasSystemBar();  /* 顶栏画进系统栏画布: 本 app 占满 240x135 */
    GetHAL().pushCanvas();
}

void AppZdMsg::onClose()
{
    mclog::tagInfo(getAppInfo().name, "close");
    s_last_run_ms = 0;
    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    kq_clear();
    /* 回到桌面: 让 GMK 镜像端切回 Launcher 画面 */
    glink_acquire("Launcher", GLINK_GID_LAUNCHER, GLINK_ICON_GENERIC);
}

void AppZdMsg::set_toast(const char* s)
{
    snprintf(_toast, sizeof(_toast), "%s", s ? s : "");
    _toast_ms = zdlink_now_ms();
}

/* 记住上一次有往来的对端(来信的 from 或 ACK 的 from), 存 NVS, 界面高亮 */
void AppZdMsg::note_peer(uint32_t id)
{
    if (!id || id == zdlink_my_id() || id == _last_peer) return;
    _last_peer = id;
    auto& st = GetHAL().getSettings();
    st.SetInt("zd_last_peer", (int32_t)id);
    st.Sync();
    ESP_LOGI(TAG, "last peer -> %08lX", (unsigned long)id);
}

/* ------------------------------------------------------------------ */
/*  按键(只在主任务里处理队列)                                         */
/* ------------------------------------------------------------------ */
void AppZdMsg::handle_keys()
{
    Keyboard::KeyEvent_t e;
    while (kq_pop(e)) {
        if (!e.state || e.isModifier) continue;
        handle_key(e);
    }
}

void AppZdMsg::handle_key(const Keyboard::KeyEvent_t& e)
{
    const bool enter = (e.keyCode == KEY_ENTER || e.keyCode == KEY_KPENTER);

    /* 键盘优势: 数字 1..5 直达页面, 不用层层翻菜单
     * (写消息页里数字是正文, 时间编辑中不跳页, 免得正在改的时间被静默丢弃) */
    if (_page != PG_WRITE && !_time_edit && e.keyCode >= KEY_1 && e.keyCode <= KEY_5) {
        Page p = (Page)((int)PG_LIST + (int)(e.keyCode - KEY_1));
        if (p != _page) {
            goto_page(p);
            ESP_LOGI(TAG, "page -> %d", (int)_page);
        }
        return;
    }

    /* '=' / '+' : 回到消息列表(与全局通知的跳转同义)。
     * 写消息页里不拦, 让它是普通字符——输入优先。 */
    if (_page != PG_WRITE && e.keyCode == KEY_EQUAL) {
        if (_page != PG_LIST) {
            goto_page(PG_LIST);
        } else {
            _list_scroll = 0;
            _unread = 0;
            ui_notify::clear_unread();
        }
        return;
    }

    /* 非输入页把 ; , . / 当作方向键(硬件没有独立方向键); 写消息页保留原字符。
     * 只看未按 Shift 的裸符号: ":" "<" ">" "?" 仍是可输入的字符。 */
    const bool plain_nav = is_plain_nav_key(e);
    const KeScanCode_t k = (_page == PG_WRITE || !plain_nav) ? e.keyCode : nav_alias(e.keyCode);
    if (k != e.keyCode) {
        ESP_LOGI(TAG, "nav 0x%02X -> 0x%02X (page %d)", (unsigned)e.keyCode, (unsigned)k,
                 (int)_page);
    }

    /* 非输入页、非设置页: 左右直接切页(硬件没有独立方向键, , / 与 ctrl+,/. 都认)。
     * 设置页要保留 ,/ 调数值, 写消息页要保留字符, 这两页不参与全局切页。 */
    if (_page != PG_WRITE && _page != PG_SET) {
        const KeScanCode_t raw = e.keyCode;
        const bool left_right = (raw == KEY_LEFT || raw == KEY_RIGHT) ||  /* ctrl 合成的方向码 */
                                (plain_nav && (raw == KEY_COMMA || raw == KEY_SLASH));
        if (left_right) {
            bool right = (raw == KEY_SLASH || raw == KEY_RIGHT);
            int p = (int)_page + (right ? 1 : -1);
            if (p < 0) p = (int)PG_ABOUT;
            if (p > (int)PG_ABOUT) p = 0;
            goto_page((Page)p);
            return;
        }
    }

    switch (_page) {
        case PG_LIST: {
            if (k == KEY_UP) {
                int max_scroll = s_line_cnt > 7 ? s_line_cnt - 7 : 0;
                if (_list_scroll < max_scroll) _list_scroll++;
            } else if (k == KEY_DOWN) {
                if (_list_scroll > 0) _list_scroll--;
            } else if (e.keyCode == KEY_TAB) {
                toggle_target();
            } else if (enter) {
                goto_page(PG_WRITE);
            } else {
                /* 键盘优势: 在列表上直接敲字符就进入写消息, 不用先翻到写消息页 */
                const char* nm = e.keyName;
                if (nm && nm[0] && !nm[1] && nm[0] >= 0x21 && nm[0] <= 0x7E) {
                    _input.clear();
                    _input.push_back(nm[0]);
                    goto_page(PG_WRITE);
                    ESP_LOGI(TAG, "list type -> write '%c'", nm[0]);
                }
            }
            break;
        }

        case PG_WRITE: {
            if (enter) {
                send_input();
            } else if (e.keyCode == KEY_TAB) {
                toggle_target();
            } else if (e.keyCode == KEY_LEFT || e.keyCode == KEY_RIGHT) {
                /* 多座桥时手动选择(协议 §4.1) */
                if (_target) {
                    uint32_t id = cycle_bridge(_bridge_id, e.keyCode == KEY_RIGHT ? 1 : -1);
                    if (id) {
                        _bridge_id = id;
                        uint8_t caps = 0;
                        pick_bridge(id, &caps);
                        char lb[16], t[40];
                        node_label(lb, sizeof(lb), id);
                        snprintf(t, sizeof(t), "桥 %s%s", lb, (caps & 0x02) ? " 可发" : " 禁发");
                        set_toast(t);
                    } else {
                        set_toast("没有可用的 C3 桥");
                    }
                }
            } else if (e.keyCode == KEY_BACKSPACE) {
                if (!_input.empty()) _input.pop_back();
            } else {
                /* keyName 已按 shift/capslock 处理过(例如 "A" / "!"), 取单字符 */
                const char* nm = e.keyName;
                if (nm && nm[0] && !nm[1]) {
                    char c = nm[0];
                    if (c >= 0x20 && c <= 0x7E && _input.size() < 200) _input.push_back(c);
                }
            }
            break;
        }

        case PG_NODES: {
            if (k == KEY_UP) {
                _node_scroll++;
            } else if (k == KEY_DOWN) {
                if (_node_scroll > 0) _node_scroll--;
            }
            break;
        }

        case PG_SET: {
            auto& st = GetHAL().getSettings();
            const int NSET = 6;
            if (_time_edit) {
                /* 时间编辑: 上下改小时, 左右改分钟, Enter 保存, G0/ESC 取消 */
                if (k == KEY_UP) {
                    _edit_h = (_edit_h + 1) % 24;
                } else if (k == KEY_DOWN) {
                    _edit_h = (_edit_h + 23) % 24;
                } else if (k == KEY_RIGHT) {
                    _edit_m = (_edit_m + 1) % 60;
                } else if (k == KEY_LEFT) {
                    _edit_m = (_edit_m + 59) % 60;
                } else if (enter) {
                    time_apply_manual();
                    _time_edit = false;
                } else if (e.keyCode == KEY_ESC) {
                    _time_edit = false;
                    set_toast("已取消");
                }
                break;
            }

            if (k == KEY_UP) {
                _set_sel = (_set_sel + NSET - 1) % NSET;
            } else if (k == KEY_DOWN) {
                _set_sel = (_set_sel + 1) % NSET;
            } else if (k == KEY_LEFT || k == KEY_RIGHT) {
                int d = (k == KEY_RIGHT) ? 1 : -1;
                if (_set_sel == 0) {  /* 静音 */
                    _mute = !_mute;
                    st.SetBool("mute", _mute);
                    GetHAL().setGlobalMute(_mute);
                    st.Sync();
                } else if (_set_sel == 1) {  /* 音量 */
                    _vol += d * 13;
                    if (_vol < 0) _vol = 0;
                    if (_vol > 255) _vol = 255;
                    if (_mute) {
                        _mute = false;
                        st.SetBool("mute", false);
                        GetHAL().setGlobalMute(false);
                    }
                    st.SetInt("sound_volume", _vol);
                    GetHAL().speaker.setVolume(_vol);
                    st.Sync();
                } else if (_set_sel == 2) {  /* 亮度 */
                    _bright += d * 5;
                    if (_bright < 10) _bright = 10;
                    if (_bright > 100) _bright = 100;
                    st.SetInt("brightness", _bright);
                    M5.Display.setBrightness((uint8_t)(_bright * 255 / 100));
                    st.Sync();
                } else if (_set_sel == 3) {  /* 参与中继 */
                    _relay_on = !_relay_on;
                    st.SetBool("zd_relay", _relay_on);
                    zdlink_set_relay(_relay_on);
                    st.Sync();
                } else if (_set_sel == 4) {  /* 时间: 左右先微调分钟 */
                    time_start_edit();
                } else if (_set_sel == 5) {  /* 时区 */
                    _tz += d;
                    if (_tz < -12) _tz = 14;
                    if (_tz > 14) _tz = -12;
                    st.SetInt("tz_offset", _tz);
                    applyTimeZone(_tz);
                    st.Sync();
                    char b[24];
                    snprintf(b, sizeof(b), "时区 UTC%+d", _tz);
                    set_toast(b);
                }
            } else if (enter) {
                if (_set_sel == 0) {
                    _mute = !_mute;
                    st.SetBool("mute", _mute);
                    GetHAL().setGlobalMute(_mute);
                    st.Sync();
                } else if (_set_sel == 3) {
                    _relay_on = !_relay_on;
                    st.SetBool("zd_relay", _relay_on);
                    zdlink_set_relay(_relay_on);
                    st.Sync();
                } else if (_set_sel == 4) {
                    time_start_edit();
                }
            }
            break;
        }

        default:
            break;
    }
}

void AppZdMsg::toggle_target()
{
    _target = _target ? 0 : 1;
    if (!_target) {
        set_toast("目标: 自动(ESP-NOW 优先)");
        return;
    }
    uint8_t caps = 0;
    uint32_t bid = pick_bridge(_bridge_id, &caps);
    if (!bid) {
        set_toast("目标: LoRa, 但未发现 C3 桥");
        return;
    }
    char lb[16], t[56];
    node_label(lb, sizeof(lb), bid);
    snprintf(t, sizeof(t), "目标: LoRa(桥 %s%s)", lb, (caps & 0x02) ? "" : " 禁发");
    set_toast(t);
}

void AppZdMsg::send_input()
{
    if (_input.empty()) {
        set_toast("先输入内容");
        return;
    }

    /* 链路优先级：ESP-NOW 可达就走 ESP-NOW；只有 ESP-NOW 不通（无近距链路/1.5s
     * 无 ACK）才降级 LoRa。_target=1 是手动强制 LoRa，保留给远距场景。
     * 自动模式下若桥不具备发射能力(如无天线的 AA:D0), 不再直接放弃,
     * 而是回落到 ESP-NOW —— 以前这里 return, 表现就是"能收不能发"。 */
    int8_t lrssi = 0;
    uint8_t live = zdlink_active_link(&lrssi, 3000);
    bool espnow_near = (_target == 0) && (live == ZD_VIA_E || live == 0);

    if (_target == 1 || !espnow_near) {
        /* 协议 §4: ZT_TEXT + ZF_LORA, dst 单播到桥 NodeNum(不能广播) */
        uint8_t caps = 0;
        uint32_t bid = pick_bridge(_bridge_id, &caps);
        const bool can_relay = bid && (caps & 0x03) == 0x03;

        if (can_relay) {
            uint16_t msgid = 0;
            if (!zdlink_send_lora_text(_input.c_str(), (int)_input.size(), bid, &msgid, nullptr)) {
                set_toast("发送失败: 无线未就绪");
                return;
            }
            _bridge_id = bid;
            _last_msgid = msgid;
            _last_via = ZD_VIA_R;
            _lnack_txt[0] = 0;
            add_line(1, ZD_VIA_R, zdlink_my_id(), 0, _input.c_str());
            _list_scroll = 0;
            ZT("tx", "msgid=%u via=R bridge=%08lX caps=%u len=%d",
               (unsigned)msgid, (unsigned long)bid, (unsigned)caps, (int)_input.size());
            _input.clear();
            set_toast("已交桥转发, 等空口确认(≥15s)");
            return;
        }

        if (_target == 1) {
            if (!bid) {
                ESP_LOGW(TAG, "lora send: no bridge yet");
                add_line(2, ZD_VIA_R, 0, 0, "未发现 C3 桥: 等心跳或检查信道");
                set_toast("没有可用的 C3 桥");
            } else if (!(caps & 0x01)) {
                add_line(2, ZD_VIA_R, bid, 0, "该节点不具备 LoRa 桥能力");
                set_toast("未发送: 桥无 LoRa 能力");
            } else {
                /* 协议 §6: caps.bit1=0 表示当前禁止发射(如 AA:D0 无天线) */
                add_line(2, ZD_VIA_R, bid, 0, "桥发射已禁用(无天线), 未发送");
                set_toast("未发送: 桥发射已禁用");
            }
            return;
        }

        /* 自动模式: 没有可用桥 → 落到下面走 ESP-NOW 直连 */
        add_line(2, ZD_VIA_R, bid, 0, "无可用发射桥, 改走 ESP-NOW");
    }

    uint16_t msgid = 0, seq = 0;
    if (!zdlink_send_text(_input.c_str(), (int)_input.size(), &msgid, &seq)) {
        set_toast("发送失败: 无线未就绪");
        ZT("tx", "msgid=0 via=E result=FAIL why=radio_not_ready");
        return;
    }
    s_tx_ms = zdlink_now_ms();
    ZT("tx", "msgid=%u seq=%u via=E fg=LoRaMsg len=%d text=\"%s\"", (unsigned)msgid,
       (unsigned)seq, (int)_input.size(), _input.c_str());

    _last_via = ZD_VIA_E;
    add_line(1, ZD_VIA_E, zdlink_my_id(), 0, _input.c_str());
    _last_msgid = msgid;
    /* 自动模式：记下这条，1.5s 没 ACK 就同一文本走 LoRa 补发（接收端按文本去重） */
    if (_target == 0) {
        snprintf(_auto_text, sizeof(_auto_text), "%s", _input.c_str());
        _auto_msgid = msgid;
        _auto_deadline = zdlink_now_ms() + 1500;
        _auto_pending = true;
    }
    _list_scroll = 0;
    _input.clear();
    set_toast("已发出, 等 ACK");
}

void AppZdMsg::drain_events()
{
    ZdEvent ev;
    while (zdlink_pop_event(ev)) {
        if (ev.kind == ZD_EV_TEXT) {
            add_line(0, ev.via, ev.src, ev.rssi, ev.text, false, (ev.flags & ZF_FORWARDED) != 0);
            note_peer(ev.src);
            char lb[16];
            node_label(lb, sizeof(lb), ev.src);
            ESP_LOGI(TAG, "chat += [%s %d %s] %s", lb, (int)ev.rssi, via_tag(ev.via), ev.text);
            if (_page != PG_LIST) {
                if (_unread < 999) _unread++;
                char t[40];
                snprintf(t, sizeof(t), "新消息 %s", lb);
                set_toast(t);
            }
        } else if (ev.kind == ZD_EV_LTEXT) {
            /* 协议 §3: 真实来源在 payload[0..3], 标 L 落列表; 本机回环另加标记 */
            add_line(0, ZD_VIA_L, ev.src, ev.rssi, ev.text, ev.loopback != 0);
            note_peer(ev.lora_from);
            char lb[16];
            node_label(lb, sizeof(lb), ev.lora_from);
            ESP_LOGI(TAG, "chat += [%s %d L snr4=%d h%u%s] %s", lb, (int)ev.rssi, (int)ev.snr4,
                     (unsigned)ev.hops, ev.loopback ? " loop" : "", ev.text);
            char t[40];
            if (_page != PG_LIST && _unread < 999) _unread++;
            if (ev.loopback) snprintf(t, sizeof(t), "本机回环 %s", lb);
            else snprintf(t, sizeof(t), "天链 %s", lb);
            set_toast(t);
        } else if (ev.kind == ZD_EV_TIME) {
            /* 协议 v1.2 §1.2: 时间帧不进聊天列表, 只按采纳规则校时 */
            time_adopt(ev.utc, ev.tsrc, ev.tacc);
        } else if (ev.kind == ZD_EV_LNACK) {
            /* 协议 §5.3: 明确失败, 显示在输入框下方, 不能当成功 */
            snprintf(_lnack_txt, sizeof(_lnack_txt), "%s", reason_text(ev.reason));
            _lnack_ms = zdlink_now_ms();
            ESP_LOGW(TAG, "lnack seq=%u msgid=%u reason=%u (%s)", (unsigned)ev.ack_seq,
                     (unsigned)ev.ack_msgid, (unsigned)ev.reason, _lnack_txt);
            ZT("nack", "msgid=%u seq=%u reason=%u text=\"%s\"", (unsigned)ev.ack_msgid,
               (unsigned)ev.ack_seq, (unsigned)ev.reason, _lnack_txt);
            set_toast(_lnack_txt);
        } else if (ev.kind == ZD_EV_ACK) {
            if (_last_msgid && ev.ack_msgid == _last_msgid) {
                ZT("ack", "msgid=%u seq=%u from=%04lX viasrc=%c dt=%ums", (unsigned)ev.ack_msgid,
                   (unsigned)ev.ack_seq, (unsigned long)(ev.src & 0xFFFF), via_tag(ev.via)[0],
                   (unsigned)(zdlink_now_ms() - s_tx_ms));
            }
            if (_last_msgid && ev.ack_msgid == _last_msgid && _last_via == ZD_VIA_R) {
                /* 只有单播到桥的 LoRa 消息 ACK 才是确定的"这一跳对端";
                 * ESP-NOW 广播会同时收到多台 ACK, 记哪台都不稳定, 不记 */
                note_peer(ev.src);
                ESP_LOGI(TAG, "lora tx acked seq=%u msgid=%u", (unsigned)ev.ack_seq,
                         (unsigned)ev.ack_msgid);
                set_toast("空口已发出(桥已 ACK)");
            }
        }
        /* BEAT/NODETAB 已更新 zdlink 内部节点表, 这里无需重复处理 */
    }
}

int AppZdMsg::node_online_count() const
{
    ZdNodeInfo nodes[8];
    int n = zdlink_get_nodes(nodes, 8);
    int on = 0;
    uint32_t now = zdlink_now_ms();
    for (int i = 0; i < n; i++) {
        if (now - nodes[i].seen_ms < 15000) on++;
    }
    return on;
}

/* ------------------------------------------------------------------ */
/*  绘制(只在 app 主任务调用)                                          */
/* ------------------------------------------------------------------ */
void AppZdMsg::draw()
{
    auto& cv = GetHAL().canvas();

    cv.fillScreen(C_BG);
    cv.setFont(&fonts::efontCN_12);  /* 正文 12px 点阵; 不用 _b 粗体(这个尺寸会糊) */
    cv.setTextDatum(top_left);
    cv.setTextWrap(false);

    draw_topbar();  /* 系统栏画布上的常驻顶栏 */

    if (s_font_demo) {
        draw_font_demo(cv);
        return;
    }

    switch (_page) {
        case PG_LIST: draw_list(); break;
        case PG_WRITE: draw_write(); break;
        case PG_NODES: draw_nodes(); break;
        case PG_SET: draw_settings(); break;
        default: draw_about(); break;
    }
}

/* 顶栏: 画进系统栏画布(240x26), 与 240x109 内容区拼成整块 240x135 全屏。
 * 常驻: 时间 / 页面标题 / 未读数 / 在线节点数 / 链路状态 / 电量 */
void AppZdMsg::draw_topbar()
{
    auto& bar = GetHAL().canvasSystemBar();
    char buf[24];

    bar.fillScreen(C_BAR);
    bar.setTextDatum(top_left);
    bar.setTextWrap(false);

    /* 时间(14px): 已对时用青色, 未同步显示 --:-- 用告警色 */
    char hm[8];
    clock_hm(hm, sizeof(hm));
    bar.setFont(&fonts::efontCN_14);
    bar.setTextColor(sys_time_valid() ? C_ACCENT : C_WARN);
    bar.drawString(hm, 6, 0);

    const char* title = "消息";
    if (_page == PG_WRITE) title = "写消息";
    else if (_page == PG_NODES) title = "节点";
    else if (_page == PG_SET) title = "设置";
    else if (_page == PG_ABOUT) title = "关于";
    bar.setTextColor(C_FG);
    bar.drawString(title, 58, 0);

    bar.setFont(&fonts::efontCN_12);

    /* 未读(橙) + 在线节点数(灰) */
    if (_unread) {
        snprintf(buf, sizeof(buf), "新%u", (unsigned)_unread);
        bar.setTextColor(C_WARN);
        bar.setTextDatum(top_right);
        bar.drawString(buf, 152, 1);
    }
    snprintf(buf, sizeof(buf), "%d节点", node_online_count());
    bar.setTextColor(C_DIM);
    bar.setTextDatum(top_right);
    bar.drawString(buf, 192, 1);

    /* 链路状态点: 绿=无线就绪, 红=未启用 */
    bar.fillCircle(200, 8, 3, zdlink_ready() ? C_IN : C_ERR);

    uint32_t now = zdlink_now_ms();
    if (_bat_ms == 0 || now - _bat_ms > 2000) {
        _bat_ms = now;
        _bat_level = (int)GetHAL().getBatLevel();
    }
    snprintf(buf, sizeof(buf), "%d%%", _bat_level);
    bar.setTextColor(_bat_level < 20 ? C_ERR : C_DIM);
    bar.setTextDatum(top_right);
    bar.drawString(buf, 237, 1);

    /* 第二行(12px): 当前目标链路 + 上次对端的信号质量, 任何页面都能一眼看到 */
    bar.setTextDatum(top_left);
    char t2[40];
    if (_target) {
        char lb[16];
        uint8_t caps = 0;
        uint32_t bid = pick_bridge(_bridge_id, &caps);
        node_label(lb, sizeof(lb), bid);
        if (!bid) {
            snprintf(t2, sizeof(t2), "目标 R 无桥");
            bar.setTextColor(C_ERR);
        } else if (caps & 0x02) {
            snprintf(t2, sizeof(t2), "目标 R %s", lb);
            bar.setTextColor(0xF81F);
        } else {
            snprintf(t2, sizeof(t2), "目标 R %s 禁发", lb);
            bar.setTextColor(C_WARN);
        }
    } else {
        snprintf(t2, sizeof(t2), "目标 E 直连");
        bar.setTextColor(C_ACCENT);
    }
    bar.drawString(t2, 6, 13);

    int rr = 0;
    if (_last_peer) {
        ZdNodeInfo nd[8];
        int n = zdlink_get_nodes(nd, 8);
        for (int i = 0; i < n; i++) {
            if (nd[i].id == _last_peer) {
                rr = nd[i].lora ? nd[i].lora_rssi : nd[i].rssi;
                break;
            }
        }
    }
    if (rr == 0) {
        snprintf(buf, sizeof(buf), "信号 --");
        bar.setTextColor(C_DIM);
    } else {
        snprintf(buf, sizeof(buf), "%ddBm", rr);
        /* 信号质量配色: 近场/可通/弱 */
        bar.setTextColor(rr > -70 ? C_IN : (rr > -85 ? C_WARN : C_ERR));
    }
    bar.setTextDatum(top_right);
    bar.drawString(buf, 234, 13);
    bar.setTextDatum(top_left);

    bar.drawFastHLine(0, bar.height() - 1, 240, C_ACCENT);
}

void AppZdMsg::draw_footer(const char* hint)
{
    auto& cv = GetHAL().canvas();
    cv.setFont(&fonts::efontCN_12);
    cv.drawFastHLine(0, 91, 240, C_LINE);

    if (_toast[0] && zdlink_now_ms() - _toast_ms < 1800) {
        cv.setTextColor(C_WARN);
        draw_clip(cv, _toast, 4, 94, 232);
        return;
    }
    cv.setTextColor(zdlink_ready() ? C_DIM : C_ERR);
    draw_clip(cv, zdlink_ready() ? hint : "无线未启用: 先开无线广播", 4, 94, 232);
}

void AppZdMsg::draw_list()
{
    auto& cv = GetHAL().canvas();
    char buf[64];

    /* 内容区没有标题栏了(标题在顶栏), 7 行铺满 109px */
    const int shown = 7;
    const int y0 = 0;
    const int step = 13;

    if (s_line_cnt == 0) {
        /* 空列表也把界面撑起来: 左侧状态, 右侧一个信号图标, 别只留一句灰字 */
        cv.setFont(&fonts::efontCN_12);
        cv.setTextColor(C_ACCENT);
        cv.drawString("链路待命", 8, 22);
        cv.setTextColor(C_DIM);
        cv.drawString("直接敲字符就能写消息发给对端", 8, 42);
        cv.drawString("1-5 切页 · G0 退出", 8, 58);

        cv.drawCircle(206, 66, 4, C_ACCENT);
        cv.drawArc(206, 66, 10, 12, 0, 90, C_ACCENT);
        cv.drawArc(206, 66, 18, 20, 0, 90, C_DIM);
    }

    for (int i = 0; i < shown; i++) {
        const Line* m = line_at(_list_scroll + i);
        if (!m) break;
        int y = y0 + i * step;

        if (m->out == 2) {  /* 系统行 */
            cv.setTextColor(C_WARN);
            cv.drawString("!", 2, y);
            draw_clip(cv, m->text, 14, y, 222);
            continue;
        }

        const bool mine = (m->out == 1);
        const bool peer = (!mine && m->src == _last_peer);

        /* 链路标签: E=ESP-NOW L=空口 R=经桥, 后缀 ~ = 被中继过 */
        snprintf(buf, sizeof(buf), "%s%s", via_tag(m->via), m->relay ? "~" : "");
        cv.setTextColor(C_DIM);
        cv.drawString(buf, 2, y);

        /* 来源设备: 橙=无 GPS, 红=带 GPS, 灰=未知(判断不出就不瞎标) */
        DevKind dk = mine ? (DevKind)DEV_NOGPS : dev_kind_of(m->src);
        uint16_t dcol = dev_color(dk);
        cv.fillCircle(20, y + 6, 3, dcol);

        /* 发送者短名 */
        char lb[16];
        if (mine) snprintf(lb, sizeof(lb), "我");
        else node_label(lb, sizeof(lb), m->src);
        cv.setTextColor(dcol);
        cv.drawString(lb, 26, y);

        /* 本次/上次连接的对端: 左侧竖条 + 行尾 » */
        if (peer) cv.drawFastVLine(0, y + 1, 11, C_ACCENT);

        int maxw = m->loop ? 152 : (peer ? 172 : 178);
        cv.setTextColor(mine ? C_OUT : C_FG);
        draw_clip(cv, m->text, 54, y, maxw);

        if (m->loop) {
            cv.setTextColor(C_WARN);
            cv.setTextDatum(top_right);
            cv.drawString("本机", 237, y);
            cv.setTextDatum(top_left);
        } else if (peer) {
            cv.setTextColor(C_ACCENT);
            cv.setTextDatum(top_right);
            cv.drawString("»", 237, y);
            cv.setTextDatum(top_left);
        }
    }

    if (_list_scroll > 0) {
        snprintf(buf, sizeof(buf), "%d/%d", s_line_cnt - _list_scroll, s_line_cnt);
        cv.setTextColor(C_DIM);
        cv.setTextDatum(top_right);
        cv.drawString(buf, 237, 0);
        cv.setTextDatum(top_left);
    }

    draw_footer("1-5切页 ;.滚动 ,/翻页 打字即写 G0退出");
}

void AppZdMsg::draw_write()
{
    auto& cv = GetHAL().canvas();

    /* 输入框(标题在顶栏了, 这里直接从第一行开始) */
    cv.fillRect(2, 1, 236, 21, C_BAR);
    cv.drawRect(2, 1, 236, 21, C_LINE);

    const char* s = _input.c_str();
    size_t n = _input.size();
    size_t st = 0;  /* 只保留能放进框里的尾部, 让光标始终可见 */
    while (st < n && (int)cv.textWidth(s + st) > 224) st++;
    cv.setTextColor(C_FG);
    cv.drawString(s + st, 6, 5);
    if (n == 0) {
        cv.setTextColor(C_DIM);
        cv.drawString("输入文本, Enter 发送", 8, 5);
    } else if (_cursor_on) {
        int cx = 6 + (int)cv.textWidth(s + st);
        if (cx > 233) cx = 233;
        cv.drawFastVLine(cx + 1, 4, 14, C_ACCENT);
    }

    /* 发送目标(始终显示, 避免不知道走哪条路) */
    cv.setTextColor(C_DIM);
    cv.drawString("目标", 2, 27);
    cv.setTextColor(_target ? 0xF81F : C_ACCENT);
    cv.drawString(_target ? "[R] 强制 LoRa" : "[A] 自动:ESP-NOW→LoRa", 32, 27);
    if (_target) {
        uint8_t caps = 0;
        uint32_t bid = pick_bridge(_bridge_id, &caps);
        char lb[16], b[40];
        node_label(lb, sizeof(lb), bid);
        if (!bid) snprintf(b, sizeof(b), "无桥");
        else if (!(caps & 0x02)) snprintf(b, sizeof(b), "桥%s 禁发", lb);
        else snprintf(b, sizeof(b), "桥%s", lb);
        cv.setTextColor((bid && (caps & 0x03) == 0x03) ? C_FG : C_ERR);
        cv.setTextDatum(top_right);
        cv.drawString(b, 237, 27);
        cv.setTextDatum(top_left);
    }

    /* 输入框下方: LNACK 失败原因优先, 否则显示路径提示(协议 §5.3) */
    bool show_lnack = _lnack_txt[0] && (zdlink_now_ms() - _lnack_ms < 6000);
    cv.setTextColor(show_lnack ? C_ERR : C_DIM);
    draw_clip(cv,
              show_lnack ? _lnack_txt
                         : (_target ? "单播给桥, 桥上空口后回 ACK(≤72B)"
                                    : "直发广播, 对端收到后回 ACK"),
              2, 43, 236);

    /* 上次发送状态 */
    uint8_t stx = _last_msgid ? zdlink_tx_state(_last_msgid) : 3;
    uint8_t rsn = _last_msgid ? zdlink_tx_reason(_last_msgid) : 0;
    const char* msg = "";
    uint16_t col = C_DIM;
    if (stx == 1) {
        msg = (_last_via == ZD_VIA_R) ? "上次: 空口已发出(桥 ACK)" : "上次: 已送达(对端 ACK)";
        col = C_IN;
    } else if (stx == 0) {
        msg = (_last_via == ZD_VIA_R) ? "上次: 等空口确认…" : "上次: 等待 ACK…";
        col = C_WARN;
    } else if (stx == 2) {
        if (rsn) {
            msg = reason_text(rsn);
            col = C_ERR;
        } else {
            msg = "上次: 未确认(超时)";
            col = C_ERR;
        }
    }
    if (msg[0]) {
        cv.setTextColor(col);
        draw_clip(cv, msg, 2, 57, 236);
    }

    /* 上次连接的对端: 一眼能看全, 时间戳只留在节点页详情 */
    char lb[16], buf[48];
    if (_last_peer) {
        node_label(lb, sizeof(lb), _last_peer);
        snprintf(buf, sizeof(buf), "对端 %s %s", lb, dev_tag(dev_kind_of(_last_peer)));
        cv.setTextColor(C_ACCENT);
    } else {
        snprintf(buf, sizeof(buf), "对端 无(还没和谁通过信)");
        cv.setTextColor(C_DIM);
    }
    draw_clip(cv, buf, 2, 71, 236);

    draw_footer("Enter 发送  Tab 目标  ^,/. 桥  G0 返回");
}

void AppZdMsg::draw_nodes()
{
    auto& cv = GetHAL().canvas();
    char buf[96];

    ZdNodeInfo nodes[8];
    int n = zdlink_get_nodes(nodes, 8);
    /* 按最近活跃排序(插入排序, n<=8) */
    for (int i = 1; i < n; i++) {
        ZdNodeInfo k = nodes[i];
        int j = i - 1;
        while (j >= 0 && nodes[j].seen_ms < k.seen_ms) {
            nodes[j + 1] = nodes[j];
            j--;
        }
        nodes[j + 1] = k;
    }

    uint32_t now = zdlink_now_ms();
    const int shown = 6;
    if (_node_scroll > (n > shown ? n - shown : 0)) _node_scroll = n > shown ? n - shown : 0;

    for (int i = 0; i < shown; i++) {
        int idx = _node_scroll + i;
        if (idx >= n) break;
        const ZdNodeInfo& nd = nodes[idx];
        int y = i * 13;

        bool on = (now - nd.seen_ms) < 15000;
        DevKind dk = dev_kind_of(nd.id);
        uint16_t dcol = dev_color(dk);
        char lb[16];
        node_label(lb, sizeof(lb), nd.id);

        if (nd.id == _last_peer) cv.drawFastVLine(0, y + 1, 11, C_ACCENT);
        cv.fillCircle(8, y + 6, 3, on ? dcol : C_DIM);

        cv.setTextColor(on ? dcol : C_DIM);
        cv.drawString(lb, 14, y);

        /* 设备类型(按节点字段判断): 无GPS / 带GPS / 未知 */
        cv.setTextColor(dcol);
        cv.drawString(dev_tag(dk), 40, y);

        /* 链路: L=空口 E=直连; 离线加 x */
        cv.setTextColor(C_DIM);
        snprintf(buf, sizeof(buf), "%s%s", nd.lora ? "L" : "E", on ? "" : "x");
        cv.drawString(buf, 78, y);

        int rr = nd.lora ? nd.lora_rssi : nd.rssi;
        if (rr == 0) snprintf(buf, sizeof(buf), "-");  /* BEAT 不带 RSSI */
        else snprintf(buf, sizeof(buf), "%d", rr);
        cv.drawString(buf, 92, y);

        char snrb[10];
        if (nd.snr == 127) {
            snprintf(snrb, sizeof(snrb), "S--");
        } else {
            /* 协议 §3 的 snr4 = SNR x 4, 显示成一位小数(17 -> 4.2) */
            int v = (int)nd.snr;
            int a = v < 0 ? -v : v;
            snprintf(snrb, sizeof(snrb), "S%s%d.%d", v < 0 ? "-" : "", a / 4,
                     (a % 4) * 25 / 10);
        }
        cv.drawString(snrb, 120, y);

        char hb[8];
        if (nd.lora && nd.hops == 255) snprintf(hb, sizeof(hb), "h?");
        else snprintf(hb, sizeof(hb), "h%u", (unsigned)nd.hops);
        cv.drawString(hb, 156, y);

        /* 新鲜度: 最后在线距今(秒/分), 与 RSSI/hops 一起判断"近且新" */
        uint32_t age = (now - nd.seen_ms) / 1000;
        char ab[8];
        if (age < 60) snprintf(ab, sizeof(ab), "%us", (unsigned)age);
        else snprintf(ab, sizeof(ab), "%um", (unsigned)(age / 60));
        cv.setTextColor(on ? C_DIM : C_ERR);
        cv.drawString(ab, 172, y);
        cv.setTextColor(C_DIM);

        if (nd.batt_pct != 255) {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)nd.batt_pct);
            cv.setTextDatum(top_right);
            cv.drawString(buf, 237, y);
            cv.setTextDatum(top_left);
        }
    }

    if (n == 0) {
        cv.setTextColor(C_DIM);
        cv.drawString("暂无节点", 8, 34);
        cv.drawString("等待心跳…", 8, 50);
    }

    /* 上次连接的对端(时间戳只放在这一行) */
    if (_last_peer) {
        const ZdNodeInfo* p = nullptr;
        for (int i = 0; i < n; i++) {
            if (nodes[i].id == _last_peer) { p = &nodes[i]; break; }
        }
        char lb[16], ago[12], rss[10];
        node_label(lb, sizeof(lb), _last_peer);
        int rr = p ? (int)(p->lora ? p->lora_rssi : p->rssi) : 0;
        if (rr == 0) snprintf(rss, sizeof(rss), "信号-");  /* 未知不显示 0dBm */
        else snprintf(rss, sizeof(rss), "%ddBm", rr);
        if (!p) snprintf(ago, sizeof(ago), "已离线");
        else if (now - p->seen_ms >= 15000) snprintf(ago, sizeof(ago), "已离线");
        else fmt_ago(ago, sizeof(ago), now - p->seen_ms);
        snprintf(buf, sizeof(buf), "上次 %s %s %s %s", lb, dev_tag(dev_kind_of(_last_peer)), rss,
                 ago);
        cv.setTextColor(C_ACCENT);
    } else {
        snprintf(buf, sizeof(buf), "上次连接: 无");
        cv.setTextColor(C_DIM);
    }
    draw_clip(cv, buf, 2, 79, 236);

    draw_footer("1-5切页 ;.滚动 ,/翻页 G0返回");
}

void AppZdMsg::draw_settings()
{
    auto& cv = GetHAL().canvas();

    static const char* kNames[6] = {"静音", "音量", "亮度", "参与中继", "时间", "时区"};
    char vals[6][24];
    snprintf(vals[0], sizeof(vals[0]), "%s", _mute ? "开" : "关");
    snprintf(vals[1], sizeof(vals[1]), "%d/255", _vol);
    snprintf(vals[2], sizeof(vals[2]), "%d%%", _bright);
    snprintf(vals[3], sizeof(vals[3]), "%s", _relay_on ? "开" : "关");
    if (_time_edit) {
        snprintf(vals[4], sizeof(vals[4]), "%02d:%02d *", _edit_h, _edit_m);
    } else {
        char hm[8];
        clock_hm(hm, sizeof(hm));
        snprintf(vals[4], sizeof(vals[4]), "%s %s", hm, time_src_name());
    }
    snprintf(vals[5], sizeof(vals[5]), "UTC%+d", _tz);

    for (int i = 0; i < 6; i++) {
        int y = i * 14;
        bool sel = (i == _set_sel);
        if (sel) {
            cv.fillRect(2, y, 236, 13, C_SEL);
            cv.setTextColor(C_ACCENT);
            cv.drawString(">", 5, y);
        }
        cv.setTextColor(sel ? C_FG : C_DIM);
        cv.drawString(kNames[i], 15, y);

        cv.setTextDatum(top_right);
        cv.setTextColor(sel ? C_ACCENT : C_FG);
        cv.drawString(vals[i], 236, y);
        cv.setTextDatum(top_left);
    }

    if (_time_edit) {
        draw_footer(";.改时 ,/改分 Enter存 Esc/G0取消");
    } else {
        draw_footer(";. 选择  ,/ 调整  Enter 切  G0 返回");
    }
}

void AppZdMsg::draw_about()
{
    auto& cv = GetHAL().canvas();
    char buf[88];

    cv.setTextColor(C_FG);
    cv.drawString("天链终端  v0.7 · ZD v1.2", 4, 0);

    cv.setTextColor(C_DIM);
    cv.drawString("E直连 L空口 R经桥 ~中继", 4, 13);

    /* 颜色含义写在这里, 列表页就不用再占宽度 */
    cv.setTextColor(C_WARN);
    cv.drawString("橙=无GPS", 4, 26);
    cv.setTextColor(C_ERR);
    cv.drawString("红=带GPS", 74, 26);
    cv.setTextColor(C_DIM);
    cv.drawString("灰=未知", 148, 26);

    snprintf(buf, sizeof(buf), "本机 #%04lX  0x%08lX", (unsigned long)(zdlink_my_id() & 0xFFFF),
             (unsigned long)zdlink_my_id());
    cv.drawString(buf, 4, 39);

    char hm[8];
    clock_hm(hm, sizeof(hm));
    snprintf(buf, sizeof(buf), "时间 %s  %s  UTC%+d", hm, time_src_name(), _tz);
    cv.drawString(buf, 4, 52);

    snprintf(buf, sizeof(buf), "电池 %d%%  堆 %uK", _bat_level,
             (unsigned)(esp_get_free_heap_size() / 1024));
    cv.drawString(buf, 4, 65);

    cv.setTextColor(C_ACCENT);
    cv.drawString("1-5/,/ 切页  G0 返回", 4, 78);

    draw_footer("顶栏常驻: 时间 未读 节点 链路 电量");
}

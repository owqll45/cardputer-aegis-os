#include "app_settings.h"

#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <hal/utils/sys_time.h>
#include <apps/utils/ui_kit.h>
#include <apps/utils/ui_theme.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/icons/icon_settings_generated.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_sntp.h>

/* ------------------------------------------------------------------ */
/*  按键队列：回调只入队，绘制只在 app 主任务                          */
/* ------------------------------------------------------------------ */
static portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_q[8];
static volatile int s_q_head = 0;
static volatile int s_q_tail = 0;
static volatile int s_held_key = -1;
static volatile uint32_t s_held_ms = 0;

static void q_push(int code)
{
    portENTER_CRITICAL(&s_q_mux);
    int next = (s_q_head + 1) % 8;
    if (next != s_q_tail) {
        s_q[s_q_head] = code;
        s_q_head = next;
    }
    portEXIT_CRITICAL(&s_q_mux);
}

static bool q_pop(int& code)
{
    bool ok = false;
    portENTER_CRITICAL(&s_q_mux);
    if (s_q_tail != s_q_head) {
        code = s_q[s_q_tail];
        s_q_tail = (s_q_tail + 1) % 8;
        ok = true;
    }
    portEXIT_CRITICAL(&s_q_mux);
    return ok;
}

static void q_reset()
{
    portENTER_CRITICAL(&s_q_mux);
    s_q_head = 0;
    s_q_tail = 0;
    portEXIT_CRITICAL(&s_q_mux);
    s_held_key = -1;
}

/* ------------------------------------------------------------------ */
/*  常量                                                               */
/* ------------------------------------------------------------------ */
static const std::string _tag = "Settings";

enum { K_UP = 1, K_DOWN, K_LEFT, K_RIGHT, K_ENTER };

static int map_key(int code)
{
    switch (code) {
        case KEY_UP:
        case KEY_W: return K_UP;
        case KEY_DOWN:
        case KEY_S: return K_DOWN;
        case KEY_LEFT:
        case KEY_A: return K_LEFT;
        case KEY_RIGHT:
        case KEY_D: return K_RIGHT;
        case KEY_ENTER:
        case KEY_KPENTER: return K_ENTER;
        default: return 0;
    }
}

static bool sta_has_ip()
{
    esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!nif) return false;
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(nif, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

static volatile bool s_ntp_synced = false;
static void sntp_cb(struct timeval*)
{
    s_ntp_synced = true;
}

/* ------------------------------------------------------------------ */
/*  生命周期                                                           */
/* ------------------------------------------------------------------ */
AppSettings::AppSettings()
{
    setAppInfo().name = "设置";
}

AppSettings::~AppSettings() {}

void AppSettings::onOpen()
{
    mclog::tagInfo(_tag, "opened");

    loadValues();
    _sel = 0;
    _scroll = 0;
    _dirty = true;
    _show_about = false;
    _confirm_reset = false;
    _toast[0] = 0;
    _ntp_state = s_ntp_synced ? 2 : 0;
    _last_refresh = 0;
    q_reset();

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([](const Keyboard::KeyEvent_t& k) {
        if (k.isModifier) return;
        int code = (int)k.keyCode;
        if (!k.state) {
            if (code == s_held_key) s_held_key = -1;
            return;
        }
        s_held_key = code;
        s_held_ms = GetHAL().millis();
        q_push(code);
    });

    ESP_LOGI(_tag.c_str(), "SETOPEN heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void AppSettings::onClose()
{
    mclog::tagInfo(_tag, "closed");
    if (_key_slot != -1) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    q_reset();
    _show_about = false;
    _confirm_reset = false;

    // 立即落盘，保证重启后设置还在
    GetHAL().getSettings().Sync();
}

void AppSettings::loadValues()
{
    auto& st = GetHAL().getSettings();
    _nv_vol    = st.GetInt("sound_volume", 128);
    if (_nv_vol < 0) _nv_vol = 0;
    if (_nv_vol > 255) _nv_vol = 255;
    _nv_bright = st.GetInt("brightness", 100);
    if (_nv_bright < 10) _nv_bright = 10;
    if (_nv_bright > 100) _nv_bright = 100;
    _nv_accent = st.GetInt("accent", 0);
    if (_nv_accent < 0 || _nv_accent >= UI_ACCENT_COUNT) _nv_accent = 0;
    _nv_tz   = st.GetInt("tz_offset", 8);
    _nv_mute = st.GetBool("mute", false);
    _nv_wifi = st.GetBool("wifi_on", true);
    _nv_link = st.GetBool("glink_on", true);
}

/* ------------------------------------------------------------------ */
/*  操作                                                               */
/* ------------------------------------------------------------------ */
void AppSettings::moveSel(int delta)
{
    _sel += delta;
    if (_sel < 0) _sel = IT_COUNT - 1;
    if (_sel >= IT_COUNT) _sel = 0;
    _dirty = true;
}

void AppSettings::adjust(int delta)
{
    auto& st = GetHAL().getSettings();

    switch (_sel) {
        case IT_VOLUME: {
            // 每步 13/255 ≈ 5%
            _nv_vol += delta * 13;
            if (_nv_vol < 0) _nv_vol = 0;
            if (_nv_vol > 255) _nv_vol = 255;
            if (_nv_mute) {
                _nv_mute = false;
                st.SetBool("mute", false);
                GetHAL().setGlobalMute(false);
            }
            st.SetInt("sound_volume", _nv_vol);
            GetHAL().speaker.setVolume(_nv_vol);
            break;
        }
        case IT_BRIGHT: {
            _nv_bright += delta * 5;
            if (_nv_bright < 10) _nv_bright = 10;
            if (_nv_bright > 100) _nv_bright = 100;
            st.SetInt("brightness", _nv_bright);
            M5.Display.setBrightness((uint8_t)(_nv_bright * 255 / 100));
            break;
        }
        case IT_ACCENT: {
            _nv_accent = (_nv_accent + delta + UI_ACCENT_COUNT) % UI_ACCENT_COUNT;
            st.SetInt("accent", _nv_accent);
            uiAccentRefresh();
            break;
        }
        case IT_TZ: {
            _nv_tz += delta;
            if (_nv_tz < -12) _nv_tz = 14;
            if (_nv_tz > 14) _nv_tz = -12;
            st.SetInt("tz_offset", _nv_tz);
            applyTimeZone(_nv_tz);
            break;
        }
        case IT_MUTE:
        case IT_WIFI:
        case IT_LINK:
            activate();   // 开关类：左右键等同 ENTER
            break;
        default:
            break;
    }
    _dirty = true;
}

void AppSettings::activate()
{
    auto& st = GetHAL().getSettings();

    switch (_sel) {
        case IT_MUTE: {
            _nv_mute = !_nv_mute;
            st.SetBool("mute", _nv_mute);
            GetHAL().setGlobalMute(_nv_mute);
            setToast(_nv_mute ? "已静音" : "已取消静音");
            break;
        }
        case IT_WIFI: {
            _nv_wifi = !_nv_wifi;
            st.SetBool("wifi_on", _nv_wifi);
            if (_nv_wifi) {
                GetHAL().wifiInit();
                ESP_LOGI(_tag.c_str(), "WIFI on (init)");
                setToast("WiFi 已开启");
            } else {
                GetHAL().wifiDeinit();
                ESP_LOGI(_tag.c_str(), "WIFI off (deinit)");
                setToast("WiFi 已关闭（广播随之停止）");
            }
            break;
        }
        case IT_LINK: {
            _nv_link = !_nv_link;
            st.SetBool("glink_on", _nv_link);
            glink_set_enabled(_nv_link);
            setToast(_nv_link ? "无线广播已开启" : "无线广播已关闭");
            break;
        }
        case IT_BT:
            setToast("蓝牙未编译进固件，当前不可用");
            break;
        case IT_NTP:
            startNtp();
            break;
        case IT_ABOUT:
            _show_about = true;
            break;
        case IT_RESET:
            if (!_confirm_reset) {
                _confirm_reset = true;
                _confirm_ms = GetHAL().millis();
            } else {
                resetDefaults();
            }
            break;
        default:
            break;
    }
    _dirty = true;
}

void AppSettings::setToast(const char* msg)
{
    strncpy(_toast, msg, sizeof(_toast) - 1);
    _toast[sizeof(_toast) - 1] = 0;
    _toast_ms = GetHAL().millis();
    _dirty = true;
}

void AppSettings::startNtp()
{
    if (!_nv_wifi || !sta_has_ip()) {
        _ntp_state = 3;
        setToast("先连接 WiFi 才能校时");
        _dirty = true;
        return;
    }

    s_ntp_synced = false;
    esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_cb);
    esp_sntp_init();

    _ntp_state = 1;
    _ntp_ms = GetHAL().millis();
    setToast("正在向网络校时…");
}

void AppSettings::resetDefaults()
{
    auto& st = GetHAL().getSettings();
    st.EraseAll();

    _nv_vol    = 128;
    _nv_bright = 100;
    _nv_accent = 0;
    _nv_tz     = 8;
    _nv_mute   = false;
    _nv_wifi   = true;
    _nv_link   = true;

    st.SetInt("sound_volume", _nv_vol);
    st.SetInt("brightness", _nv_bright);
    st.SetInt("accent", _nv_accent);
    st.SetInt("tz_offset", _nv_tz);
    st.SetBool("mute", false);
    st.SetBool("wifi_on", true);
    st.SetBool("glink_on", true);

    GetHAL().setGlobalMute(false);
    GetHAL().speaker.setVolume(_nv_vol);
    M5.Display.setBrightness((uint8_t)(_nv_bright * 255 / 100));
    applyTimeZone(_nv_tz);
    uiAccentRefresh();
    glink_set_enabled(true);
    st.Sync();

    _confirm_reset = false;
    setToast("已恢复默认设置");
}

/* ------------------------------------------------------------------ */
/*  主循环                                                             */
/* ------------------------------------------------------------------ */
void AppSettings::onRunning()
{
    uint32_t now = GetHAL().millis();

    if (GetHAL().goButton().wasClicked()) {
        if (_show_about) {
            _show_about = false;
            _dirty = true;
        } else if (_confirm_reset) {
            _confirm_reset = false;
            _dirty = true;
        } else {
            close();
            return;
        }
    }

    // 确认重置超时
    if (_confirm_reset && now - _confirm_ms > 4000) {
        _confirm_reset = false;
        _dirty = true;
    }

    // 长按连发
    int held = s_held_key;
    if (held >= 0 && !_show_about && !_confirm_reset) {
        if (now - s_held_ms > 450 && now - _repeat_ms > 110) {
            _repeat_ms = now;
            int m = map_key(held);
            if (m == K_UP) moveSel(-1);
            else if (m == K_DOWN) moveSel(1);
            else if (m == K_LEFT) adjust(-1);
            else if (m == K_RIGHT) adjust(1);
        }
    }

    int code;
    while (q_pop(code)) {
        if (code == KEY_ESC) { close(); return; }
        if (_show_about) {
            _show_about = false;
            _dirty = true;
            continue;
        }
        if (_confirm_reset) {
            if (map_key(code) == K_ENTER) {
                resetDefaults();
            } else {
                _confirm_reset = false;
                _dirty = true;
            }
            continue;
        }
        int m = map_key(code);
        if (!m) continue;
        if (m == K_UP) moveSel(-1);
        else if (m == K_DOWN) moveSel(1);
        else if (m == K_LEFT) adjust(-1);
        else if (m == K_RIGHT) adjust(1);
        else activate();
    }

    // 校时状态
    if (_ntp_state == 1) {
        if (s_ntp_synced) {
            _ntp_state = 2;
            setToast("校时完成");
        } else if (now - _ntp_ms > 15000) {
            _ntp_state = 3;
            setToast("校时超时，检查 WiFi");
        }
    }

    // toast 过期
    if (_toast[0] && now - _toast_ms > 1600) {
        _toast[0] = 0;
        _dirty = true;
    }

    // 电池/内存/WiFi 状态每秒刷新
    if (now - _last_refresh >= 1000) {
        _last_refresh = now;
        _dirty = true;
    }

    if (!_dirty) return;
    _dirty = false;

    if (_show_about) {
        drawAbout();
    } else {
        draw();
    }
    GetHAL().pushCanvas();
}

/* ------------------------------------------------------------------ */
/*  绘制                                                               */
/* ------------------------------------------------------------------ */
const char* AppSettings::valueText(int item, char* buf, size_t n, uint16_t* color)
{
    switch (item) {
        case IT_VOLUME:
            snprintf(buf, n, "%d%%", (int)(_nv_vol * 100 / 255));
            break;
        case IT_MUTE:
            *color = _nv_mute ? TFT_RED : TFT_GREEN;
            return _nv_mute ? "静音中" : "正常";
        case IT_BRIGHT:
            snprintf(buf, n, "%d%%", _nv_bright);
            break;
        case IT_ACCENT:
            *color = uiAccentPalette(_nv_accent);
            return uiAccentName(_nv_accent);
        case IT_WIFI:
            if (!_nv_wifi) {
                *color = UI_SUB;
                return "关";
            }
            if (GetHAL().isApActive()) {
                *color = TFT_ORANGE;
                return "开·热点";
            }
            if (sta_has_ip()) {
                *color = TFT_GREEN;
                return "开·有IP";
            }
            *color = TFT_YELLOW;
            return "开·无IP";
        case IT_LINK:
            *color = _nv_link ? TFT_GREEN : UI_SUB;
            return _nv_link ? "广播中" : "已关";
        case IT_BT:
            *color = UI_SUB;
            return "未编译";
        case IT_TZ:
            snprintf(buf, n, "UTC%+d", _nv_tz);
            break;
        case IT_NTP:
            switch (_ntp_state) {
                case 1: *color = TFT_YELLOW; return "校时中";
                case 2: *color = TFT_GREEN;  return "已同步";
                case 3: *color = TFT_RED;    return "需WiFi";
                default: *color = UI_SUB;    return "未校时";
            }
        case IT_BAT: {
            int lv = M5.Power.getBatteryLevel();
            bool ch = M5.Power.isCharging();
            if (lv < 0) snprintf(buf, n, "未知");
            else if (ch) snprintf(buf, n, "%d%% 充电", lv);
            else snprintf(buf, n, "%d%%", lv);
            *color = (lv >= 0 && lv < 20) ? TFT_RED : TFT_GREEN;
            break;
        }
        case IT_MEM:
            snprintf(buf, n, "剩%uK", (unsigned)(esp_get_free_heap_size() / 1024));
            break;
        case IT_ABOUT:
            *color = UI_SUB;
            return "ENTER";
        case IT_RESET:
            *color = TFT_ORANGE;
            return "ENTER";
        default:
            buf[0] = 0;
            break;
    }
    return buf;
}

void AppSettings::draw()
{
    auto& cv = GetHAL().canvas();
    const int W = cv.width();
    const int H = cv.height();
    uint16_t accent = uiAccentColor();

    cv.fillScreen(UI_BG);

    // 标题栏 20px
    cv.fillRect(0, 0, W, 20, 0x1082);
    cv.fillRect(0, 0, 3, 20, accent);
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(TFT_WHITE);
    cv.setCursor(8, 4);
    cv.print("系统设置");

    char pos[16];
    snprintf(pos, sizeof(pos), "%d/%d", _sel + 1, (int)IT_COUNT);
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(0x7BEF);
    cv.setCursor(W - 6 - cv.textWidth(pos), 6);
    cv.print(pos);
    cv.drawFastHLine(0, 20, W, UI_LINE);

    const int rowH  = 15;
    const int rows  = 5;
    const int listY = 22;

    static const char* kLabels[IT_COUNT] = {
        "音量", "静音", "屏幕亮度", "配色", "WiFi", "无线广播", "蓝牙键盘",
        "时区", "网络校时", "电池", "内存", "关于本机", "恢复默认"
    };

    if (_sel < _scroll) _scroll = _sel;
    if (_sel >= _scroll + rows) _scroll = _sel - rows + 1;
    if (_scroll > IT_COUNT - rows) _scroll = IT_COUNT - rows;
    if (_scroll < 0) _scroll = 0;

    for (int r = 0; r < rows; r++) {
        int i = _scroll + r;
        if (i >= IT_COUNT) break;
        int y = listY + r * rowH;
        bool sel = (i == _sel);

        if (sel) {
            cv.fillRoundRect(2, y, W - 4, rowH - 1, 4, UI_CARD_HI);
            cv.fillRect(2, y + 2, 2, rowH - 5, accent);
        }

        cv.setFont(&fonts::efontCN_12);
        cv.setTextColor(sel ? TFT_WHITE : 0xCE79);
        cv.setCursor(8, y + 1);
        cv.print(kLabels[i]);

        char vb[24] = {};
        uint16_t vc = accent;
        const char* v = valueText(i, vb, sizeof(vb), &vc);
        cv.setTextColor(vc);
        cv.setCursor(W - 6 - cv.textWidth(v), y + 1);
        cv.print(v);
    }

    // 滚动条
    if (IT_COUNT > rows) {
        int barH = rows * rowH;
        int th = barH * rows / IT_COUNT;
        if (th < 6) th = 6;
        int ty = listY + (barH - th) * _scroll / (IT_COUNT - rows);
        cv.fillRect(W - 2, listY, 2, barH, 0x2104);
        cv.fillRect(W - 2, ty, 2, th, accent);
    }

    // 底部键位提示（画在屏幕上）；有临时提示时占用这一行，避免盖住列表
    cv.setFont(&fonts::efontCN_10);
    if (_toast[0]) {
        cv.setTextColor(TFT_ORANGE);
        cv.setCursor(5, H - 12);
        cv.print(_toast);
    } else if (_confirm_reset) {
        cv.setTextColor(TFT_ORANGE);
        cv.setCursor(5, H - 12);
        cv.print("再按 ENTER 确认重置  其它键取消");
    } else {
        cv.setTextColor(0x6B4D);
        cv.setCursor(5, H - 12);
        cv.print("↑↓选择 ←→调整 ENTER执行 G0/Esc返回");
    }
}

void AppSettings::drawAbout()
{
    auto& cv = GetHAL().canvas();
    const int W = cv.width();
    const int H = cv.height();
    uint16_t accent = uiAccentColor();

    cv.fillScreen(UI_BG);
    cv.fillRect(0, 0, W, 20, 0x1082);
    cv.fillRect(0, 0, 3, 20, accent);
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(TFT_WHITE);
    cv.setCursor(8, 4);
    cv.print("关于本机");

    char line[8][56];
    int n = 0;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(line[n++], sizeof(line[0]), "芯片 ESP32-S3  %d核 %dMHz",
             chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    const esp_app_desc_t* app = esp_app_get_description();
    snprintf(line[n++], sizeof(line[0]), "固件 %.20s v%.12s", app->project_name, app->version);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (ps_total > 0) {
        snprintf(line[n++], sizeof(line[0]), "Flash %luMB  PSRAM %uKB",
                 (unsigned long)(flash_size / (1024 * 1024)), (unsigned)(ps_total / 1024));
    } else {
        snprintf(line[n++], sizeof(line[0]), "Flash %luMB  PSRAM 无",
                 (unsigned long)(flash_size / (1024 * 1024)));
    }

    snprintf(line[n++], sizeof(line[0]), "堆 %uK  最低 %uK  块 %uK",
             (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(esp_get_minimum_free_heap_size() / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(line[n++], sizeof(line[0]), "MAC %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    unsigned long sec = (unsigned long)(esp_timer_get_time() / 1000000ULL);
    snprintf(line[n++], sizeof(line[0]), "运行 %02lu:%02lu:%02lu",
             sec / 3600, (sec % 3600) / 60, sec % 60);

    time_t now_t = time(nullptr);
    struct tm tmv;
    localtime_r(&now_t, &tmv);
    snprintf(line[n++], sizeof(line[0]), "本地 %02d:%02d:%02d UTC%+d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, _nv_tz);

    const int lineH = 11;
    int y = 24;
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(0xCE79);
    for (int i = 0; i < n && y + lineH <= H - 13; i++) {
        cv.setCursor(6, y);
        cv.print(line[i]);
        y += lineH;
    }

    cv.setTextColor(0x6B4D);
    cv.setCursor(5, H - 12);
    cv.print("任意键返回设置");
}

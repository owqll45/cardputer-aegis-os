#include "app_set_wifi.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include "config_store.h"
#include <cstring>
#include <esp_wifi.h>
#include "../utils/ui_kit.h"

static uint32_t app_millis() { return esp_timer_get_time() / 1000; }

AppSetWiFi::AppSetWiFi() {
    setAppInfo().name = "Network";
}

AppSetWiFi::~AppSetWiFi() {}

void AppSetWiFi::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _page = PAGE_MAIN;
    _state = ST_IDLE;
    _sel = 0;
    _scroll = 0;
    _input.clear();
    _msg.clear();
    _scan_req = false;
    _connect_req = false;
    _exit_req = false;

    // 默认关闭：只有之前已连接（例如 AI 应用连过）才显示为开启
    _wifi_on = GetHAL().isWifiConnected();

    _key_slot = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& e) { handle_key(e); });
}

void AppSetWiFi::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）——子页面回上一级，主页面退出
    if (GetHAL().goButton().wasClicked()) {
        if (_page != PAGE_MAIN) {
            _page = PAGE_MAIN;
            _state = ST_IDLE;
            _input.clear();
        } else {
            close();
        }
        return;
    }
    if (_exit_req) { close(); return; }

    // 耗时操作放在这里，避免在按键回调里阻塞
    if (_scan_req) { _scan_req = false; do_scan(); }
    if (_connect_req) { _connect_req = false; do_connect(); }

    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);

    if (_page == PAGE_MAIN) {
        ui_title(cv, "网络");
        render_main();
    } else if (_page == PAGE_SCAN) {
        ui_title(cv, "选择 WiFi");
        render_scan();
    } else {
        ui_title(cv, _need_ssid ? "输入名称" : "输入密码");
        render_input();
    }

    GetHAL().pushCanvas();
}

void AppSetWiFi::onClose() {
    mclog::tagInfo(getAppInfo().name, "closed");
    if (_key_slot >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
}

// ---------------------------------------------------------------------------
// 主页面
// ---------------------------------------------------------------------------
void AppSetWiFi::render_main() {
    auto& cv = GetHAL().canvas();

    // 状态卡片
    ui_card(cv, 6, 27, 228, 26, _sel == 0);
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(UI_TEXT);
    cv.setCursor(14, 33);
    cv.print("WiFi");

    bool connected = GetHAL().isWifiConnected();
    const char* state_txt = _wifi_on ? (connected ? "已连接" : "已开启") : "关闭";
    uint16_t state_col = _wifi_on ? (connected ? UI_OK : UI_WARN) : UI_SUB;

    cv.setTextColor(state_col);
    cv.setTextDatum(top_right);
    cv.drawString(state_txt, 184, 33);
    cv.setTextDatum(top_left);

    // 开关图形
    int sx = 192, sy = 36;
    cv.fillRoundRect(sx, sy, 34, 14, 7, _wifi_on ? UI_OK : 0x39E7);
    cv.fillCircle(_wifi_on ? sx + 27 : sx + 7, sy + 7, 5, TFT_WHITE);

    // 菜单项
    const char* items[] = {"扫描附近网络", "手动输入 SSID", "关闭 WiFi 释放内存"};
    for (int i = 0; i < 3; i++) {
        int y = 57 + i * 12;
        bool hi = (_sel == i + 1);
        cv.setFont(&fonts::efontCN_10_b);
        cv.setTextColor(hi ? UI_ACCENT : UI_TEXT);
        cv.setCursor(14, y);
        cv.print(hi ? "> " : "  ");
        cv.print(items[i]);
    }

    ui_hint(cv, "W/S 选择  Enter 确认  G0 退出");
}

// ---------------------------------------------------------------------------
// 扫描结果页
// ---------------------------------------------------------------------------
void AppSetWiFi::render_scan() {
    auto& cv = GetHAL().canvas();

    if (_state == ST_SCANNING) {
        cv.setFont(&fonts::efontCN_12);
        cv.setTextColor(UI_WARN);
        cv.setTextDatum(top_center);
        cv.drawString("正在扫描附近 WiFi…", 120, 48);
        cv.setTextDatum(top_left);
        ui_hint(cv, "请稍候");
        return;
    }

    if (_aps.empty()) {
        cv.setFont(&fonts::efontCN_12);
        cv.setTextColor(UI_SUB);
        cv.setTextDatum(top_center);
        cv.drawString("没有发现网络", 120, 44);
        cv.drawString("请打开 WiFi 或手动输入", 120, 62);
        cv.setTextDatum(top_left);
        ui_hint(cv, "G0 返回");
        return;
    }
    int visible = 4;
    if (_sel < _scroll) _scroll = _sel;
    if (_sel >= _scroll + visible) _scroll = _sel - visible + 1;

    char buf[40];
    for (int i = 0; i < visible; i++) {
        int idx = _scroll + i;
        if (idx >= (int)_aps.size()) break;
        int y = 27 + i * 17;
        bool hi = (idx == _sel);
        ui_card(cv, 6, y, 228, 16, hi);

        cv.setFont(&fonts::efontCN_10_b);
        cv.setTextColor(hi ? UI_ACCENT : UI_TEXT);
        cv.setCursor(12, y + 3);
        // 过长名称截断
        std::string name = _aps[idx].second;
        if (name.size() > 20) name = name.substr(0, 19) + "…";
        cv.print(name.c_str());

        int rssi = _aps[idx].first;
        float ratio = (rssi + 95) / 60.0f;
        ui_bar(cv, 176, y + 5, 40, 6, ratio, rssi > -60 ? UI_OK : rssi > -75 ? UI_WARN : UI_BAD);

        snprintf(buf, sizeof(buf), "%d", rssi);
        cv.setTextColor(UI_SUB);
        cv.setTextDatum(top_right);
        cv.drawString(buf, 226, y + 3);
        cv.setTextDatum(top_left);
    }

    ui_hint(cv, "W/S 选择  Enter 连接  G0 返回");
}

// ---------------------------------------------------------------------------
// 输入页
// ---------------------------------------------------------------------------
void AppSetWiFi::render_input() {
    auto& cv = GetHAL().canvas();

    if (!_need_ssid && !_ssid.empty()) {
        cv.setFont(&fonts::efontCN_10_b);
        cv.setTextColor(UI_SUB);
        cv.setCursor(8, 27);
        cv.printf("目标网络：%s", _ssid.c_str());
    }

    ui_card(cv, 6, 39, 228, 26, true);

    // 密码用圆点显示
    std::string shown;
    if (_need_ssid) {
        shown = _input;
    } else {
        shown.assign(_input.size(), '*');
    }
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(UI_TEXT);
    cv.setCursor(12, 45);
    if (shown.empty()) {
        cv.setTextColor(UI_SUB);
        cv.print(_need_ssid ? "输入网络名称" : "输入密码（可留空）");
    } else {
        cv.print(shown.c_str());
    }

    if (app_millis() - _cursor_t > 500) {
        _cursor = !_cursor;
        _cursor_t = app_millis();
    }
    if (_cursor) {
        int cx = 12 + cv.textWidth(shown.c_str());
        cv.drawFastVLine(cx + 1, 44, 14, UI_ACCENT);
    }

    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    cv.setCursor(8, 71);
    cv.print("字母 / 数字 / 空格 / .  删除键退格");

    cv.setCursor(8, 84);
    if (_state == ST_CONNECTING) {
        cv.setTextColor(UI_WARN);
        cv.print("正在连接，请稍候…");
    } else if (_state == ST_OK) {
        cv.setTextColor(UI_OK);
        cv.print("连接成功！");
    } else if (_state == ST_FAIL) {
        cv.setTextColor(UI_BAD);
        cv.print("连接失败，请检查密码后重试");
    }

    ui_hint(cv, "Enter 确认  G0 返回");
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------
void AppSetWiFi::handle_key(const Keyboard::KeyEvent_t& e) {
    if (!e.state || e.isModifier) return;
    bool up = (e.keyCode == KEY_W || e.keyCode == KEY_E);
    bool down = (e.keyCode == KEY_S);

    if (_page == PAGE_MAIN) {
        if (up && _sel > 0) _sel--;
        if (down && _sel < 3) _sel++;
        if (e.keyCode == KEY_A || e.keyCode == KEY_D) {
            if (_sel == 0) {
                if (_wifi_on) { power_off(); }
                else { GetHAL().wifiInit(); _wifi_on = true; }
            }
        }
        if (e.keyCode == KEY_ENTER || e.keyCode == KEY_SPACE) {
            if (_sel == 0) {
                if (_wifi_on) { power_off(); }
                else { GetHAL().wifiInit(); _wifi_on = true; }
            } else if (_sel == 1) {
                _page = PAGE_SCAN;
                _sel = 0;
                _scroll = 0;
                _aps.clear();
                _state = ST_SCANNING;
                _scan_req = true;
            } else if (_sel == 2) {
                _page = PAGE_INPUT;
                _need_ssid = true;
                _input.clear();
                _state = ST_IDLE;
            } else if (_sel == 3) {
                power_off();
                _msg = "WiFi 已关闭";
            }
        }
        return;
    }

    if (_page == PAGE_SCAN) {
        if (up && _sel > 0) _sel--;
        if (down && _sel + 1 < (int)_aps.size()) _sel++;
        if (e.keyCode == KEY_ENTER && !_aps.empty()) {
            _ssid = _aps[_sel].second;
            _password.clear();
            _input.clear();
            _need_ssid = false;
            _page = PAGE_INPUT;
            _state = ST_IDLE;
        }
        return;
    }

    // PAGE_INPUT
    if (e.keyCode == KEY_BACKSPACE) { if (!_input.empty()) _input.pop_back(); return; }
    if (e.keyCode == KEY_ENTER) {
        if (_need_ssid) {
            if (_input.empty()) return;
            _ssid = _input;
            _input.clear();
            _need_ssid = false;
            _state = ST_IDLE;
        } else {
            _password = _input;
            _state = ST_CONNECTING;
            _connect_req = true;
        }
        return;
    }
    if (_input.size() >= 63) return;
    if (e.keyCode >= KEY_A && e.keyCode <= KEY_Z) _input += (char)('a' + (e.keyCode - KEY_A));
    else if (e.keyCode >= KEY_0 && e.keyCode <= KEY_9) _input += (char)('0' + (e.keyCode - KEY_0));
    else if (e.keyCode == KEY_SPACE) _input += ' ';
    else if (e.keyCode == KEY_DOT) _input += '.';
}

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------
void AppSetWiFi::do_scan() {
    if (!_wifi_on) {
        GetHAL().wifiInit();
        _wifi_on = true;
    }
    _state = ST_SCANNING;
    _aps.clear();
    GetHAL().wifiScan(_aps);  // 阻塞扫描
    _state = ST_IDLE;
    _sel = 0;
    _scroll = 0;
    mclog::tagInfo(getAppInfo().name, "scan done, {} ap", (int)_aps.size());
}

void AppSetWiFi::do_connect() {
    if (!_wifi_on) {
        GetHAL().wifiInit();
        _wifi_on = true;
    }

    // 保存配置，方便其他应用（AI）启动时自动使用
    ConfigStore::getInstance().init();
    ConfigStore::getInstance().setWifiSsid(_ssid);
    ConfigStore::getInstance().setWifiPassword(_password);

    bool ok = GetHAL().wifiConnect(_ssid, _password);
    _state = ok ? ST_OK : ST_FAIL;
    _msg_t = app_millis();
}

void AppSetWiFi::power_off() {
    GetHAL().wifiDeinit();
    _wifi_on = false;
    _state = ST_IDLE;
    _aps.clear();
}

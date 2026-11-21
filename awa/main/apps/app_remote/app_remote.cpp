#include "app_remote.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <cstring>
#include <cstdlib>
#include "../utils/ui_kit.h"

static uint32_t app_millis() { return esp_timer_get_time() / 1000; }

AppRemote::AppRemote() { setAppInfo().name = "IR Remote"; }
AppRemote::~AppRemote() {}

void AppRemote::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _input_buffer.clear();
    _status.clear();
    _status_time = 0;
    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& e) {
            if (!e.state || e.isModifier) return;
            if (e.keyCode == KEY_ENTER) { parse_and_send_ir(_input_buffer); _input_buffer.clear(); return; }
            if (e.keyCode == KEY_BACKSPACE) { if (!_input_buffer.empty()) _input_buffer.pop_back(); return; }
            if (e.keyCode >= KEY_A && e.keyCode <= KEY_Z) _input_buffer += (char)('a' + (e.keyCode - KEY_A));
            if (e.keyCode >= KEY_0 && e.keyCode <= KEY_9) _input_buffer += (char)('0' + (e.keyCode - KEY_0));
            if (e.keyCode == KEY_SPACE) _input_buffer += ' ';
            if (e.keyCode == KEY_DOT) _input_buffer += '.';
        });
}

void AppRemote::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);
    ui_title(cv, "红外遥控");

    // 输入卡片
    ui_card(cv, 6, 28, 228, 30);
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(UI_SUB);
    cv.setCursor(12, 32);
    cv.print("地址 + 指令（十六进制）");

    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(UI_TEXT);
    cv.setCursor(12, 45);
    cv.print(_input_buffer.c_str());
    if (app_millis() / 500 % 2) {
        int cx = 12 + cv.textWidth(_input_buffer.c_str());
        cv.drawFastVLine(cx + 1, 44, 13, UI_ACCENT);
    }

    // 格式提示
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(UI_SUB);
    cv.setCursor(8, 63);
    cv.print("格式  地址 指令    示例  0 0x12");

    // 发送结果
    if (!_status.empty()) {
        cv.setTextColor(app_millis() - _status_time < 1500 ? UI_OK : UI_SUB);
        cv.setCursor(8, 78);
        cv.print(_status.c_str());
    }

    ui_hint(cv, "回车发送  支持 0xA5/165  G0 返回");
    GetHAL().pushCanvas();
}

void AppRemote::onClose() {
    mclog::tagInfo(getAppInfo().name, "closed");
    if (_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
}

void AppRemote::parse_and_send_ir(const std::string& command) {
    _parse_result.clear();
    std::string token;
    for (char ch : command) {
        if (ch == ' ') {
            if (!token.empty()) _parse_result.push_back(token);
            token.clear();
        } else {
            token += ch;
        }
    }
    if (!token.empty()) _parse_result.push_back(token);

    if (_parse_result.size() < 2) {
        _status = "格式错误：需要地址和指令两个值";
        _status_time = app_millis();
        return;
    }
    uint8_t addr = (uint8_t)strtoul(_parse_result[0].c_str(), nullptr, 0);
    uint8_t cmd  = (uint8_t)strtoul(_parse_result[1].c_str(), nullptr, 0);
    GetHAL().irSend(addr, cmd);

    char buf[48];
    snprintf(buf, sizeof(buf), "已发送  地址 0x%02X  指令 0x%02X", addr, cmd);
    _status = buf;
    _status_time = app_millis();
}

void AppRemote::split_string(const std::string& str, char sep, std::vector<std::string>& result) {
    result.clear();
    std::string t;
    for (char c : str) {
        if (c == sep) {
            if (!t.empty()) result.push_back(t);
            t.clear();
        } else {
            t += c;
        }
    }
    if (!t.empty()) result.push_back(t);
}

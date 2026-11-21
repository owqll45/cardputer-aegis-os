#include "app_stringir_toolkit.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <cstring>
#include "../utils/ui_kit.h"
#include "rs_encoder.h"
#include "rs_decoder.h"

static uint32_t app_millis() { return esp_timer_get_time() / 1000; }

AppStringIRToolKit::AppStringIRToolKit() {
    setAppInfo().name = "StringIR";
}

AppStringIRToolKit::~AppStringIRToolKit() {}

void AppStringIRToolKit::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _screen_state = ScreenState::Main;
    _input_text.clear();
    _cursor_state = false;
    _cursor_update_time = 0;

    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handle_key_event(keyEvent); });
}

void AppStringIRToolKit::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) {
        if (_screen_state == ScreenState::Main) { close(); return; }
        _screen_state = ScreenState::Main;  // 子页面先回主界面
        return;
    }

    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);
    ui_title(cv, "红外字符串");

    switch (_screen_state) {
        case ScreenState::Main: render_main_interface(); ui_hint(cv, "回车发送  TAB 切换  G0 返回"); break;
        case ScreenState::Sending: process_sending(); render_sending_animation(); ui_hint(cv, "请稍候"); break;
        case ScreenState::SendComplete: render_send_complete(); ui_hint(cv, "回车/G0 返回"); break;
        case ScreenState::Error: render_error_screen(); break;
    }
    GetHAL().pushCanvas();
}

void AppStringIRToolKit::onClose() {
    mclog::tagInfo(getAppInfo().name, "closed");
    if (_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
}

void AppStringIRToolKit::render_main_interface() {
    render_text_input_line();
    render_style_line();
    render_help_line();
}

void AppStringIRToolKit::render_text_input_line() {
    auto& c = GetHAL().canvas();
    bool focus = (_focus_line == FocusLine::TextInput);
    ui_card(c, 6, 28, 228, 30, focus);

    c.setFont(&fonts::efontCN_10);
    c.setTextColor(UI_SUB);
    c.setCursor(12, 32);
    c.print("要发送的文本");

    c.setFont(&fonts::efontCN_12);
    c.setTextColor(UI_TEXT);
    c.setCursor(12, 45);
    c.print(_input_text.c_str());
    if (focus) {
        update_cursor();
        if (_cursor_state) {
            int cx = 12 + c.textWidth(_input_text.c_str());
            c.drawFastVLine(cx + 1, 44, 13, UI_ACCENT);
        }
    }
}

void AppStringIRToolKit::render_style_line() {
    auto& c = GetHAL().canvas();
    bool focus = (_focus_line == FocusLine::Style);
    ui_card(c, 6, 60, 228, 24, focus);

    char buf[48];
    snprintf(buf, sizeof(buf), "样式  [%s]", _style_options[_style_index].c_str());
    ui_kv(c, 12, 65, 150, "显示效果", buf, focus ? UI_ACCENT : UI_TEXT);

    c.setFont(&fonts::efontCN_10);
    c.setTextColor(UI_SUB);
    c.setTextDatum(top_right);
    c.drawString("A / D 切换", 228, 65);
    c.setTextDatum(top_left);
}

void AppStringIRToolKit::render_help_line() {
    auto& c = GetHAL().canvas();
    c.setFont(&fonts::efontCN_10);
    c.setTextColor(UI_SUB);
    c.setCursor(8, 88);
    c.printf("已输入 %d / %d 字符", (int)_input_text.length(), (int)INPUT_BUFFER_SIZE);
}

void AppStringIRToolKit::render_sending_animation() {
    auto& c = GetHAL().canvas();
    _sending_dots = (_sending_dots + 1) % 4;
    std::string dots(_sending_dots, '.');
    c.setFont(&fonts::efontCN_14);
    c.setTextColor(UI_WARN);
    c.setTextDatum(top_center);
    c.drawString(("发送中" + dots).c_str(), 120, 40);
    c.setTextDatum(top_left);
}

void AppStringIRToolKit::render_send_complete() {
    auto& c = GetHAL().canvas();
    c.setFont(&fonts::efontCN_14);
    c.setTextColor(UI_OK);
    c.setTextDatum(top_center);
    c.drawString("已发送", 120, 38);
    c.setFont(&fonts::efontCN_12);
    c.setTextColor(UI_SUB);
    c.drawString("按 回车 返回", 120, 86);
    c.setTextDatum(top_left);
}

void AppStringIRToolKit::render_error_screen() {
    ui_msg(GetHAL().canvas(), "发送失败", _error_message.c_str(), nullptr, UI_BAD);
}

void AppStringIRToolKit::handle_key_event(const Keyboard::KeyEvent_t& keyEvent) {
    if (!keyEvent.state || keyEvent.isModifier) return;
    if (_screen_state == ScreenState::SendComplete || _screen_state == ScreenState::Error) {
        if (keyEvent.keyCode == KEY_ENTER) _screen_state = ScreenState::Main;
        return;
    }
    if (keyEvent.keyCode == KEY_TAB) { _focus_line = (_focus_line == FocusLine::TextInput) ? FocusLine::Style : FocusLine::TextInput; return; }
    if (keyEvent.keyCode == KEY_ENTER) { start_sending(); return; }
    if (_focus_line == FocusLine::TextInput) {
        if (keyEvent.keyCode == KEY_BACKSPACE) { if (!_input_text.empty()) _input_text.pop_back(); }
        else if (keyEvent.keyCode >= KEY_A && keyEvent.keyCode <= KEY_Z) { if (_input_text.length() < INPUT_BUFFER_SIZE) _input_text += (char)('a' + (keyEvent.keyCode - KEY_A)); }
        else if (keyEvent.keyCode == KEY_SPACE) { if (_input_text.length() < INPUT_BUFFER_SIZE) _input_text += ' '; }
        else if (keyEvent.keyCode >= KEY_1 && keyEvent.keyCode <= KEY_9) { if (_input_text.length() < INPUT_BUFFER_SIZE) _input_text += (char)('1' + (keyEvent.keyCode - KEY_1)); }
        else if (keyEvent.keyCode == KEY_0) { if (_input_text.length() < INPUT_BUFFER_SIZE) _input_text += '0'; }
    } else if (_focus_line == FocusLine::Style) {
        if (keyEvent.keyCode == KEY_LEFT || keyEvent.keyCode == KEY_A) _style_index = (_style_index - 1 + _style_options.size()) % _style_options.size();
        if (keyEvent.keyCode == KEY_RIGHT || keyEvent.keyCode == KEY_D) _style_index = (_style_index + 1) % _style_options.size();
    }
}

void AppStringIRToolKit::update_cursor() {
    if (app_millis() - _cursor_update_time > CURSOR_BLINK_PERIOD) { _cursor_state = !_cursor_state; _cursor_update_time = app_millis(); }
}

void AppStringIRToolKit::start_sending() {
    if (_input_text.empty()) { show_error_message("请先输入要发送的文本"); return; }
    _screen_state = ScreenState::Sending; _sending_start_time = app_millis(); send_ir_data();
}

void AppStringIRToolKit::process_sending() {
    if (app_millis() - _sending_start_time > 2000) _screen_state = ScreenState::SendComplete;
}

void AppStringIRToolKit::send_ir_data() {
    uint32_t code = 0;
    for (int i = 0; i < 4 && i < (int)_input_text.length(); i++) code = (code << 8) | (uint8_t)_input_text[i];
    GetHAL().irSend(0x00, (uint8_t)(code & 0xFF));
}

void AppStringIRToolKit::show_error_message(const std::string& message) {
    _error_message = message; _error_display_start_time = app_millis(); _screen_state = ScreenState::Error;
}

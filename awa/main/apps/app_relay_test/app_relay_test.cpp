#include "app_relay_test.h"
#include "hal/hal.h"
#include "../utils/ui_kit.h"
#include <driver/gpio.h>
#include <esp_log.h>

AppRelayTest::AppRelayTest() {
    setAppInfo().name = "RelayTester";
}

AppRelayTest::~AppRelayTest() {}

void AppRelayTest::onOpen() {
    // 不再修改 M5.Display 的 rotation / colorDepth，只接管两个 GPIO
    esp_log_level_set("*", ESP_LOG_NONE);

    gpio_reset_pin(GPIO_NUM_1);
    gpio_reset_pin(GPIO_NUM_2);

    _relay_state = false;
    _auto_mode = false;
    _history.clear();
    for (int i = 0; i < 60; i++) _history.push_back(false);

    apply_pin_state();
    draw_ui();
}

void AppRelayTest::onClose() {
    esp_log_level_set("*", ESP_LOG_INFO);
    // 恢复引脚为默认状态，避免影响其他应用
    gpio_reset_pin((gpio_num_t)_pins[0]);
    gpio_reset_pin((gpio_num_t)_pins[1]);
}

void AppRelayTest::apply_pin_state() {
    int pin = _pins[_selected_pin_idx];

    int level;
    if (_relay_state) {
        level = _logic_active_high ? 1 : 0;
    } else {
        level = _logic_active_high ? 0 : 1;
    }

    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_3);

    if (_drive_mode == 0) {
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)pin, level);
    } else if (_drive_mode == 1) {
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT_OD);
        gpio_set_level((gpio_num_t)pin, level);
    } else {
        if (_relay_state) {
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)pin, _logic_active_high ? 1 : 0);
        } else {
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)pin, _logic_active_high ? 0 : 1);
            GetHAL().delay(100);
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
            gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
        }
    }
}

void AppRelayTest::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    bool updated = false;
    auto& keyEvent = GetHAL().keyboard.getLatestKeyEvent();
    if (keyEvent.state) {
        if (keyEvent.keyCode == KEY_Q) {
            _selected_pin_idx = (_selected_pin_idx + 1) % 2;
            apply_pin_state();
            updated = true;
        } else if (keyEvent.keyCode == KEY_W) {
            _drive_mode = (_drive_mode + 1) % 3;
            apply_pin_state();
            updated = true;
        } else if (keyEvent.keyCode == KEY_S) {
            _drive_mode = (_drive_mode + 2) % 3;
            apply_pin_state();
            updated = true;
        } else if (keyEvent.keyCode == KEY_A || keyEvent.keyCode == KEY_D) {
            _logic_active_high = !_logic_active_high;
            apply_pin_state();
            updated = true;
        } else if (keyEvent.keyCode == KEY_SPACE || keyEvent.keyCode == KEY_ENTER) {
            if (GetHAL().millis() - _last_toggle > 300) {
                if (_auto_mode) {
                    _auto_mode = false;
                } else {
                    toggle_relay();
                }
                _last_toggle = GetHAL().millis();
                updated = true;
            }
        }
        GetHAL().keyboard.clearKeyEvent();
    }

    if (_auto_mode && (GetHAL().millis() - _last_toggle > 1000)) {
        toggle_relay();
        _last_toggle = GetHAL().millis();
        updated = true;
    }

    if (updated) draw_ui();
}

void AppRelayTest::draw_ui() {
    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);
    ui_title(cv, "继电器测试");

    // 左：状态卡片
    ui_card(cv, 6, 28, 116, 58, _relay_state);
    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    cv.setCursor(12, 31);
    cv.print("输出状态");

    cv.setFont(&fonts::efontCN_14);
    cv.setTextColor(_relay_state ? UI_OK : UI_BAD);
    cv.setTextDatum(top_center);
    cv.drawString(_relay_state ? "ON" : "OFF", 64, 46);
    cv.setTextDatum(top_left);

    char buf[40];
    const char* mode_str = (_drive_mode == 0) ? "推挽" : (_drive_mode == 1) ? "开漏" : "悬空";
    snprintf(buf, sizeof(buf), "%s  ·  %s", _pin_names[_selected_pin_idx],
             _logic_active_high ? "高有效" : "低有效");
    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    cv.setTextDatum(top_center);
    cv.drawString(buf, 64, 72);
    cv.setTextDatum(top_left);

    // 右：设置列表
    int x = 128, w = 106;
    ui_card(cv, x, 28, w, 16);
    ui_kv(cv, x + 6, 31, w - 12, "引脚", _pin_names[_selected_pin_idx], UI_ACCENT);

    ui_card(cv, x, 46, w, 16);
    ui_kv(cv, x + 6, 49, w - 12, "逻辑", _logic_active_high ? "高有效" : "低有效", UI_ACCENT);

    ui_card(cv, x, 64, w, 22);
    ui_kv(cv, x + 6, 67, w - 12, "驱动模式", mode_str, UI_ACCENT);

    // 底部键位
    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    cv.setCursor(6, 88);
    cv.print("Q:换引脚  A/D:逻辑  W/S:驱动  空格:开关");

    ui_hint(cv, "G0 返回");
    GetHAL().pushCanvas();
}

void AppRelayTest::toggle_relay() {
    _relay_state = !_relay_state;

    _history.erase(_history.begin());
    _history.push_back(_relay_state);

    apply_pin_state();
}

void AppRelayTest::update_auto_mode() {}

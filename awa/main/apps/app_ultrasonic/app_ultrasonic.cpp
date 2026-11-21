/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_ultrasonic.h"
#include <mooncake_log.h>
#include <hal.h>
#include "../utils/ui_kit.h"

using namespace mooncake;

AppUltrasonic::AppUltrasonic() {
    setAppInfo().name = "Distance";
    setAppInfo().icon = nullptr;
    _ultrasonic = nullptr;
    _distance = 0.0f;
    _last_read_time = 0;
    _last_update_time = 0;
    _history_index = 0;
    _handle_key_event_slot_id = -1;

    for (int i = 0; i < 12; i++) {
        _distance_history[i] = 0.0f;
    }
}

AppUltrasonic::~AppUltrasonic() {
    if (_ultrasonic) {
        delete _ultrasonic;
    }
}

void AppUltrasonic::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Application opened");

    // 超声波传感器（GPIO 3 Trig / GPIO 4 Echo）
    _ultrasonic = new Ultrasonic(3, 4);
    _sensor_connected = false;
    _last_read_time = 0;
}

void AppUltrasonic::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    // 每 400ms 读一次，未连接也持续重试（接上后自动恢复）
    if (GetHAL().millis() - _last_read_time >= 400) {
        readSensorData();
        _last_read_time = GetHAL().millis();
    }

    drawDisplay();
}

void AppUltrasonic::onClose() {
    mclog::tagInfo(getAppInfo().name, "Application closed");

    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    }
    if (_ultrasonic) {
        delete _ultrasonic;
        _ultrasonic = nullptr;
    }
}

void AppUltrasonic::readSensorData() {
    if (!_ultrasonic) { _sensor_connected = false; return; }
    float dist = _ultrasonic->readDistance();
    if (dist > 1.0f && dist < 500.0f) {
        _distance = dist;
        _sensor_connected = true;
        updateHistoryData();
    } else {
        _sensor_connected = false;
    }
}

void AppUltrasonic::drawDisplay() {
    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);

    if (!_sensor_connected) {
        ui_msg(cv, "未检测到超声波传感器", "请接线：Trig→G3   Echo→G4", "电源：VCC→5V   GND→G");
        GetHAL().pushCanvas();
        return;
    }

    ui_title(cv, "超声波测距");

    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", _distance);
    ui_big_value(cv, 74, 30, buf, "cm", UI_ACCENT);

    // 右侧趋势图
    drawTrendGraph();

    // 距离等级提示
    const char* level = "较远";
    uint16_t color = UI_OK;
    if (_distance < 10) { level = "非常近"; color = UI_BAD; }
    else if (_distance < 30) { level = "较近"; color = UI_WARN; }

    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(color);
    cv.setTextDatum(top_center);
    cv.drawString(level, 52, 80);
    cv.setTextDatum(top_left);

    ui_hint(cv, "量程 2~400cm  每 0.4s 刷新  G0 返回");
    GetHAL().pushCanvas();
}

void AppUltrasonic::updateHistoryData() {
    _distance_history[_history_index] = _distance;
    _history_index = (_history_index + 1) % 12;
}

void AppUltrasonic::drawTrendGraph() {
    auto& cv = GetHAL().canvas();
    int gx = 132, gy = 28, gw = 100, gh = 62;

    cv.fillRoundRect(gx, gy, gw, gh, 5, UI_CARD);
    cv.drawRoundRect(gx, gy, gw, gh, 5, UI_LINE);

    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(UI_SUB);
    cv.setCursor(gx + 5, gy + 2);
    cv.print("距离趋势");

    int step = (gw - 8) / 12;
    int x0 = gx + 4;
    int y_base = gy + gh - 4;

    for (int i = 0; i < 12; i++) {
        int idx = (_history_index + i) % 12;
        float v = _distance_history[idx];
        int h = (int)(v * (gh - 22) / 200.0f);
        if (h > gh - 22) h = gh - 22;
        if (v > 0 && h < 2) h = 2;
        if (h > 0) {
            cv.fillRect(x0 + i * step, y_base - h, step - 2, h, UI_OK);
        }
    }
}

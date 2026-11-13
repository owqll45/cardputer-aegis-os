/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_dht11.h"
#include <mooncake_log.h>
#include <hal.h>
#include <math.h>
#include "../utils/ui_kit.h"

using namespace mooncake;

AppDHT11::AppDHT11() {
    setAppInfo().name = "Temp/Humi+";
    setAppInfo().icon = nullptr;
    _dht11 = nullptr;
    _target_temp = 0.0f;
    _target_humi = 0.0f;
    _display_temp = 0.0f;
    _display_humi = 0.0f;
    _last_read_time = 0;
    _last_ui_update = 0;
    _history_index = 0;

    for (int i = 0; i < HISTORY_LEN; i++) {
        _humi_history[i] = 0.0f;
    }
}









AppDHT11::~AppDHT11() {
    if (_dht11) delete _dht11;
}

void AppDHT11::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Application opened");

    // GPIO 2 (Cardputer Grove G2)
    _dht11 = new DHT11(2);
    _sensor_connected = false;
    _last_read_time = 0;
    _last_ui_update = 0;

    readSensorData();
    _display_temp = _target_temp;
    _display_humi = _target_humi;
}

void AppDHT11::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    if (GetHAL().millis() - _last_read_time >= 1500) {
        readSensorData();
        _last_read_time = GetHAL().millis();
    }

    if (GetHAL().millis() - _last_ui_update >= 60) {
        _display_temp += (_target_temp - _display_temp) * 0.2f;
        _display_humi += (_target_humi - _display_humi) * 0.2f;
        drawDisplay();
        _last_ui_update = GetHAL().millis();
    }
}

void AppDHT11::onClose() {
    if (_dht11) { delete _dht11; _dht11 = nullptr; }
}

void AppDHT11::readSensorData() {
    if (!_dht11) { _sensor_connected = false; return; }
    float t, h;
    bool success = _dht11->readTempAndHumi(&t, &h);
    if (!success) {
        _sensor_connected = false;
        return;
    }

    _sensor_connected = true;
    _target_temp = t;
    _target_humi = h;

    if (_max_temp == -999.0f || t > _max_temp) _max_temp = t;
    if (_min_temp == 999.0f || t < _min_temp) _min_temp = t;

    _humi_history[_history_index] = h;
    _history_index = (_history_index + 1) % HISTORY_LEN;
}

void AppDHT11::drawDisplay() {
    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);

    if (!_sensor_connected) {
        ui_msg(cv, "未检测到 DHT11 传感器", "请接线：DATA→G2   电源 VCC→5V", "GND→G   接好后自动恢复读取");
        GetHAL().pushCanvas();
        return;
    }

    ui_title(cv, "温湿度 DHT11");

    char buf[64];

    // 温度卡片
    ui_card(cv, 4, 26, 116, 48);
    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    cv.setCursor(10, 29);
    cv.print("温度");
    cv.setFont(&fonts::efontCN_14);
    cv.setTextColor(UI_WARN);
    snprintf(buf, sizeof(buf), "%.1f", _display_temp);
    cv.setCursor(10, 44);
    cv.print(buf);
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(UI_SUB);
    cv.setCursor(84, 46);
    cv.print("°C");

    // 湿度卡片
    ui_card(cv, 122, 26, 112, 48);
    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    cv.setCursor(128, 29);
    cv.print("湿度");
    cv.setFont(&fonts::efontCN_14);
    cv.setTextColor(UI_ACCENT);
    snprintf(buf, sizeof(buf), "%.0f", _display_humi);
    cv.setCursor(128, 44);
    cv.print(buf);
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(UI_SUB);
    cv.setCursor(184, 46);
    cv.print("%");

    // 极值 + 趋势
    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    snprintf(buf, sizeof(buf), "最高 %.1f°  最低 %.1f°", _max_temp, _min_temp);
    cv.setCursor(8, 80);
    cv.print(buf);

    drawTrend();

    ui_hint(cv, "每 1.5s 读取一次  G0 返回");
    GetHAL().pushCanvas();
}

void AppDHT11::drawTrend() {
    auto& cv = GetHAL().canvas();
    int gx = 148, gy = 76, gw = 86, gh = 18;

    cv.fillRoundRect(gx, gy, gw, gh, 4, UI_CARD);
    cv.drawRoundRect(gx, gy, gw, gh, 4, UI_LINE);

    for (int i = 0; i < HISTORY_LEN - 1; i++) {
        int i1 = (_history_index + i) % HISTORY_LEN;
        int i2 = (_history_index + i + 1) % HISTORY_LEN;
        int x1 = gx + 2 + i * (gw - 4) / (HISTORY_LEN - 1);
        int x2 = gx + 2 + (i + 1) * (gw - 4) / (HISTORY_LEN - 1);
        int y1 = gy + gh - 2 - (int)(_humi_history[i1] * (gh - 4) / 100.0f);
        int y2 = gy + gh - 2 - (int)(_humi_history[i2] * (gh - 4) / 100.0f);
        cv.drawLine(x1, y1, x2, y2, UI_ACCENT);
    }
}

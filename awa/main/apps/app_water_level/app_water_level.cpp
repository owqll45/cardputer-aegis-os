/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_water_level.h"
#include <mooncake_log.h>
#include <hal.h>
#include <math.h>
#include "../utils/ui_kit.h"

using namespace mooncake;

AppWaterLevel::AppWaterLevel() {
    setAppInfo().name = "Water Level";
    setAppInfo().icon = nullptr;
}

AppWaterLevel::~AppWaterLevel() {
    if (_sensor) delete _sensor;
}

void AppWaterLevel::onOpen() {
    // 默认 G2 (Cardputer Grove)
    _sensor = new WaterLevelSensor(2);
    _sensor->init();

    for (int i = 0; i < HIST_LEN; i++) _history[i] = 0;
    _hist_idx = 0;
    _current_level = 0;
    _target_level = 0;
    _raw_value = 0;
    _sensor_ok = false;
    _last_read = 0;
    _last_draw = 0;
}

void AppWaterLevel::onClose() {
    if (_handle_key >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key);
    if (_sensor) { delete _sensor; _sensor = nullptr; }
}

void AppWaterLevel::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    if (GetHAL().millis() - _last_read > 100) {
        readSensor();
        _last_read = GetHAL().millis();
    }

    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);

    if (!_sensor_ok) {
        ui_msg(cv, "未检测到水位传感器", "请接线：Signal→G2   电源 VCC→5V", "GND→G   接好后自动恢复监测");
        GetHAL().pushCanvas();
        return;
    }

    ui_title(cv, "水位监测");

    if (GetHAL().millis() - _last_draw > 40) {
        _current_level += (_target_level - _current_level) * 0.12f;
        _wave_phase += 0.16f;
        if (_wave_phase > 6.28f) _wave_phase -= 6.28f;
        _last_draw = GetHAL().millis();
    }

    drawTank();

    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", _current_level);
    ui_big_value(cv, 54, 24, buf, "%", _current_level > 85 ? UI_BAD : UI_ACCENT);

    snprintf(buf, sizeof(buf), "原始值 %d", _raw_value);
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(UI_SUB);
    cv.setCursor(102, 27);
    cv.print(buf);

    drawHistory();

    ui_hint(cv, "实时水位 10Hz  G0 返回");
    GetHAL().pushCanvas();
}

void AppWaterLevel::readSensor() {
    if (!_sensor) { _sensor_ok = false; return; }
    _raw_value = _sensor->readRaw();
    float pct = _sensor->readPercent();
    if (_raw_value < 60) {
        // 未接传感器时读数接近 0，视为未连接
        _sensor_ok = false;
        return;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    _sensor_ok = true;
    _target_level = pct;
    _history[_hist_idx] = pct;
    _hist_idx = (_hist_idx + 1) % HIST_LEN;
}

void AppWaterLevel::drawTank() {
    auto& cv = GetHAL().canvas();
    int x = 8, y = 76, w = 224, h = 18;

    cv.fillRoundRect(x, y, w, h, 4, UI_CARD);
    cv.drawRoundRect(x, y, w, h, 4, UI_LINE);

    int water_w = (int)((w - 4) * _current_level / 100.0f);
    if (water_w > 0) {
        cv.fillRect(x + 2, y + 2, water_w, h - 4, _current_level > 85 ? UI_BAD : 0x04BF);
    }

    // 水面波纹
    if (water_w > 4) {
        for (int i = 0; i < water_w - 4; i += 6) {
            int wy = y + 3 + (int)(sinf(_wave_phase + i * 0.25f) * 1.5f);
            cv.drawFastHLine(x + 2 + i, wy, 4, TFT_WHITE);
        }
    }
}

void AppWaterLevel::drawHistory() {
    auto& cv = GetHAL().canvas();
    int gx = 102, gy = 40, gw = 130, gh = 28;

    cv.fillRoundRect(gx, gy, gw, gh, 4, UI_CARD);
    cv.drawRoundRect(gx, gy, gw, gh, 4, UI_LINE);

    for (int i = 0; i < HIST_LEN - 1; i++) {
        int i1 = (_hist_idx + i) % HIST_LEN;
        int i2 = (_hist_idx + i + 1) % HIST_LEN;
        int x1 = gx + 3 + i * (gw - 6) / (HIST_LEN - 1);
        int x2 = gx + 3 + (i + 1) * (gw - 6) / (HIST_LEN - 1);
        int y1 = gy + gh - 3 - (int)(_history[i1] * (gh - 6) / 100.0f);
        int y2 = gy + gh - 3 - (int)(_history[i2] * (gh - 6) / 100.0f);
        cv.drawLine(x1, y1, x2, y2, UI_OK);
    }
}

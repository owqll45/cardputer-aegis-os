/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include "apps/app_sensors/water_level.h"

class AppWaterLevel : public mooncake::AppAbility {
public:
    AppWaterLevel();
    ~AppWaterLevel();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    WaterLevelSensor* _sensor = nullptr;

    float _current_level = 0.0f;  // 平滑后的 0-100
    float _target_level = 0.0f;
    int _raw_value = 0;
    uint32_t _last_read = 0;
    uint32_t _last_draw = 0;
    bool _sensor_ok = false;

    float _wave_phase = 0.0f;

    static const int HIST_LEN = 40;
    float _history[HIST_LEN];
    int _hist_idx = 0;

    int _handle_key = -1;

    void readSensor();
    void drawTank();
    void drawHistory();
};

/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include "apps/app_sensors/dht11.h"

/**
 * @brief DHT11 温湿度应用（统一 UI 版）
 */
class AppDHT11 : public mooncake::AppAbility {
public:
    AppDHT11();
    ~AppDHT11();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    DHT11* _dht11 = nullptr;

    float _target_temp = 0.0f;
    float _target_humi = 0.0f;
    float _display_temp = 0.0f;
    float _display_humi = 0.0f;
    float _max_temp = -999.0f;
    float _min_temp = 999.0f;

    bool _sensor_connected = false;

    uint32_t _last_read_time = 0;
    uint32_t _last_ui_update = 0;

    static const int HISTORY_LEN = 40;
    float _humi_history[HISTORY_LEN];
    int _history_index = 0;

    void readSensorData();
    void drawDisplay();
    void drawTrend();
};

/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>
#include <esp_adc/adc_oneshot.h>

class WaterLevelSensor {
public:
    // Default G2 (GPIO 2)
    WaterLevelSensor(int pin = 2);
    ~WaterLevelSensor();
    
    bool init();
    int readRaw();
    float readPercent(); // 0.0 - 100.0

private:
    int _pin;
    adc_oneshot_unit_handle_t _adc_handle = nullptr;
    adc_channel_t _adc_channel;
    bool _initialized = false;
    bool _use_mock = false;
    bool _use_gpio_fallback = false;
};

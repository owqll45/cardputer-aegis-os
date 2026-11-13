/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include "apps/app_sensors/ultrasonic.h"

/**
 * @brief 超声波距离传感器应用
 */
class AppUltrasonic : public mooncake::AppAbility {
public:
    AppUltrasonic();
    ~AppUltrasonic();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // 超声波传感器实例
    Ultrasonic* _ultrasonic;
    
    // 数据
    float _distance;
    uint32_t _last_read_time;
    uint32_t _last_update_time;
    
    // 历史数据用于绘制趋势图
    float _distance_history[12]; // 保存最近12个距离值
    int _history_index;
    
    // 事件处理
    int _handle_key_event_slot_id;
    
    // 辅助方法
    void readSensorData();
    void drawDisplay();
    void updateHistoryData();
    void drawTrendGraph();
    
    bool _sensor_connected = false;
};

/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <cstdint>

/**
 * @brief 崩溃测试应用
 * 用于测试Android-like崩溃处理系统
 */
class AppCrashTest : public mooncake::AppAbility {
public:
    AppCrashTest();
    ~AppCrashTest();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    uint32_t _time_count;
    int _crash_type;
    int _handle_key_event_slot_id = -1;
    
    void trigger_crash();
    void draw_ui();
};

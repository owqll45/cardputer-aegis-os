/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <cstdint>
#include <hal/hal.h>
#include <ctime>

class AppClock : public mooncake::AppAbility {
public:
    AppClock();
    ~AppClock();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    int64_t _last_update = 0;

    void render_interface();
    void update_time_display();
    void show_network_time();
    void show_system_time();
};

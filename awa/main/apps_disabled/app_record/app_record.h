/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <cstdint>
#include <hal/hal.h>

class AppRecord : public mooncake::AppAbility {
public:
    AppRecord();
    ~AppRecord();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
private:
    static constexpr size_t RECORD_LENGTH = 200;
    static constexpr size_t RECORD_SIZE = 16000;
    uint32_t _time_count = 0;
    int16_t* _rec_data = nullptr;
    size_t _rec_idx = 0;
    bool _is_recording = true;
    void render_page_recording();
    void render_waveform();
    void handle_input();
};

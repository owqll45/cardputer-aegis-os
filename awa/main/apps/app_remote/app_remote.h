/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <cstdint>
#include <hal/hal.h>
#include <string>
#include <vector>

class AppRemote : public mooncake::AppAbility {
public:
    AppRemote();
    ~AppRemote();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
private:
    std::string _input_buffer;
    std::string _status;
    uint32_t _status_time = 0;
    std::vector<std::string> _parse_result;
    int _key_event_slot_id = -1;
    void parse_and_send_ir(const std::string& command);
    void split_string(const std::string& str, char separator, std::vector<std::string>& result);
};

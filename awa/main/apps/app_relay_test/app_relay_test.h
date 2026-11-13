#pragma once
#include <mooncake.h>
#include <vector>
#include <M5Unified.h>

class AppRelayTest : public mooncake::AppAbility {
public:
    AppRelayTest();
    ~AppRelayTest() override;
    
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void draw_ui();
    void toggle_relay();
    void update_auto_mode();
    void apply_pin_state();
    
    bool _relay_state = false;
    bool _auto_mode = false;
    bool _logic_active_high = true; // Default Active High
    int _drive_mode = 1; // 0: Push-Pull, 1: Open-Drain (Default), 2: Input-Floating
    int _selected_pin_idx = 0; // 0: G1, 1: G2
    
    const int _pins[2] = {GPIO_NUM_1, GPIO_NUM_2};
    const char* _pin_names[2] = {"G1", "G2"};
    
    std::vector<bool> _history;
    uint32_t _last_toggle = 0;
};

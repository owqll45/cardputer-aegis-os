/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <string>

namespace crash_handler {

// 崩溃信息结构体
typedef struct {
    uint32_t crash_time;       // 崩溃时间戳
    uint32_t exception_cause;  // 异常原因
    uint32_t exception_pc;     // 异常发生的程序计数器
    uint32_t exception_ps;     // 异常发生时的程序状态字
    uint32_t stack_pointer;    // 异常发生时的堆栈指针
    char task_name[16];        // 崩溃的任务名称
    char error_msg[128];       // 错误信息
    uint32_t backtrace[8];     // 调用栈回溯
} crash_info_t;

// 初始化崩溃处理模块
void init();

// 保存崩溃信息
void save_crash_info(const crash_info_t* info);

// 读取崩溃信息
bool read_crash_info(crash_info_t* info);

// 清除崩溃信息
void clear_crash_info();

// 显示崩溃界面
// is_runtime: if true, adds a "Dismiss" option to return to caller
void show_crash_screen(const crash_info_t* info, bool is_runtime = false);

// 重启系统
void reboot_system();

// 返回桌面
void return_to_desktop();

// Set currently running app name (Stored in RTC memory)
void set_running_app(const char* name);

// Mark the next restart as intentional (no crash screen will appear)
void mark_intentional_restart();

// Check if system rebooted from a crash, and show screen if so
void check_for_crash();

// Helper to check if it was an app crash
bool is_app_crash();

// 弹窗类型
enum PopupType {
    POPUP_TYPE_ERROR,    // 红色 (致命错误)
    POPUP_TYPE_WARNING,  // 黄色 (警告/内存/电源)
    POPUP_TYPE_INFO      // 蓝色 (提示)
};

// 显示标准系统弹窗
bool show_system_popup(const char* title, const char* message, PopupType type = POPUP_TYPE_WARNING,
                       bool is_long_press = true);

// 显示生命安全报警 (Sentinel v4.0)
void show_life_alert(const char* title, const char* message, uint16_t color);

// 显示灾难性倒计时警报 (自动 10 秒倒计时)
void show_disaster_alert(const char* reason);

// 显示电源警告弹窗 (针对 Cardputer 电池开关)
void show_power_warning();

}  // namespace crash_handler

/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "crash_handler.h"
#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/hal.h>
#include <mooncake.h>
#include <string.h>
#include <driver/gpio.h>  // Added for GPIO operations

namespace crash_handler {

static const char* TAG           = "CrashHandler";
static const char* NVS_NAMESPACE = "crash_info";

// RTC Memory to survive reboot
static RTC_NOINIT_ATTR char _running_app_name[32];
static RTC_NOINIT_ATTR uint32_t _magic_word;
static RTC_NOINIT_ATTR uint32_t _intentional_restart_flag;  // 标记有意重启，跳过経溃界面
#define CRASH_MAGIC               0xDEADBEEF
#define INTENTIONAL_RESTART_MAGIC 0xBAADF00D  // 有意重启标识

void mark_intentional_restart()
{
    _intentional_restart_flag = INTENTIONAL_RESTART_MAGIC;
    _magic_word               = 0;  // 清除活跃的 app 名字，避免被当成経溃
}

void set_running_app(const char* name)
{
    if (name) {
        strncpy(_running_app_name, name, sizeof(_running_app_name) - 1);
        _running_app_name[sizeof(_running_app_name) - 1] = 0;
        _magic_word                                      = CRASH_MAGIC;
    }
}

void check_for_crash()
{
    esp_reset_reason_t reason = esp_reset_reason();

    // 如果是有意重启（如 LegacyApp 退出），直接跳过経溃界面
    if (_intentional_restart_flag == INTENTIONAL_RESTART_MAGIC) {
        _intentional_restart_flag = 0;
        _magic_word               = 0;
        ESP_LOGI(TAG, "Intentional restart detected, skipping crash screen.");
        return;
    }

    // Check if it was a panic/crash OR software reset (e.g. triggered by user in crash test app)
    if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
        reason == ESP_RST_SW) {
        // Only show crash screen if we have a magic word set (meaning it was an "expected" run that crashed)
        // OR if it's a panic.
        if (_magic_word == CRASH_MAGIC || reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
            reason == ESP_RST_TASK_WDT) {
            ESP_LOGW(TAG, "System reset due to crash! Reason: %d", reason);

            crash_info_t info = {0};

            // Recover app name
            if (_magic_word == CRASH_MAGIC) {
                strncpy(info.task_name, _running_app_name, sizeof(info.task_name) - 1);
                // Clear magic word so we don't show crash screen on next normal reboot
                _magic_word = 0;
            } else {
                strcpy(info.task_name, "System/Unknown");
            }

            // Set error message based on reason
            switch (reason) {
                case ESP_RST_PANIC:
                    strcpy(info.error_msg, "System Panic (Exception)");
                    break;
                case ESP_RST_INT_WDT:
                    strcpy(info.error_msg, "Interrupt Watchdog Timeout");
                    break;
                case ESP_RST_TASK_WDT:
                    strcpy(info.error_msg, "Task Watchdog Timeout");
                    break;
                case ESP_RST_WDT:
                    strcpy(info.error_msg, "Other Watchdog Reset");
                    break;
                case ESP_RST_SW:
                    strcpy(info.error_msg, "Software Reset (Possible Crash)");
                    break;
                default:
                    strcpy(info.error_msg, "Unknown Crash");
                    break;
            }

            // Show the screen (Blocking)
            show_crash_screen(&info);
        }
    }
}

// Check if crash was from an app
bool is_app_crash()
{
    return (_magic_word == CRASH_MAGIC && strncmp(_running_app_name, "Launcher", 8) != 0);
}

// 初始化崩溃处理模块
void init()
{
    // NVS已经在HAL中初始化，这里不需要再次初始化
    ESP_LOGI(TAG, "Crash handler initialized");
}

// 保存崩溃信息
void save_crash_info(const crash_info_t* info)
{
    if (!info) {
        ESP_LOGE(TAG, "Invalid crash info");
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    // 保存崩溃信息
    err = nvs_set_blob(handle, "crash_data", info, sizeof(crash_info_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save crash info: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    // 提交更改
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Crash info saved successfully");
}

// 读取崩溃信息
bool read_crash_info(crash_info_t* info)
{
    if (!info) {
        ESP_LOGE(TAG, "Invalid crash info pointer");
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    // 读取崩溃信息
    size_t size = sizeof(crash_info_t);
    err         = nvs_get_blob(handle, "crash_data", info, &size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read crash info: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Crash info read successfully");
    return true;
}

// 清除崩溃信息
void clear_crash_info()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    // 删除崩溃信息
    err = nvs_erase_key(handle, "crash_data");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase crash info: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    // 提交更改
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Crash info cleared successfully");
}

// Show crash screen
void show_crash_screen(const crash_info_t* info, bool is_runtime)
{
    // Redundant M5.begin() removed to save stack and avoid resource conflict

    // Ensure display is on and bright
    M5.Display.setBrightness(255);

    // Use M5.Display directly for full screen coverage (240x135)
    M5.Display.fillScreen(0x222222);  // Background

    // --- 1. Title Bar (Top) ---
    M5.Display.fillRect(0, 0, 240, 24, TFT_RED);
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::efontCN_14);

    if (is_runtime) {
        M5.Display.drawString(" 运行时错误 (Runtime Error)", 5, 12);
    } else {
        M5.Display.drawString(" 应用无响应 (App Crash)", 5, 12);
    }

    M5.Display.setTextDatum(middle_right);
    M5.Display.setFont(&fonts::efontCN_12);
    if (is_runtime) {
        M5.Display.drawString("系统未暂停 (Active) ", 235, 12);
    } else {
        M5.Display.drawString("系统已暂停 (Halted) ", 235, 12);
    }

    // --- 2. Error Content ---
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(TFT_WHITE, 0x222222);

    // App Name (Icon style)
    M5.Display.fillRoundRect(10, 35, 40, 40, 8, 0x444444);  // Icon placeholder
    M5.Display.setTextColor(TFT_RED, 0x444444);
    M5.Display.setFont(&fonts::efontCN_24);
    M5.Display.drawString("!", 22, 42);  // Big Exclamation

    M5.Display.setTextColor(TFT_YELLOW, 0x222222);
    M5.Display.setFont(&fonts::efontCN_16);
    M5.Display.setCursor(60, 32);
    M5.Display.printf("%s", info ? info->task_name : "Unknown App");

    // Analyze Error for Detail
    const char* reason_cn = "未知错误";
    const char* reason_en = "Unknown Error";
    const char* advice_cn = "请尝试重启设备或长按 ESC 强制退出";
    // const char* advice_en = "Please restart device";

    const char* err = info ? info->error_msg : "";
    if (strstr(err, "Panic")) {
        reason_cn = "致命异常: 内存或指令错误";
        reason_en = "Fatal Exception: Memory/Instr Error";
        advice_cn = "程序试图访问非法内存地址";
    } else if (strstr(err, "Watchdog")) {
        reason_cn = "超时: 系统卡死/死循环";
        reason_en = "Timeout: System Freeze/Loop";
        advice_cn = "程序长时间占用CPU未释放";
    } else if (strstr(err, "Software")) {
        reason_cn = "软件复位: 主动报错";
        reason_en = "Software Reset: Assert Failed";
        advice_cn = "程序遇到不可恢复的逻辑错误";
    } else if (strstr(err, "Loop")) {
        reason_cn = "异常: 应用频繁重启";
        reason_en = "Error: Rapid App Relaunch";
        advice_cn = "检测到应用进入死循环，已拦截";
    }

    // Draw Reason
    M5.Display.setFont(&fonts::efontCN_12);
    M5.Display.setTextColor(0xFF8888, 0x222222);  // Light Red
    M5.Display.setCursor(60, 52);
    M5.Display.printf("%s", reason_cn);
    M5.Display.setCursor(60, 66);
    M5.Display.setTextColor(0xAAAAAA, 0x222222);  // Grey
    M5.Display.printf("%s", reason_en);

    // Draw Advice (Scrollable or just 2 lines)
    M5.Display.setTextColor(TFT_CYAN, 0x222222);
    M5.Display.setCursor(10, 82);
    M5.Display.printf("分析: %s", advice_cn);
    // M5.Display.setCursor(10, 96);
    // M5.Display.printf("Note: %s", advice_en); // Space is tight

    // --- 3. Action Buttons (Bottom) ---
    int btn_y = 105;  // Moved down slightly
    int btn_h = 24;

    // Left Button: Reboot (GO)
    M5.Display.fillRoundRect(5, btn_y, 110, btn_h, 4, 0x444444);
    M5.Display.drawRoundRect(5, btn_y, 110, btn_h, 4, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, 0x444444);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("重启系统 (GO)", 60, btn_y + btn_h / 2 + 2);

    // Right Button: Report/Exit (ESC)
    M5.Display.fillRoundRect(125, btn_y, 110, btn_h, 4, TFT_BLUE);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLUE);
    if (is_runtime) {
        M5.Display.drawString("我知道了 (ESC)", 180, btn_y + btn_h / 2 + 2);
    } else {
        M5.Display.drawString("返回桌面 (ESC)", 180, btn_y + btn_h / 2 + 2);
    }

    // --- 4. Auto-Sentinel Timer (v5.0 Enhanced) ---
    uint32_t screen_start            = GetHAL().millis();
    const uint32_t AUTO_SAFE_TIMEOUT = 5000;  // 5 Seconds to auto-enter Life Support

    // Give it a moment to render
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Hold Timers
    uint32_t go_hold_start       = 0;
    const uint32_t HOLD_DURATION = 3000;  // 3 Seconds

    while (1) {
        GetHAL().update();
        GetHAL().keyboard.update();

        // --- 检测任意按键 (用于打断倒计时) ---
        auto keyEvent        = GetHAL().keyboard.getLatestKeyEvent();
        bool any_key_pressed = keyEvent.state || GetHAL().homeButton().isPressed() || M5.BtnA.isPressed();

        // --- 任意键打断自动安全模式倒计时 ---
        if (any_key_pressed && !is_runtime) {
            screen_start = GetHAL().millis();  // 重置倒计时
        }

        // --- Auto-Safe Logic ---
        if (!is_runtime) {
            uint32_t elapsed = GetHAL().millis() - screen_start;
            if (elapsed < AUTO_SAFE_TIMEOUT) {
                int sec_left = (AUTO_SAFE_TIMEOUT - elapsed) / 1000 + 1;
                // 清除底部区域防止残影
                M5.Display.fillRect(0, 135 - 14, 240, 14, 0x222222);
                M5.Display.setTextColor(TFT_ORANGE, 0x222222);
                M5.Display.setFont(&fonts::efontCN_10);
                M5.Display.setTextDatum(middle_center);
                char auto_buf[64];
                sprintf(auto_buf, "无操作 %d 秒后进入安全模式 | 按任意键取消", sec_left);
                M5.Display.drawString(auto_buf, 120, 135 - 7);
                M5.Display.setTextDatum(top_left);
            } else {
                // Timeout reached!
                GetHAL().renderSafeModeMonitor();
                break;
            }
        }

        // --- GO Button Logic (Hold to Reboot) ---
        if (GetHAL().homeButton().isPressed()) {
            if (go_hold_start == 0) go_hold_start = GetHAL().millis();

            uint32_t held_ms = GetHAL().millis() - go_hold_start;
            float ratio      = (float)held_ms / HOLD_DURATION;
            if (ratio > 1.0f) ratio = 1.0f;

            // Draw Button Animation (Filling from left)
            M5.Display.fillRoundRect(5, btn_y, 110, btn_h, 4, 0x444444);
            M5.Display.fillRoundRect(5, btn_y, (int)(110 * ratio), btn_h, 4, TFT_WHITE);

            M5.Display.setTextDatum(middle_center);
            M5.Display.setTextColor(ratio > 0.5 ? TFT_BLACK : TFT_WHITE, ratio > 0.5 ? TFT_WHITE : 0x444444);

            int seconds_left = 3 - (held_ms / 1000);
            if (seconds_left < 0) seconds_left = 0;

            char buf[32];
            sprintf(buf, "重启中 %d...", seconds_left);
            M5.Display.drawString(buf, 60, btn_y + btn_h / 2 + 2);
            M5.Display.setTextDatum(top_left);

            if (held_ms >= HOLD_DURATION) {
                reboot_system();
                break;
            }
        } else {
            if (go_hold_start != 0) {
                go_hold_start = 0;
                M5.Display.fillRoundRect(5, btn_y, 110, btn_h, 4, 0x444444);
                M5.Display.drawRoundRect(5, btn_y, 110, btn_h, 4, TFT_WHITE);
                M5.Display.setTextDatum(middle_center);
                M5.Display.setTextColor(TFT_WHITE, 0x444444);
                M5.Display.drawString("重启系统 (GO)", 60, btn_y + btn_h / 2 + 2);
                M5.Display.setTextDatum(top_left);
            }
        }

        // --- 功能键处理 ---
        // ESC / Enter: 返回桌面或关闭
        bool is_confirm_pressed =
            (keyEvent.state && (keyEvent.keyCode == KEY_ENTER || keyEvent.keyCode == 13 || keyEvent.keyCode == 10));
        bool is_esc_pressed = (keyEvent.state && (keyEvent.keyCode == KEY_ESC || keyEvent.keyCode == 27));

        if (is_confirm_pressed || is_esc_pressed) {
            if (is_runtime) {
                break;
            } else {
                return_to_desktop();
                break;
            }
        }

        // Manual Safe Mode (S key)
        if (keyEvent.state && (keyEvent.keyCode == KEY_S || keyEvent.keyCode == 's')) {
            GetHAL().renderSafeModeMonitor();
            break;
        }

        // Log output (L key)
        if (keyEvent.state && keyEvent.keyCode == KEY_L) {
            ESP_LOGE(TAG, "=== CRASH DUMP ===");
            ESP_LOGE(TAG, "App: %s", info ? info->task_name : "Unknown");
            ESP_LOGE(TAG, "Reason: %s", info ? info->error_msg : "Unknown");
            if (is_runtime) {
                ESP_LOGE(TAG, "Type: Runtime Error (Non-Fatal)");
            } else {
                ESP_LOGE(TAG, "Type: System Crash (Fatal)");
            }

            M5.Display.fillRect(80, 50, 80, 20, TFT_BLACK);
            M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
            M5.Display.drawString("已输出日志", 120, 60);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            M5.Display.fillRect(80, 50, 80, 20, 0x222222);
            M5.Display.setTextColor(0xFF8888, 0x222222);
            M5.Display.drawString(info ? info->error_msg : "未知错误", 60, 60);
        }

        GetHAL().keyboard.clearKeyEvent();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// 重启系统
void reboot_system()
{
    ESP_LOGI(TAG, "Rebooting system...");
    esp_restart();
}

// 返回桌面
void return_to_desktop()
{
    ESP_LOGI(TAG, "Returning to desktop...");

    // 重置画布
    GetHAL().resetCanvas();

    // 清除崩溃信息
    clear_crash_info();

    // 强制关闭所有正在运行的应用（Launcher除外）
    auto* am = mooncake::GetMooncake().getAppAbilityManager();
    if (am) {
        auto apps = am->getAllAbilityInstance();
        for (auto* ability : apps) {
            auto app = static_cast<mooncake::AppAbility*>(ability);
            // 如果应用正在运行且不是 Launcher，则关闭它
            if (app->currentState() == mooncake::AppAbility::StateRunning && app->getAppInfo().name != "Launcher") {
                ESP_LOGW(TAG, "Closing app: %s", app->getAppInfo().name.c_str());
                app->close();
            }
        }
    }

    // 应用导致的崩溃可能把 WiFi 带进异常状态, 强制重置一次让后续 glink 干净重建
    // (硬 panic 重启来不及做清理, 但这种 return_to_desktop() 是应用级崩溃后的路径)
    ESP_LOGI(TAG, "Force-resetting WiFi to recover from app crash...");
    GetHAL().wifiDeinit();
    vTaskDelay(20 / portTICK_PERIOD_MS);
    GetHAL().wifiInit();

    // 打开桌面应用
    mooncake::GetMooncake().openApp(0);
}

// 显示灾难性倒计时警报 (自动 10 秒倒计时)
void show_disaster_alert(const char* reason)
{
    if (GetHAL().isSystemPopupActive()) return;
    GetHAL().setSystemPopupActive(true);

    // [FUTURE FIX] Sandbox the UI context
    Hal::UIContextGuard guard;
    M5.Display.setFont(&fonts::efontCN_12);
    M5.Display.setTextSize(1.0);

    const char* countdown_voice[] = {"十", "九", "八", "七", "六", "五", "四", "三", "二", "一"};

    for (int i = 10; i >= 1; i--) {
        // 灾难模式：红白高频闪烁背景 (每秒闪烁 2 次)
        for (int f = 0; f < 2; f++) {
            M5.Display.fillScreen(TFT_RED);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            M5.Display.fillScreen(TFT_WHITE);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        // 绘制弹窗内容
        int bw = 220, bh = 100;
        int bx = (240 - bw) / 2;
        int by = (135 - bh) / 2;

        M5.Display.fillRoundRect(bx, by, bw, bh, 8, TFT_BLACK);
        M5.Display.drawRoundRect(bx, by, bw, bh, 8, TFT_RED);

        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.setTextDatum(top_center);
        M5.Display.setFont(&fonts::efontCN_14);
        M5.Display.drawString("!!! 灾难级安全预警 !!!", 120, by + 10);

        M5.Display.setFont(&fonts::efontCN_12);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString(reason, 120, by + 35);

        M5.Display.setFont(&fonts::efontCN_24);
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        char buf[64];
        sprintf(buf, "%s (%d)", countdown_voice[10 - i], i);
        M5.Display.drawString(buf, 120, by + 55);

        M5.Display.setFont(&fonts::efontCN_10);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.drawString("系统即将强制重启以保护硬件", 120, by + 85);

        // 内核心跳
        GetHAL().reportAppHealth();
    }

    // 倒计时结束，强制重启
    esp_restart();
}

// 显示标准系统弹窗
bool show_system_popup(const char* title, const char* message, PopupType type, bool is_long_press)
{
    if (GetHAL().isSystemPopupActive()) return false;
    GetHAL().setSystemPopupActive(true);

    // [FUTURE FIX] Save current App UI context
    Hal::UIContextGuard guard;

    // Force system font and style - Larger for better readability (Sentinel v3.9)
    M5.Display.setFont(&fonts::efontCN_14);
    M5.Display.setTextSize(1.0);
    M5.Display.setTextDatum(top_left);

    // 强制点亮屏幕
    M5.Display.setBrightness(255);

    uint16_t theme_color = TFT_YELLOW;
    uint16_t text_color  = TFT_BLACK;
    if (type == POPUP_TYPE_ERROR) {
        theme_color = TFT_RED;
        text_color  = TFT_WHITE;
    } else if (type == POPUP_TYPE_INFO) {
        theme_color = TFT_BLUE;
        text_color  = TFT_WHITE;
    }

    // 准备一个全屏遮罩（半透明效果或纯黑）
    // 为了极致安卓体验，我们先对背景进行“暗化”
    if (type == POPUP_TYPE_ERROR) {
        // 灾难模式：红白高频闪烁背景
        for (int i = 0; i < 4; i++) {
            M5.Display.fillScreen(TFT_RED);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            M5.Display.fillScreen(TFT_WHITE);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    } else {
        for (int y = 0; y < 135; y += 2) {
            for (int x = 0; x < 240; x += 2) {
                uint16_t c = M5.Display.readPixel(x, y);
                M5.Display.drawPixel(x, y, (c >> 1) & 0x7BEF);
            }
        }
    }

    auto render_popup = [&](float ratio, bool is_holding) {
        // 1. 绘制背景框 (Optimized size for better readability)
        int bw = 220, bh = 100;  // Slightly wider, adjusted height
        int bx = (240 - bw) / 2;  // Center horizontally
        int by = (135 - bh) / 2;  // Center vertically

        // Draw shadow effect (subtle)
        M5.Display.fillRoundRect(bx + 3, by + 3, bw, bh, 10, 0x333333);
        M5.Display.fillRoundRect(bx, by, bw, bh, 10, TFT_BLACK);
        M5.Display.drawRoundRect(bx, by, bw, bh, 10, theme_color);
        M5.Display.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 9, theme_color);  // Double border

        // 2. 标题栏 (Enhanced with icon area)
        int title_h = 24;
        M5.Display.fillRect(bx, by, bw, title_h, theme_color);

        // Draw type-specific icon
        if (type == POPUP_TYPE_ERROR) {
            M5.Display.setTextColor(TFT_WHITE, theme_color);
            M5.Display.setFont(&fonts::efontCN_16);
            M5.Display.setTextDatum(middle_left);
            M5.Display.drawString(" ⚠ ", bx + 8, by + title_h / 2);  // Warning icon
        } else if (type == POPUP_TYPE_WARNING) {
            M5.Display.setTextColor(TFT_WHITE, theme_color);
            M5.Display.setFont(&fonts::efontCN_16);
            M5.Display.setTextDatum(middle_left);
            M5.Display.drawString(" ⚡ ", bx + 8, by + title_h / 2);  // Lightning/warning icon
        } else {
            M5.Display.setTextColor(TFT_WHITE, theme_color);
            M5.Display.setFont(&fonts::efontCN_16);
            M5.Display.setTextDatum(middle_left);
            M5.Display.drawString(" ℹ ", bx + 8, by + title_h / 2);  // Info icon
        }

        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::efontCN_12);
        M5.Display.drawString(title, 120, by + title_h / 2);

        // 3. 内容 (Improved spacing and font)
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.setTextDatum(top_left);
        M5.Display.setFont(&fonts::efontCN_12);

        int text_x = bx + 12;
        int text_y = by + title_h + 10;

        std::string msg = message;
        size_t pos      = 0;
        while ((pos = msg.find("\\n")) != std::string::npos) {
            M5.Display.setCursor(text_x, text_y);
            M5.Display.println(msg.substr(0, pos).c_str());
            msg.erase(0, pos + 2);
            text_y += 15;  // Optimized line spacing
        }
        M5.Display.setCursor(text_x, text_y);
        M5.Display.println(msg.c_str());

        // 4. 确认按钮 (Enhanced style)
        int btn_w = 130, btn_h = 24;
        int btn_x = (240 - btn_w) / 2;
        int btn_y = by + bh - btn_h - 8;

        M5.Display.setFont(&fonts::efontCN_12);
        M5.Display.setTextDatum(middle_center);

        if (is_long_press) {
            if (is_holding) {
                // Progress bar style button fill
                M5.Display.fillRoundRect(btn_x, btn_y, (int)(btn_w * ratio), btn_h, 6, theme_color);
                char progress_buf[32];
                sprintf(progress_buf, "确认中... %d%%", (int)(ratio * 100));
                M5.Display.setTextColor(ratio > 0.5 ? text_color : theme_color, ratio > 0.5 ? theme_color : TFT_BLACK);
                M5.Display.drawString(progress_buf, 120, btn_y + btn_h / 2);
            } else {
                M5.Display.drawRoundRect(btn_x, btn_y, btn_w, btn_h, 6, theme_color);
                M5.Display.setTextColor(theme_color, TFT_BLACK);
                M5.Display.drawString("⏱ 长按回车确认", 120, btn_y + btn_h / 2);
            }
        } else {
            M5.Display.drawRoundRect(btn_x, btn_y, btn_w, btn_h, 6, theme_color);
            M5.Display.setTextColor(theme_color, TFT_BLACK);
            M5.Display.drawString("✓ 我知道了", 120, btn_y + btn_h / 2);
        }
    };

    // 初始绘制
    render_popup(0, false);

    // 等待按键释放，防止上一层交互的回车残留
    vTaskDelay(200 / portTICK_PERIOD_MS);
    GetHAL().keyboard.clearKeyEvent();

    uint32_t hold_start          = 0;
    const uint32_t HOLD_DURATION = 1500;

    while (1) {
        GetHAL().reportAppHealth();
        M5.update();
        GetHAL().keyboard.update();
        auto keyEvent = GetHAL().keyboard.getLatestKeyEvent();

        // 增加对物理按键 G0 的支持作为兜底
        bool is_confirm_pressed = keyEvent.state || M5.BtnA.isPressed();

        if (!is_long_press) {
            if (is_confirm_pressed) {
                M5.Display.fillScreen(TFT_BLACK);
                GetHAL().setSystemPopupActive(false);
                GetHAL().keyboard.clearKeyEvent();
                return true;
            }
        } else {
            if (is_confirm_pressed) {
                if (hold_start == 0) hold_start = GetHAL().millis();
                uint32_t held = GetHAL().millis() - hold_start;
                float ratio   = (float)held / HOLD_DURATION;
                if (ratio > 1.0f) ratio = 1.0f;

                render_popup(ratio, true);

                if (held >= HOLD_DURATION) {
                    M5.Display.fillScreen(TFT_BLACK);
                    GetHAL().setSystemPopupActive(false);
                    GetHAL().keyboard.clearKeyEvent();
                    return true;
                }
            } else {
                if (hold_start != 0) {
                    hold_start = 0;
                    render_popup(0, false);
                }
            }
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// 显示生命安全报警 (Sentinel v4.9d Enhanced)
void show_life_alert(const char* title, const char* message, uint16_t color)
{
    if (GetHAL().isSystemPopupActive()) return;
    GetHAL().setSystemPopupActive(true);

    Hal::UIContextGuard guard;
    M5.Display.setBrightness(255);

    uint32_t start_ts = GetHAL().millis();
    bool is_panic     = (strstr(title, "惊恐") != nullptr);

    while (1) {
        GetHAL().reportAppHealth();
        M5.update();
        GetHAL().keyboard.update();

        // 任意键退出，但增加一个最小显示时间 (1秒) 防止误触瞬秒
        if (GetHAL().millis() - start_ts > 1000) {
            if (GetHAL().keyboard.getLatestKeyEvent().state || M5.BtnA.wasPressed()) break;
        }

        // --- 1. 背景与边框闪烁 (Panic Attack 特效) ---
        uint32_t now = GetHAL().millis();
        bool flash   = (now / 400) % 2;  // 慢速闪烁

        if (flash) {
            M5.Display.drawRect(0, 0, 240, 135, color);
            M5.Display.drawRect(1, 1, 238, 133, color);
            M5.Display.drawRect(2, 2, 236, 131, color);
        } else {
            M5.Display.drawRect(0, 0, 240, 135, TFT_BLACK);
            M5.Display.drawRect(1, 1, 238, 133, TFT_BLACK);
            M5.Display.drawRect(2, 2, 236, 131, TFT_BLACK);
        }

        // --- 2. 标题 ---
        M5.Display.setTextColor(color, TFT_BLACK);
        M5.Display.setTextDatum(top_center);
        M5.Display.setFont(&fonts::efontCN_24);
        M5.Display.drawString(title, 120, 15);

        // --- 3. 呼吸引导 (如果是惊恐发作) ---
        if (is_panic) {
            // 紧急干预程序：视觉引导呼气/吸气
            float breath = (sin(now / 1000.0f) + 1.0f) / 2.0f;  // 4秒一周期
            int radius   = 10 + (int)(30 * breath);
            M5.Display.drawCircle(120, 75, 42, 0x444444);  // 辅助圆
            M5.Display.fillCircle(120, 75, radius, color);

            M5.Display.setFont(&fonts::efontCN_14);
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
            M5.Display.drawString(breath > 0.5f ? "请深吸气..." : "请慢呼气...", 120, 115);
        } else {
            // 普通报警：显示详细内容
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
            M5.Display.setFont(&fonts::efontCN_12);

            std::string msg = message;
            size_t pos      = 0;
            int y           = 55;
            while ((pos = msg.find("\\n")) != std::string::npos) {
                M5.Display.drawString(msg.substr(0, pos).c_str(), 120, y);
                msg.erase(0, pos + 2);
                y += 18;
            }
            M5.Display.drawString(msg.c_str(), 120, y);

            M5.Display.setTextColor(0x7BEF, TFT_BLACK);
            M5.Display.setFont(&fonts::efontCN_10);
            M5.Display.drawString("按任意键确认并关闭", 120, 115);
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }

    M5.Display.fillScreen(TFT_BLACK);
    
    // CRITICAL FIX: Restore all canvases after popup close
    // Without this, keyboard bar and system bar disappear!
    GetHAL().pushCanvasSystemBar();
    GetHAL().pushCanvasKeyboardBar();
    GetHAL().pushCanvas();
    
    GetHAL().setSystemPopupActive(false);
    GetHAL().keyboard.clearKeyEvent();
}

// 显示电源警告
void show_power_warning()
{
    const char* msg =
        "检测到 USB 已插入，但电池未充电\\n\\n原因: 底部电源开关处于 ON 状态\\n建议: 关机或将开关拨至 OFF "
        "以开启快充\\n\\n当前仅通过 USB 供电，电池可能耗尽。";
    show_system_popup("充电模式提示 (Power)", msg, POPUP_TYPE_WARNING);
}

}  // namespace crash_handler

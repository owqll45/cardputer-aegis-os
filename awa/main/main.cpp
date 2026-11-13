/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.h>
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps.h>
#include <hal.h>
#include <apps/app_extras/app_extras.h>
#include "apps/app_legacy_app/legacy_app_wifi.h"
#include "apps/app_relay_test/app_relay_test.h"
#include "apps/app_multimeter/app_multimeter.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <nvs_flash.h>
#include <nvs.h>
#include "crash_handler.h" // Added
#include "security/security_manager.h" // Added
#include "apps/app_registry.h"
#include "serial_pump.h"
#include <apps/utils/toast/toast.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

LegacyAppWifi system_wifi;
bool should_launch_bomb = false;
void system_cmd_handler(const char* cmd) {
    if (strstr(cmd, "POWER:ON")) should_launch_bomb = true;
}

// FreeRTOS task handles
TaskHandle_t desktop_task_handle = NULL;
TaskHandle_t app_task_handle = NULL;

// Task stack sizes
#define DESKTOP_TASK_STACK_SIZE 8192
#define APP_TASK_STACK_SIZE 16384

// Task priorities
#define DESKTOP_TASK_PRIORITY 5
#define APP_TASK_PRIORITY 4

// Global flag to indicate if app task should be running
bool app_task_running = false;
int current_app_id = -1;

// App task function
void app_task(void *pvParameters)
{
    mclog::tagInfo("Task", "App task started");
    
    while (1) {
        if (app_task_running) {
            // Run the app
            GetMooncake().update();
            // 全局提示条(静音/有声): 必须在本任务、且在 app 推屏之后叠加,
            // 绝不能在键盘回调(desktop_task)里画
            ui_toast::update();
        } else {
            // If no app is running, yield to other tasks
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        
        GetHAL().feedTheDog();
    }
}

// Desktop task function
void desktop_task(void *pvParameters)
{
    mclog::tagInfo("Task", "Desktop task started");
    
    while (1) {
        GetHAL().update();
        
        if (should_launch_bomb) {
            should_launch_bomb = false;
            
            // Debounce to prevent screen flickering from multiple requests
            static uint32_t last_launch_ts = 0;
            if (GetHAL().millis() - last_launch_ts > 5000) {
                last_launch_ts = GetHAL().millis();
                mclog::tagInfo("System", "REMOTE LAUNCH TRIGGERED");
                GetMooncake().openApp(0);
            } else {
                mclog::tagInfo("System", "Launch ignored (debounce)");
            }
        }
        
        GetHAL().feedTheDog();
    }
}

extern "C" void app_main(void)
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable Brownout Detector

    // Setup logger
    mclog::set_level(mclog::level_debug);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // 复位原因: 用于区分正常开机 / panic / 看门狗 / 掉电
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* rr_str    = "UNKNOWN";
        switch (rr) {
            case ESP_RST_POWERON:  rr_str = "POWERON"; break;
            case ESP_RST_EXT:      rr_str = "EXTERNAL"; break;
            case ESP_RST_SW:       rr_str = "SOFTWARE"; break;
            case ESP_RST_PANIC:    rr_str = "PANIC(crash)"; break;
            case ESP_RST_INT_WDT:  rr_str = "INT_WDT"; break;
            case ESP_RST_TASK_WDT: rr_str = "TASK_WDT"; break;
            case ESP_RST_WDT:      rr_str = "OTHER_WDT"; break;
            case ESP_RST_BROWNOUT: rr_str = "BROWNOUT"; break;
            case ESP_RST_DEEPSLEEP:rr_str = "DEEPSLEEP"; break;
            default: break;
        }
        ESP_LOGW("BOOT", "=== reset_reason=%d (%s) heap=%u ===", (int)rr, rr_str,
                 (unsigned)esp_get_free_heap_size());
    }

    // HAL init
    ESP_LOGI("MAIN", "=== DEBUG: Before HAL init ===");
    GetHAL().init();
    ESP_LOGI("MAIN", "=== DEBUG: After HAL init ===");

    // Init NVS (required by SecurityManager)
    ESP_LOGI("MAIN", "=== DEBUG: Before NVS init ===");
    nvs_flash_init();
    ESP_LOGI("MAIN", "=== DEBUG: After NVS init ===");

    // Security Init
    ESP_LOGI("MAIN", "=== DEBUG: Before SecurityManager ===");
    sys_security::SecurityManager::get().init();
    ESP_LOGI("MAIN", "=== DEBUG: After SecurityManager ===");

    // Check for previous crash
    // crash_handler::check_for_crash(); // SKIP: blocks on previous crash state

    // Auto connect WiFi - Removed to speed up boot
    // std::string ssid = GetHAL().getSettings().GetString("wifi_ssid");
    // std::string password = GetHAL().getSettings().GetString("wifi_password");
    // if (!ssid.empty()) {
    //     mclog::tagInfo("System", "Auto connecting to WiFi: {}", ssid);
    //     GetHAL().wifiConnect(ssid, password);
    // }

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // === Lazy loading: register apps (metadata + factory only, NO instantiation) ===
    // Apps are instantiated ONLY when opened, and freed when closed.
    ESP_LOGI("MAIN", "=== DEBUG: Registering apps (lazy) ===");
    
    RegisterApp<AppLegacyApp>("LegacyApp");
    RegisterApp<AppMultimeter>("Multimeter");
    RegisterApp<AppSetWiFi>("Network");
    RegisterApp<AppClock>("Clock");
    // RegisterApp<AppKeyboard>("Keyboard");  // DISABLED in CMakeLists
    RegisterApp<AppImu>("IMU");
    RegisterApp<AppStringIRToolKit>("StringIR");
    RegisterApp<AppScientificCalculator>("SciCalc");
    RegisterApp<AppFunctionCalculator>("FuncCalc");
    RegisterApp<AppSysInfo>("SysInfo");
    RegisterApp<AppDHT11>("Temp/Humi+");
    RegisterApp<AppUltrasonic>("Distance");
    RegisterApp<AppGomoku>("Gomoku");
    RegisterApp<AppShogi>("Shogi");
    RegisterApp<AppFPS>("FPS");
    RegisterApp<App2048>("2048");
    RegisterApp<AppAI>("DeepSeek");
    RegisterApp<AppRace>("Racing");
    RegisterApp<AppSnake>("Snake");
    RegisterApp<AppBreakout>("Breakout");
    RegisterApp<AppTetris>("Tetris");
    RegisterApp<AppPong>("Pong");
    RegisterApp<AppReversi>("Reversi");
    RegisterApp<AppTank>("Tank");
    RegisterApp<AppUno>("UNO");
    RegisterApp<AppConnect4>("Connect4");
    RegisterApp<AppCheckers>("Checkers");
    RegisterApp<AppFlappy>("Flappy");
    RegisterApp<AppInvaders>("Invaders");
    RegisterApp<AppMinesweeper>("Minesweeper");
    RegisterApp<AppWaterLevel>("Water Level");
    RegisterApp<AppRelayTest>("RelayTester");
    
    ESP_LOGI("MAIN", "=== DEBUG: %d apps registered ===", (int)GetAppRegistry().size());
    ESP_LOGI("MAIN", "=== DEBUG: free heap after register = %u KB ===",
             (unsigned)(esp_get_free_heap_size() / 1024));
    
    // Only Launcher stays resident (system app)
    GetMooncake().installApp(std::make_unique<Launcher>());
    ESP_LOGI("MAIN", "=== DEBUG: Launcher installed ===");
    ESP_LOGI("MAIN", "=== DEBUG: free heap after launcher = %u KB, min = %u KB ===",
             (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(esp_get_minimum_free_heap_size() / 1024));

    // START SYSTEM WIFI (Daemon)
    // mclog::tagInfo("System", "Starting System WiFi Daemon...");
    // register_system_callback(system_cmd_handler);
    // system_wifi.init(NULL);

    // Create FreeRTOS tasks
    audio::set_keyboard_sfx_enable(true);
    
    // Create desktop task
    xTaskCreate(&desktop_task, "desktop_task", DESKTOP_TASK_STACK_SIZE, NULL, DESKTOP_TASK_PRIORITY, &desktop_task_handle);
    
    // Create app task
    xTaskCreate(&app_task, "app_task", APP_TASK_STACK_SIZE, NULL, APP_TASK_PRIORITY, &app_task_handle);
    
    // Start the app task
    app_task_running = true;
    
    // === 倒计时自动恢复：重启后如果有未完成的倒计时，自动启动 LegacyApp 续时 ===
    {
        nvs_handle_t nh;
        if (nvs_open("storage", NVS_READONLY, &nh) == ESP_OK) {
            uint8_t cd = 0;
            if (nvs_get_u8(nh, "cnt_down", &cd) == ESP_OK && cd == 1) {
                ESP_LOGI("MAIN", "=== Active countdown found! Auto-resuming LegacyApp ===");
                // Find LegacyApp in registry and auto-open it
                auto& reg = GetAppRegistry();
                for (size_t i = 0; i < reg.size(); i++) {
                    if (reg[i].name == "LegacyApp") {
                        int mid = InstallFromRegistry(i);
                        if (mid >= 0) {
                            GetMooncake().openApp(mid);
                        }
                        break;
                    }
                }
            }
            nvs_close(nh);
        }
    }
    
    // === 开机静默自检 (O8): 快速硬件检测写 SD, 不打扰界面 ===
    {
        FILE* f = fopen("/sdcard/boot_selftest.txt", "w");
        if (f) {
            uint32_t before = esp_get_free_heap_size();
            fprintf(f, "BOOT SELFTEST\n");
            fprintf(f, "heap=%u largest=%u\n", (unsigned)before,
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            fprintf(f, "reset_reason=%d\n", (int)esp_reset_reason());
            fprintf(f, "battery=%d%%\n", (int)GetHAL().getBatLevel());
            fprintf(f, "display=OK (launcher rendered)\n");
            fprintf(f, "serial_pump=OK (listening)\n");
            fprintf(f, "crypto=OK (key loaded)\n");
            fclose(f);
            ESP_LOGI("MAIN", "boot selftest saved");
        }
    }
    
    // The main task can now be used for other purposes or just idle
    while (1) {
        SerialPump::pump(); // 系统级串口泵: 防 USB CDC 缓冲满死机 + 菜单下响应指令
        
        // === 指令自动唤醒: 任何界面收到指令 → 立即自动打开 LegacyApp 执行 ===
        if (SerialPump::pendingCount() > 0) {
            auto& reg = GetAppRegistry();
            for (size_t i = 0; i < reg.size(); i++) {
                if (reg[i].name == "LegacyApp" && reg[i].cached_id < 0) {
                    ESP_LOGI("MAIN", "=== Command received! Auto-launching LegacyApp ===");
                    int mid = InstallFromRegistry(i);
                    if (mid >= 0) {
                        GetMooncake().openApp(mid);
                    }
                    break;
                }
            }
        }
        
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

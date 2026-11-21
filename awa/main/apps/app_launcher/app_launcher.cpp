/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include <apps/utils/theme.h>
#include <apps/utils/common.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <hal.h>
#include "crash_handler.h"
#include <apps/app_registry.h>
#include <apps/utils/glink/glink.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <hal/utils/perf.h>

using namespace mooncake;

#include <map>
static std::map<std::string, std::string> _shared_data_store;

void Launcher::setSystemSharedData(const char* key, const char* value) {
    if (key && value) {
        _shared_data_store[key] = value;
    }
}

const char* Launcher::getSystemSharedData(const char* key) {
    if (key && _shared_data_store.count(key)) {
        return _shared_data_store[key].c_str();
    }
    return nullptr;
}

void Launcher::onCreate()
{
    setAppInfo().name = "Launcher";
    // 启动器是系统 app: 打开大内存应用时 Mooncake 的"内存守卫"会把后台非系统 app 全部 close(),
    // 之前 Launcher 被误杀 → 应用退出后没人接手, 画面永远停在被退出的 app 上(表现为"返回键退不出去")。
    setAppInfo().isSystemApp = true;
    mclog::tagInfo(getAppInfo().name, "on create");
    
    // Set current app name for crash handler
    crash_handler::set_running_app("Launcher");

    // Init
    // If we are recovering from an app crash, skip boot anim
    // But check_for_crash is blocking, so if we are here, user already dismissed it.
    // However, if we want to "auto-restore", we can check if it was an app crash.
    // Actually, crash_handler::check_for_crash() clears the magic word after showing the screen.
    // So we don't know here.
    
    // Standard boot flow - using CANVAS mode to avoid display corruption
    // boot_anim(); // SKIP: canvas creation fails (no PSRAM)
    start_menu();
    start_system_bar();
    // start_keyboard_bar(); // DISABLED: user wants keyboard bar removed

    // ESP-NOW 镜像服务: 延后到桌面首帧之后再拉起(见 onRunning),
    // 避免 WiFi 初始化(~100ms + 42KB)挡住开机第一屏
    // glink_acquire("Launcher", GLINK_GID_LAUNCHER, GLINK_ICON_GENERIC);

    ESP_LOGI("Launcher", "=== DEBUG: desktop free heap = %u KB, min ever = %u KB, largest = %u KB ===",
             (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(esp_get_minimum_free_heap_size() / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

    open();
}

void Launcher::onRunning()
{
    // 性能口径: 桌面帧率 = 每秒完成的 onRunning 次数(含 pushCanvas 的 SPI 传输)
    static bool s_first_frame = true;
    static uint32_t s_frames = 0;
    static uint32_t s_fps_t0 = 0;
    static uint32_t s_render_us_sum = 0;

    // ESP-NOW 全局服务: 信标/心跳/补发/WiFi 存活探测(launcher 常驻 Running, 适合挂这里)
    glink_poll();

    // 首帧之后再拉起 ESP-NOW 镜像(此时画面已可见, WiFi 初始化不再挡住开机)
    static bool s_glink_ready = false;
    if (!s_glink_ready && _data.running_app_id < 0 && perf_ms() > 2500) {
        s_glink_ready = true;
        glink_acquire("Launcher", GLINK_GID_LAUNCHER, GLINK_ICON_GENERIC);
        perf_boot("glink_ready");
    }

    // Check if any external app was opened (e.g. via Remote / 串口自测)。
    // 必须每帧查: 之前每 10 帧查一次, 外部拉起的 app 在前几帧会被误判成"桌面",
    // 于是这里把 app 刚建好的画布 reset 掉(日志 "Canvas invalid, resetting")。
    if (_data.running_app_id == -1) {
        auto app_list = GetMooncake().getAppAbilityManager()->getAllAbilityInstance();
        for (auto& app_raw : app_list) {
            auto app = static_cast<AppAbility*>(app_raw);
            if (app->getId() != getId() && GetMooncake().getAppCurrentState(app->getId()) == AppAbility::StateRunning) {
                _data.running_app_id = app->getId();
                break;
            }
        }
    }

    // 防御性检查：确保画布有效。
    // 应用运行期间不检查：像 DeepSeek 录音时会临时释放整屏画布内存给 HTTPS 用。
    if (_data.running_app_id < 0 && !GetHAL().isCanvasValid()) {
        ESP_LOGW("Launcher", "Canvas invalid, resetting...");
        GetHAL().resetCanvas();
    }
    
    // If app is opened and running
    if (_data.running_app_id >= 0) {
        // If running app is closed
        if (GetMooncake().getAppCurrentState(_data.running_app_id) == AppAbility::StateSleeping) {
            
            // Set current app back to Launcher
            crash_handler::set_running_app("Launcher");
            ESP_LOGI("APP", "CLOSE -> Launcher heap=%u", (unsigned)esp_get_free_heap_size());
            
            ANIM_APP_CLOSE(); // 先执行关闭动�?            
            
            // === Lazy loading: uninstall closed app to free RAM ===
            for (size_t i = 0; i < GetAppRegistry().size(); i++) {
                if (GetAppRegistry()[i].cached_id == _data.running_app_id) {
                    UninstallFromRegistry(i);
                    break;
                }
            }
            _data.running_app_id = -1;
            
            // Fix: Force menu to ignore input until released to prevent auto-reopen
            reset_menu_input();
            GetHAL().goButton().clear();  // 丢弃退出时残留的 G0 点击，防止刚回桌面又被吃掉
            
            // 重置共享画布（尺寸/色深/裁剪），避免应用残留导致下次退出异常
            GetHAL().resetCanvas();
            
            // 日志：记录退出后桌面可用内存(内部堆)
            ESP_LOGI("Launcher", "Free heap after app close: %u KB", (unsigned)(esp_get_free_heap_size() / 1024));
            
            // Resume system bars and menu
            update_menu(true); // 强制重绘菜单
            render_system_bar();
            // render_keyboard_bar(); // DISABLED

            // 回到桌面: C3 镜像切回 Launcher 画面(若 WiFi 被上个 app 杀掉, 这里重建)
            glink_acquire("Launcher", GLINK_GID_LAUNCHER, GLINK_ICON_GENERIC);
        }
        // else: App is running, do NOT update system bar or keyboard bar
    } else {
        uint64_t t_begin = esp_timer_get_time();
        update_system_bar();
        update_menu();
        s_render_us_sum += (uint32_t)(esp_timer_get_time() - t_begin);
        s_frames++;

        if (s_first_frame) {
            s_first_frame = false;
            s_fps_t0 = perf_ms();
            perf_boot("first_frame");
        }

        uint32_t now_ms = perf_ms();
        if (now_ms - s_fps_t0 >= 5000) {
            float secs = (now_ms - s_fps_t0) / 1000.0f;
            ESP_LOGI("PERF", "desktop fps=%.1f frames=%u avg_render=%.2fms free=%uKB largest=%uKB",
                     s_frames / secs, (unsigned)s_frames,
                     s_frames ? (s_render_us_sum / 1000.0f / s_frames) : 0.0f,
                     (unsigned)perf_free_kb(), (unsigned)perf_largest_kb());
            s_frames = 0;
            s_render_us_sum = 0;
            s_fps_t0 = now_ms;
        }
    }
}

void Launcher::handle_app_open(int index, int appId)
{
    // Fix: Force wait for release BEFORE opening app
    reset_menu_input();
    GetHAL().goButton().clear();  // 清掉在桌面误按的 G0，避免应用一打开就退出
    
    // Check for rapid re-launch (仅防按键抖动: 300ms 内重复打开同一应用)
    // 之前用 2 秒阈值会误伤"退出应用后立刻再进"的正常操作, 还会弹"App Loop"崩溃页,
    // 表现为应用打不开/闪退, 这里改成直接忽略, 不弹崩溃界面.
    static int last_app_id = -1;
    static uint32_t last_open_time = 0;
    uint32_t now = GetHAL().millis();

    if (appId == last_app_id && (now - last_open_time) < 300) {
        ESP_LOGW("Launcher", "Ignore repeated open (bounce) appID: %d", appId);
        reset_menu_input();
        return;
    }
    
    last_app_id = appId;
    last_open_time = now;
    
    // === Lazy loading: uninstall all other apps first to free RAM ===
    for (size_t i = 0; i < GetAppRegistry().size(); i++) {
        if ((int)i != appId) {
            UninstallFromRegistry(i);
        }
    }
    
    // Install app from registry on demand (instantiates only NOW)
    int mooncake_id = InstallFromRegistry(appId);
    if (mooncake_id < 0) {
        ESP_LOGE("Launcher", "Failed to install app from registry: %d", appId);
        return;
    }
    
    auto app = static_cast<AppAbility*>(GetMooncake().getAppAbilityManager()->getAbilityInstance(mooncake_id));
    if (app) {
        crash_handler::set_running_app(app->getAppInfo().name.c_str());
        ESP_LOGI("APP", "OPEN '%s' id=%d heap=%u", app->getAppInfo().name.c_str(), appId,
                 (unsigned)esp_get_free_heap_size());
    }

    // 优化：释放不必要的内存
    GetHAL().canvas().fillScreen(TFT_BLACK);
    GetHAL().pushCanvas();

    // 重置主画布的剪裁区域和属性
    GetHAL().canvas().setClipRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    GetHAL().canvas().setPivot(0, 0);
    
    // 日志：记录当前内存状态
    ESP_LOGI("Launcher", "Free heap before app open: %u", (unsigned)esp_get_free_heap_size());
    
    GetMooncake().openApp(mooncake_id);
    _data.running_app_id = mooncake_id;
    // canvas 尺寸能直接反映全屏 sprite 是否分配成功(135)或降级(109)
    ESP_LOGI("APP", "canvas after open: %dx%d heap=%u",
             (int)GetHAL().canvas().width(), (int)GetHAL().canvas().height(),
             (unsigned)esp_get_free_heap_size());

    ANIM_APP_OPEN();
}

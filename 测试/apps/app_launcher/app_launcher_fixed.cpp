/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include "utils/audio/audio.h"
#include <apps/utils/theme.h>
#include <apps/utils/common.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <hal.h>

using namespace mooncake;

void Launcher::onCreate()
{
    setAppInfo().name = "Launcher";
    mclog::tagInfo(getAppInfo().name, "on create");

    // --- Audio Init (Restore Button Sound) ---
    audio::set_keyboard_sfx_enable(true);

    // Init
    boot_anim();
    start_menu();
    start_system_bar();
    start_keyboard_bar();

    open();
}

void Launcher::onRunning()
{
    // 优化：降低检测频率，每 10 帧检测一次
    static uint32_t check_external_app_counter = 0;

    // Check if any external app was opened (e.g. via Remote)
    if (_data.running_app_id == -1) {
        check_external_app_counter++;
        if (check_external_app_counter > 10) { // 降频处理
            check_external_app_counter = 0;
            
            auto app_list = GetMooncake().getAppAbilityManager()->getAllAbilityInstance();
            for (auto& app_raw : app_list) {
                auto app = static_cast<AppAbility*>(app_raw);
                if (app->getId() != getId() && GetMooncake().getAppCurrentState(app->getId()) == AppAbility::StateRunning) {
                    _data.running_app_id = app->getId();
                    break;
                }
            }
        }
    }

    // If app is opened and running
    if (_data.running_app_id >= 0) {
        // If running app is closed
        if (GetMooncake().getAppCurrentState(_data.running_app_id) == AppAbility::StateSleeping) {
            ANIM_APP_CLOSE(); // 先执行关闭动画
            
            _data.running_app_id = -1;
            
            // Resume system bars and menu
            update_menu(true); // 强制重绘菜单
            render_system_bar();
            render_keyboard_bar();
        }
        // else: App is running, do NOT update system bar or keyboard bar
    } else {
        update_system_bar();
        update_menu();
    }
}

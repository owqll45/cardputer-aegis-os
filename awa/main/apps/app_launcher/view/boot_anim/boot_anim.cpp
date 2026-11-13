/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "../../app_launcher.h"
#include "assets/boot_sfx.h"
#include "assets/logo_adv.h"
#include "assets/logo.h"
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <smooth_ui_toolkit.h>
#include <mooncake_log.h>
#include <hal.h>

// #define NO_BOOT_PLAY

using namespace smooth_ui_toolkit;

// void start_arkanoid();  // TEMP DISABLED - arkanoid game not available

void Launcher::boot_anim()
{
    mclog::tagInfo(getAppInfo().name, "start boot anim CYBER (FULLSCREEN CANVAS MODE)");

    M5.Display.setBrightness(255);

    // Create FULLSCREEN temporary sprite for boot animation
    lgfx::LGFX_Sprite boot_canvas(&M5.Display);
    if (!boot_canvas.createSprite(240, 135)) {
        mclog::tagError(getAppInfo().name, "Failed to create fullscreen boot canvas");
        return;
    }
    boot_canvas.setColorDepth(16);
    boot_canvas.fillScreen(0x000000);
    boot_canvas.pushSprite(0, 0);

    // Cyberpunk Boot Sequence - Optimized for full screen (240x135)
    const char* boot_lines[] = {
        "[ OK ] CPU Cores Init",
        "[ OK ] MemCheck: PASS",
        "[ OK ] Secure Boot: ON",
        "[ OK ] Load Kernel...",
        "[ OK ] Mount FS...",
        "[ OK ] Security Svcs...",
        "[ OK ] User Profile...",
        "[DONE] SYSTEM READY."
    };

    // Use font optimized for fullscreen display
    boot_canvas.setFont(&fonts::efontCN_12);
    boot_canvas.setTextColor(0x07E0); // Terminal Green
    boot_canvas.setTextSize(1);

    int line_height = 14;
    int start_y = 8;  // More top padding for fullscreen

    // Play sfx start
    #ifndef NO_BOOT_PLAY
    M5.Speaker.setVolume(255);
    #endif

    for (int i = 0; i < 8; i++) {
        boot_canvas.setCursor(10, start_y + (i * line_height));  // Left-aligned with margin
        boot_canvas.print(boot_lines[i]);

        // Push to display after each line (FULLSCREEN)
        boot_canvas.pushSprite(0, 0);

        vTaskDelay((40 + (rand() % 100)) / portTICK_PERIOD_MS);
        vTaskDelay(1); // Feed watchdog
    }

    // Progress Bar (Full width, centered)
    int bar_width = 200;
    int bar_height = 8;
    int bar_x = (240 - bar_width) / 2;  // Center horizontally
    int bar_y = 120;  // Near bottom

    boot_canvas.drawRect(bar_x, bar_y, bar_width, bar_height, 0x07E0);
    for(int w=0; w<bar_width-4; w+=6) {
        boot_canvas.fillRect(bar_x+2, bar_y+2, w, bar_height-4, 0x07E0);
        boot_canvas.pushSprite(0, 0);
        vTaskDelay(18 / portTICK_PERIOD_MS);
    }

    // "PRESS ANY KEY" prompt (centered at bottom)
    boot_canvas.setTextColor(0xFFFF); // White
    boot_canvas.setTextDatum(middle_center);
    boot_canvas.drawString("PRESS ANY KEY TO CONTINUE", 120, 132);
    boot_canvas.pushSprite(0, 0);

    // Wait enter - Use raw M5 API only
    int egg_count = 0;
    while (1) {
        vTaskDelay(50 / portTICK_PERIOD_MS);
        M5.update();
        GetHAL().keyboard.update();

        if (M5.BtnA.wasPressed()) {
            M5.Speaker.setVolume(90);
            audio::play_random_tone();
            break;
        }

        auto key_event = GetHAL().keyboard.getLatestKeyEvent();

        if (!key_event.state && key_event.keyCode == KEY_G) {
            for (int i = 0; i < 3; i++) {
                vTaskDelay(40 / portTICK_PERIOD_MS);
                audio::play_random_tone();
            }
            egg_count++;
            if (egg_count > 2) {
                // TEMP: arkanoid disabled
                mclog::tagInfo(getAppInfo().name, "Easter egg activated! (arkanoid disabled)");
            }
        } else if (!key_event.state && key_event.keyCode != KEY_NONE) {
            break;
        }
    }

    GetHAL().keyboard.clearKeyEvent();

    // Clean up fullscreen canvas
    boot_canvas.deleteSprite();

    mclog::tagInfo(getAppInfo().name, "boot anim completed (FULLSCREEN MODE)");
}

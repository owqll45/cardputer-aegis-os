/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>
#include <apps/utils/theme.h>

#define FW_VERSION "AEGIS OS v2.1.0"

// Dynamic Display Constants
#define SCREEN_WIDTH  (GetHAL().canvas().width())
#define SCREEN_HEIGHT (GetHAL().canvas().height())
#define CENTER_X      (SCREEN_WIDTH / 2)
#define CENTER_Y      (SCREEN_HEIGHT / 2)

// Theme Shortcuts
#define THEME_BG      THEME_COLOR_BG

#define ANIM_APP_OPEN()                                                                                     \
    for (int i = 10; i < 123; i += 8) {                                                                     \
        GetHAL().canvas().fillSmoothCircle(CENTER_X, CENTER_Y - 10, i,                                        \
                                         THEME_BG);                                                         \
        GetHAL().pushCanvas();                                                                              \
    }

#define ANIM_APP_CLOSE()                                                                                    \
    for (int i = 123; i > 10; i -= 8) {                                                                     \
        update_menu(false);                                                                                 \
        GetHAL().canvas().fillSmoothCircle(CENTER_X, CENTER_Y - 10, i,                                        \
                                         THEME_BG);                                                         \
        GetHAL().pushCanvas();                                                                              \
    }

// 固定步长累加器: 返回本帧应执行的逻辑步数(最多 4 步, 防卡顿雪崩)
static inline int game_steps(uint32_t now_ms, uint32_t& last_ms, uint32_t step_ms = 16)
{
    uint32_t delta = now_ms - last_ms;
    if (delta < step_ms) {
        return 0;
    }
    int steps = (int)(delta / step_ms);
    last_ms  = now_ms;
    return steps > 4 ? 4 : steps;
}

struct AppIcon_t {
public:
    AppIcon_t(const uint16_t* iconBig, const uint16_t* iconSmall)
    {
        this->iconBig   = iconBig;
        this->iconSmall = iconSmall;
    }

    const uint16_t* iconBig;
    const uint16_t* iconSmall;
};

/*
 * mod_settings.c — 设置入口模块
 *
 * 外壳页本身不做设置，长按确认时调用基座 ui_open_settings()，
 * 打开的还是原来那套设置菜单（语义未改）。
 */
#include <stdint.h>

#include "module.h"
#include "ui.h"

static int settings_init(void) { return 0; }

static void settings_tick(uint32_t now_ms) { (void)now_ms; }

static void settings_draw(int x, int y, int w, int h)
{
    (void)h;
    ui_text(UI_F12, x + 4, y + 2, "设置", UI_YELLOW);
    ui_hline(x + 4, y + 18, w - 8, UI_TREND);

    ui_text(UI_F10, x + 4, y + 26, "长按 BOOT 键 → 打开基座设置菜单", UI_LGRAY);
    ui_text(UI_F10, x + 4, y + 44, "菜单里可重新搜索主机 / Xbox 手柄 / 背光", UI_GRAY);
    ui_text(UI_F10, x + 4, y + 62, "短按 下一项   双击 确认   三击 返回", UI_GRAY);
    ui_hline(x + 4, y + 84, w - 8, UI_TREND);
    ui_text(UI_F10, x + 4, y + 92, "镜像页: 短按仍为设置菜单, 长按进外壳", UI_DGRAY);
}

static int settings_on_key(int key, int press_ms)
{
    (void)press_ms;
    if (key == 1) {
        ui_open_settings(); /* 基座原有菜单，逻辑一行未改 */
        return 1;
    }
    return 0;
}

module_t g_mod_settings = {
    .id = "settings",
    .label = "设置",
    .init = settings_init,
    .tick = settings_tick,
    .draw = settings_draw,
    .on_key = settings_on_key,
    .enabled = 0,
};

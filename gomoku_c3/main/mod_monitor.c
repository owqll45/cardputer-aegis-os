/*
 * mod_monitor.c — 基座镜像模块
 *
 * 这个模块本身不画任何东西：镜像页的内容由基座原有的 render_state() 负责。
 * 它存在的意义是让「镜像」成为右栏里一个可选项，并让外壳知道
 * 当前是否处于「基座镜像有效」状态。
 */
#include <stdint.h>

#include "module.h"

static int monitor_init(void) { return 0; }

static void monitor_tick(uint32_t now_ms) { (void)now_ms; }

static void monitor_draw(int x, int y, int w, int h)
{
    /* 故意留空：镜像页由基座 render_state()/draw_status() 画 */
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

static int monitor_on_key(int key, int press_ms)
{
    (void)key;
    (void)press_ms;
    return 0;
}

module_t g_mod_monitor = {
    .id = "monitor",
    .label = "镜像",
    .init = monitor_init,
    .tick = monitor_tick,
    .draw = monitor_draw,
    .on_key = monitor_on_key,
    .enabled = 0,
};

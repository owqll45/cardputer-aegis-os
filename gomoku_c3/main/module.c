/*
 * module.c — 模块表 + 注册/查找/启用判断 + 外壳页
 *
 * 外壳页布局（320x240）：
 *   ┌──────────────────────────────┬────────┐
 *   │ 当前模块内容 264x224          │ 右栏56 │  <- 右栏列 enabled 模块
 *   ├──────────────────────────────┴────────┤
 *   │ 底部 16px 常驻键义提示                 │
 *   └───────────────────────────────────────┘
 *
 * 手动在右栏选过模块后锁 60s，期间不自动跳回镜像页；
 * 锁定期间若主机有画面进来，只在右栏顶部点一个黄点。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "module.h"
#include "bus.h"
#include "ui.h"

static const char *TAG = "mod";

/* 编译期拔模块开关：置 0 即彻底不参与（连注册表都不进） */
#ifndef MOD_ENABLE_MONITOR
#define MOD_ENABLE_MONITOR 1
#endif
#ifndef MOD_ENABLE_MSGS
#define MOD_ENABLE_MSGS 1
#endif
#ifndef MOD_ENABLE_NODES
#define MOD_ENABLE_NODES 1
#endif
#ifndef MOD_ENABLE_LORA
#define MOD_ENABLE_LORA 1
#endif
#ifndef MOD_ENABLE_SETTINGS
#define MOD_ENABLE_SETTINGS 1
#endif
#ifndef MOD_ENABLE_MIRROR
#define MOD_ENABLE_MIRROR 1
#endif

/* 各模块自己导出的实例 */
extern module_t g_mod_monitor;
extern module_t g_mod_msgs;
extern module_t g_mod_nodes;
extern module_t g_mod_lora;
extern module_t g_mod_settings;
extern module_t g_mod_mirror;

/* MODULE_ORDER: monitor(基座镜像) / msgs / nodes / lora / settings */
static module_t *g_modules[] = {
#if MOD_ENABLE_MONITOR
    &g_mod_monitor,
#endif
#if MOD_ENABLE_MSGS
    &g_mod_msgs,
#endif
#if MOD_ENABLE_NODES
    &g_mod_nodes,
#endif
#if MOD_ENABLE_LORA
    &g_mod_lora,
#endif
#if MOD_ENABLE_MIRROR
    &g_mod_mirror,
#endif
#if MOD_ENABLE_SETTINGS
    &g_mod_settings,
#endif
    NULL,
};

#define MOD_LIVE_MAX 8
static module_t *s_live[MOD_LIVE_MAX]; /* 注册成功且 enabled 的模块，右栏顺序 */
static int s_live_n = 0;
static int s_cur = 0;   /* 当前显示的模块（s_live 下标） */
static int s_focus = 0; /* 右栏焦点 */
static uint32_t s_lock_until_ms = 0;
static uint32_t s_last_draw_ms = 0;
static int s_last_fresh = 0;

/* 外壳页布局 */
#define SHELL_LEFT_W   264
#define SHELL_BAR_W     56
#define SHELL_BOTTOM_H  16
#define SHELL_CONTENT_H (UI_H - SHELL_BOTTOM_H)
#define SHELL_ROW_H     39
#define SHELL_ROW_Y0    18
#define SHELL_ROWS      5

static uint32_t mod_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int live_index(const char *id)
{
    for (int i = 0; i < s_live_n; i++)
        if (strcmp(s_live[i]->id, id) == 0) return i;
    return -1;
}

static int lock_active(void)
{
    return s_lock_until_ms && mod_now() < s_lock_until_ms;
}

/* ---------------- 注册表 ---------------- */
void module_registry_init(void)
{
    s_live_n = 0;
    for (int i = 0; g_modules[i] && s_live_n < MOD_LIVE_MAX; i++) {
        module_t *m = g_modules[i];
        m->enabled = 1;
        if (m->init) {
            int rc = m->init();
            if (rc != 0) {
                m->enabled = 0;
                ESP_LOGW(TAG, "module '%s' init failed rc=%d -> disabled", m->id, rc);
            }
        }
        if (m->enabled) {
            s_live[s_live_n++] = m;
            ESP_LOGI(TAG, "module '%s' (%s) ready", m->id, m->label);
        } else {
            ESP_LOGW(TAG, "module '%s' disabled", m->id);
        }
    }
    s_cur = 0;
    s_focus = 0;
    s_lock_until_ms = 0;
    ESP_LOGI(TAG, "registry: %d/%d modules enabled", s_live_n, module_count());
}

int module_count(void)
{
    int n = 0;
    for (int i = 0; g_modules[i]; i++) n++;
    return n;
}

module_t *module_at(int i)
{
    if (i < 0 || i >= module_count()) return NULL;
    return g_modules[i];
}

module_t *module_find(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; g_modules[i]; i++)
        if (strcmp(g_modules[i]->id, id) == 0) return g_modules[i];
    return NULL;
}

int module_enabled_count(void)
{
    return s_live_n;
}

/* ---------------- 渲染分流 ---------------- */
int module_mirror_wanted(uint32_t now_ms, int frame_fresh)
{
    int mi;

    s_last_fresh = frame_fresh;
    if (!frame_fresh) return 0; /* 无帧/超时/映射不了 -> 外壳页 */

    mi = live_index("monitor");
    if (mi >= 0 && s_cur == mi) return 1;

    if (now_ms < s_lock_until_ms) return 0; /* 手选锁定中，不自动跳回镜像页 */

    /* 锁定结束：自动回镜像页（monitor 被拔掉也回基座镜像） */
    if (mi >= 0) {
        s_cur = mi;
        s_focus = mi;
    }
    return 1;
}

int module_shell_active(void)
{
    return !module_mirror_wanted(mod_now(), s_last_fresh);
}

int module_shell_hold(void)
{
    return lock_active();
}

void module_shell_enter(void)
{
    int mi = live_index("monitor");
    if (s_live_n > 0) {
        int pick = -1;
        for (int i = 0; i < s_live_n; i++)
            if (i != mi) { pick = i; break; }
        if (pick >= 0) {
            s_cur = pick;
            s_focus = pick;
        }
    }
    s_lock_until_ms = mod_now() + 60000;
    module_shell_draw();
}

void module_key(int key, int press_ms)
{
    if (s_live_n <= 0) return;

    if (key == 0) {
        /* 短按：右栏焦点下移 */
        s_focus = (s_focus + 1) % s_live_n;
        module_shell_draw();
        return;
    }

    /* 长按：确认进入该模块，并锁 60s */
    s_cur = s_focus;
    s_lock_until_ms = mod_now() + 60000;
    module_t *m = s_live[s_cur];
    int consumed = 0;
    if (m && m->on_key) consumed = m->on_key(1, press_ms);
    ESP_LOGI(TAG, "enter module '%s' (lock 60s)", m ? m->id : "?");
    /* 模块自己吃掉按键（例如设置模块打开了基座菜单）就不要再盖一层外壳 */
    if (!consumed) ui_present_now();
}

void module_loop(uint32_t now_ms, int frame_fresh)
{
    bus_ev_t ev;
    int mirror;

    /* 队列只负责清空；模块各自按 seq 从历史缓冲追读，互不抢事件 */
    while (bus_poll(&ev, now_ms)) {
    }

    for (int i = 0; i < s_live_n; i++)
        if (s_live[i]->tick) s_live[i]->tick(now_ms);

    mirror = module_mirror_wanted(now_ms, frame_fresh);
    if (!mirror && now_ms - s_last_draw_ms >= 250) module_shell_draw();
}

/* ---------------- 外壳页绘制 ---------------- */
static ui_color_t mod_color(const char *id)
{
    if (strcmp(id, "monitor") == 0) return UI_CYAN;
    if (strcmp(id, "msgs") == 0) return UI_GREEN;
    if (strcmp(id, "nodes") == 0) return UI_BLUE;
    if (strcmp(id, "lora") == 0) return UI_ORANGE;
    if (strcmp(id, "mirror") == 0) return UI_MAGENTA;
    if (strcmp(id, "settings") == 0) return UI_YELLOW;
    return UI_GRAY;
}

/* 取 UTF-8 首个字符（右栏图标里显示一个字） */
static void first_cp(const char *s, char *out, int cap)
{
    int n = 0;
    out[0] = 0;
    if (!s || !*s || cap < 2) return;
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) n = 1;
    else if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, (size_t)n);
    out[n] = 0;
}

void module_shell_draw(void)
{
    int dot;

    if (s_live_n > 0) {
        if (s_cur < 0 || s_cur >= s_live_n) s_cur = 0;
        if (s_focus < 0 || s_focus >= s_live_n) s_focus = s_cur;
    }

    ui_clip_reset();
    ui_clear(UI_BG);

    /* 左区：当前模块只画自己的矩形 */
    ui_clip(0, 0, SHELL_LEFT_W, SHELL_CONTENT_H);
    if (s_live_n == 0) {
        ui_text_cx(UI_F12, 0, SHELL_LEFT_W, 100, "无模块", UI_DGRAY);
    } else {
        module_t *m = s_live[s_cur];
        if (m->draw) m->draw(0, 0, SHELL_LEFT_W, SHELL_CONTENT_H);
        /* 镜像页没有画面数据时，外壳给个占位（monitor 模块本身不画东西） */
        if (strcmp(m->id, "monitor") == 0 && !s_last_fresh) {
            ui_text_cx(UI_F12, 0, SHELL_LEFT_W, 96, "等待 Cardputer 画面", UI_GRAY);
            ui_text_cx(UI_F10, 0, SHELL_LEFT_W, 116, "画面到达后自动回到镜像页", UI_DGRAY);
        }
    }
    ui_clip_reset();

    /* 右栏：enabled 模块列表 */
    ui_fill(SHELL_LEFT_W, 0, SHELL_BAR_W, SHELL_CONTENT_H, UI_PANEL);
    ui_vline(SHELL_LEFT_W, 0, SHELL_CONTENT_H, UI_TREND);

    /* 顶部小点：锁定期间有新画面 */
    dot = (s_last_fresh && lock_active()) ? 1 : 0;
    if (dot) ui_disc(SHELL_LEFT_W + SHELL_BAR_W - 11, 9, 3, UI_YELLOW);
    else if (s_live_n) ui_disc(SHELL_LEFT_W + SHELL_BAR_W - 11, 9, 2, UI_TMID);

    if (s_live_n == 0) {
        ui_text_cx(UI_F10, SHELL_LEFT_W, SHELL_BAR_W, 100, "无模块", UI_DGRAY);
    }

    for (int i = 0; i < s_live_n && i < SHELL_ROWS; i++) {
        int y = SHELL_ROW_Y0 + i * SHELL_ROW_H;
        int sel = (i == s_focus);
        char cp[8];

        if (sel) {
            ui_fill(SHELL_LEFT_W + 1, y, SHELL_BAR_W - 1, SHELL_ROW_H - 2, UI_SELBG);
            ui_fill(SHELL_LEFT_W + 1, y, 3, SHELL_ROW_H - 2, UI_YELLOW);
        }
        ui_fill(SHELL_LEFT_W + 20, y + 3, 16, 16, mod_color(s_live[i]->id));
        first_cp(s_live[i]->label, cp, sizeof(cp));
        ui_text_cx(UI_F10, SHELL_LEFT_W + 20, 16, y + 6, cp, UI_BLACK);
        ui_text_cx(UI_F10, SHELL_LEFT_W, SHELL_BAR_W, y + 22, s_live[i]->label,
                   sel ? UI_YELLOW : UI_LGRAY);
    }

    /* 底部常驻键义提示 */
    ui_fill(0, SHELL_CONTENT_H, UI_W, SHELL_BOTTOM_H, UI_BLACK);
    ui_hline(0, SHELL_CONTENT_H, UI_W, UI_TREND);
    ui_text_cx(UI_F10, 0, UI_W, SHELL_CONTENT_H + 3, "短按 移动  长按 确认", UI_LGRAY);

    ui_push();
    s_last_draw_ms = mod_now();
}

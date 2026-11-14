/*
 * mod_nodes.c — 节点列表
 *
 * 数据来自 bus 的 EV_NODE_UP：节点号 / 短名 / 跳数 / GPS 能力。
 */
#include <stdio.h>
#include <string.h>

#include "module.h"
#include "bus.h"
#include "ui.h"

#define NODE_N 8

typedef struct {
    uint32_t from;
    uint8_t  hops;
    uint8_t  gps;
    char     name[14];
} node_t;

static node_t s_node[NODE_N]; /* 0=最近上线的排前面会移动，这里按首次出现顺序 */
static int    s_n;
static uint32_t s_seq;
static uint32_t s_evt;

static void node_up(uint32_t from, uint8_t hops, uint8_t gps, const char* name)
{
    s_evt++;
    for (int i = 0; i < s_n; i++) {
        if (s_node[i].from == from) {
            s_node[i].hops = hops;
            s_node[i].gps = gps;
            if (name && name[0]) snprintf(s_node[i].name, sizeof(s_node[i].name), "%s", name);
            return;
        }
    }
    if (s_n >= NODE_N) return;
    s_node[s_n].from = from;
    s_node[s_n].hops = hops;
    s_node[s_n].gps = gps;
    snprintf(s_node[s_n].name, sizeof(s_node[s_n].name), "%s",
             (name && name[0]) ? name : "----");
    s_n++;
}

static int nodes_init(void)
{
    s_seq = bus_seq();
    return 0;
}

static void nodes_tick(uint32_t now_ms)
{
    bus_ev_t b;
    uint32_t seq = bus_seq();
    (void)now_ms;

    while (s_seq < seq) {
        s_seq++;
        if (!bus_history_get(s_seq, &b)) continue;
        if (b.type == EV_NODE_UP)
            node_up(b.a, (uint8_t)((b.b >> 8) & 0xFF), (uint8_t)(b.b & 0xFF), b.text);
    }
}

static void nodes_draw(int x, int y, int w, int h)
{
    char buf[48];

    ui_text(UI_F12, x + 4, y + 2, "节点", UI_BLUE);
    snprintf(buf, sizeof(buf), "%d 个", s_n);
    ui_text(UI_F10, x + w - 4 - ui_text_w(UI_F10, buf), y + 3, buf, UI_LGRAY);
    ui_hline(x + 4, y + 17, w - 8, UI_TREND);

    if (s_n == 0) {
        ui_text(UI_F10, x + 4, y + 26, "暂无节点", UI_DGRAY);
        ui_text(UI_F10, x + 4, y + 40, "收到 NodeInfo 后列出最近 8 个", UI_DGRAY);
        return;
    }

    int yy = y + 22;
    for (int i = 0; i < s_n && yy + 18 <= y + h; i++) {
        const char* gps = s_node[i].gps == 1 ? "无GPS" : (s_node[i].gps == 2 ? "有GPS" : "--");
        snprintf(buf, sizeof(buf), "!%04lx  %s", (unsigned long)(s_node[i].from & 0xFFFF),
                 s_node[i].name);
        ui_text(UI_F10, x + 4, yy, buf, UI_WHITE);
        snprintf(buf, sizeof(buf), "%u 跳  %s", (unsigned)s_node[i].hops, gps);
        ui_text(UI_F10, x + w - 4 - ui_text_w(UI_F10, buf), yy, buf, UI_GRAY);
        ui_hline(x + 4, yy + 13, w - 8, UI_TMID);
        yy += 20;
    }
}

static int nodes_on_key(int key, int press_ms)
{
    (void)key;
    (void)press_ms;
    return 0;
}

module_t g_mod_nodes = {
    .id = "nodes",
    .label = "节点",
    .init = nodes_init,
    .tick = nodes_tick,
    .draw = nodes_draw,
    .on_key = nodes_on_key,
    .enabled = 0,
};

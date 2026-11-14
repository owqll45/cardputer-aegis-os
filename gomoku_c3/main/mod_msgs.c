/*
 * mod_msgs.c — 消息列表
 *
 * 数据全部来自 bus（EV_LORA_TEXT），与 LoRa 页各自追读同一份历史，
 * 互不抢占事件。
 */
#include <stdio.h>
#include <string.h>

#include "module.h"
#include "bus.h"
#include "ui.h"

#define MSG_N      7
#define NAME_N     12

typedef struct {
    uint32_t from;
    int8_t   rssi;
    char     text[52];
} mmsg_t;

static mmsg_t  s_msg[MSG_N]; /* 0=最新 */
static int     s_msg_n;
static uint32_t s_node[NAME_N];
static char    s_name[NAME_N][12];
static int     s_name_n;
static uint32_t s_seq;
static uint32_t s_total;

static const char* sender(uint32_t from)
{
    static char hex[12];
    for (int i = 0; i < s_name_n; i++)
        if (s_node[i] == from) return s_name[i];
    snprintf(hex, sizeof(hex), "!%04lx", (unsigned long)(from & 0xFFFF));
    return hex;
}

static void learn_name(uint32_t from, const char* name)
{
    if (!name || !name[0]) return;
    for (int i = 0; i < s_name_n; i++) {
        if (s_node[i] == from) {
            snprintf(s_name[i], sizeof(s_name[i]), "%s", name);
            return;
        }
    }
    if (s_name_n >= NAME_N) return;
    s_node[s_name_n] = from;
    snprintf(s_name[s_name_n], sizeof(s_name[0]), "%s", name);
    s_name_n++;
}

static void msg_push(uint32_t from, int8_t rssi, const char* txt)
{
    for (int i = MSG_N - 1; i > 0; i--) s_msg[i] = s_msg[i - 1];
    if (s_msg_n < MSG_N) s_msg_n++;
    s_msg[0].from = from;
    s_msg[0].rssi = rssi;
    snprintf(s_msg[0].text, sizeof(s_msg[0].text), "%s", txt ? txt : "");
    s_total++;
}

static int msgs_init(void)
{
    s_seq = bus_seq();
    return 0;
}

static void msgs_tick(uint32_t now_ms)
{
    bus_ev_t b;
    uint32_t seq = bus_seq();
    (void)now_ms;

    while (s_seq < seq) {
        s_seq++;
        if (!bus_history_get(s_seq, &b)) continue;
        if (b.type == EV_LORA_TEXT)
            msg_push(b.b, (int8_t)(b.a & 0xFF), b.text);
        else if (b.type == EV_NODE_UP)
            learn_name(b.a, b.text);
    }
}

static void msgs_draw(int x, int y, int w, int h)
{
    char buf[64];

    ui_text(UI_F12, x + 4, y + 2, "消息", UI_GREEN);
    snprintf(buf, sizeof(buf), "共 %lu 条", (unsigned long)s_total);
    ui_text(UI_F10, x + w - 4 - ui_text_w(UI_F10, buf), y + 3, buf, UI_LGRAY);
    ui_hline(x + 4, y + 17, w - 8, UI_TREND);

    if (s_msg_n == 0) {
        ui_text(UI_F10, x + 4, y + 26, "暂无消息", UI_DGRAY);
        ui_text(UI_F10, x + 4, y + 40, "收到 LoRa 文本后显示最近 7 条", UI_DGRAY);
        return;
    }

    int yy = y + 22;
    for (int i = 0; i < s_msg_n && yy + 20 <= y + h; i++) {
        snprintf(buf, sizeof(buf), "%s  %d dB", sender(s_msg[i].from), s_msg[i].rssi);
        ui_text(UI_F10, x + 4, yy, buf, UI_GRAY);
        ui_text(UI_F10, x + 4, yy + 11, s_msg[i].text, UI_WHITE);
        ui_hline(x + 4, yy + 22, w - 8, UI_TMID);
        yy += 28;
    }
}

static int msgs_on_key(int key, int press_ms)
{
    (void)key;
    (void)press_ms;
    return 0;
}

module_t g_mod_msgs = {
    .id = "msgs",
    .label = "消息",
    .init = msgs_init,
    .tick = msgs_tick,
    .draw = msgs_draw,
    .on_key = msgs_on_key,
    .enabled = 0,
};

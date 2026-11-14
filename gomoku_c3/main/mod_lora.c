/*
 * mod_lora.c — LoRa 页
 *
 * 数据全部来自 bus（lora.c 把收到的 TEXT/NODE 事件 publish 出来）：
 *   最近 6 条文本 + RSSI/SNR + 节点数 + 调制档位/收发状态。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_mac.h"

#include "module.h"
#include "bus.h"
#include "ui.h"
#include "lora.h"

#define LORA_MSG_N  6
#define LORA_NODE_N 12

typedef struct {
    uint32_t from;
    int8_t   rssi;
    int8_t   snr4;
    char     text[48];
} lmsg_t;

static lmsg_t  s_msg[LORA_MSG_N]; /* 0=最新 */
static int     s_msg_n;
static uint32_t s_node[LORA_NODE_N];
static char    s_node_name[LORA_NODE_N][12];
static int     s_node_n;
static int8_t  s_rssi = -127;
static int8_t  s_snr4 = 0;
static uint32_t s_seq;
static int     s_tx_ok = -1;

static void node_add(uint32_t from, const char* name)
{
    for (int i = 0; i < s_node_n; i++) {
        if (s_node[i] == from) {
            if (name && name[0]) snprintf(s_node_name[i], sizeof(s_node_name[i]), "%s", name);
            return;
        }
    }
    if (s_node_n >= LORA_NODE_N) return;
    s_node[s_node_n] = from;
    snprintf(s_node_name[s_node_n], sizeof(s_node_name[0]), "%s",
             (name && name[0]) ? name : "----");
    s_node_n++;
}

static const char* node_name(uint32_t from)
{
    static char hex[12];
    for (int i = 0; i < s_node_n; i++)
        if (s_node[i] == from) return s_node_name[i];
    snprintf(hex, sizeof(hex), "!%04lx", (unsigned long)(from & 0xFFFF));
    return hex;
}

static void msg_push(uint32_t from, int8_t rssi, int8_t snr4, const char* txt)
{
    for (int i = LORA_MSG_N - 1; i > 0; i--) s_msg[i] = s_msg[i - 1];
    if (s_msg_n < LORA_MSG_N) s_msg_n++;
    s_msg[0].from = from;
    s_msg[0].rssi = rssi;
    s_msg[0].snr4 = snr4;
    snprintf(s_msg[0].text, sizeof(s_msg[0].text), "%s", txt ? txt : "");
}

static int lora_mod_init(void)
{
    uint8_t mac[6] = {0};
    uint32_t node;

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    node = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
           ((uint32_t)mac[4] << 8) | mac[5];
    lora_init(node); /* SPI2 由基座 app_main 先初始化好 */
    s_tx_ok = lora_tx_allowed() ? 1 : 0;
    s_seq = bus_seq(); /* 只看启动之后的事件 */
    return 0;
}

static void lora_mod_tick(uint32_t now_ms)
{
    bus_ev_t b;
    uint32_t seq;
    lora_evt_t e;
    (void)now_ms;

    /* 驱动内部队列只用于兼容旧接口，事件已经同步到总线，这里清空防溢出 */
    while (lora_evt_pop(&e)) {
    }

    seq = bus_seq();
    while (s_seq < seq) {
        s_seq++;
        if (!bus_history_get(s_seq, &b)) continue;
        if (b.type == EV_LORA_TEXT) {
            int8_t rssi = (int8_t)(b.a & 0xFF);
            int8_t snr4 = (int8_t)((b.a >> 8) & 0xFF);
            s_rssi = rssi;
            s_snr4 = snr4;
            msg_push(b.b, rssi, snr4, b.text);
        } else if (b.type == EV_NODE_UP) {
            node_add(b.a, b.text);
        }
    }
}

static void fmt_snr(char* o, size_t cap, int8_t snr4)
{
    int v = (int)snr4 * 10 / 4; /* 1/4 dB -> 0.1 dB */
    snprintf(o, cap, "%s%d.%d", v < 0 ? "-" : "", abs(v) / 10, abs(v) % 10);
}

static void lora_mod_draw(int x, int y, int w, int h)
{
    char buf[64], snr[12];
    int yy;

    ui_text(UI_F12, x + 4, y + 2, "LoRa 433", UI_ORANGE);
    snprintf(buf, sizeof(buf), "节点 %d", s_node_n);
    ui_text(UI_F10, x + w - 4 - ui_text_w(UI_F10, buf), y + 3, buf, UI_LGRAY);
    ui_hline(x + 4, y + 17, w - 8, UI_TREND);

    if (s_rssi <= -127) {
        snprintf(buf, sizeof(buf), "RSSI --    SNR --");
    } else {
        fmt_snr(snr, sizeof(snr), s_snr4);
        snprintf(buf, sizeof(buf), "RSSI %d dBm   SNR %s", s_rssi, snr);
    }
    ui_text(UI_F10, x + 4, y + 21, buf, UI_CYAN);

    snprintf(buf, sizeof(buf), "SF11 LONG_FAST   %s", s_tx_ok == 1 ? "TX 开" : "只收");
    ui_text(UI_F10, x + 4, y + 33, buf, UI_DGRAY);
    ui_hline(x + 4, y + 46, w - 8, UI_TREND);

    if (s_msg_n == 0) {
        ui_text(UI_F10, x + 4, y + 54, "暂无 LoRa 文本", UI_DGRAY);
        ui_text(UI_F10, x + 4, y + 68, "长按右栏确认后本页保持刷新", UI_DGRAY);
        return;
    }

    yy = y + 52;
    for (int i = 0; i < s_msg_n && yy + 22 <= y + h; i++) {
        fmt_snr(snr, sizeof(snr), s_msg[i].snr4);
        snprintf(buf, sizeof(buf), "%s  %d dB  %s", node_name(s_msg[i].from),
                 s_msg[i].rssi, snr);
        ui_text(UI_F10, x + 4, yy, buf, UI_GRAY);
        ui_text(UI_F10, x + 4, yy + 11, s_msg[i].text, UI_WHITE);
        yy += 27;
    }
}

static int lora_mod_on_key(int key, int press_ms)
{
    (void)key;
    (void)press_ms;
    return 0;
}

module_t g_mod_lora = {
    .id = "lora",
    .label = "LoRa",
    .init = lora_mod_init,
    .tick = lora_mod_tick,
    .draw = lora_mod_draw,
    .on_key = lora_mod_on_key,
    .enabled = 0,
};

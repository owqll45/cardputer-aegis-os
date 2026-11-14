/*
 * bus.h — 极简事件总线
 *
 * 模块之间不直接调用：产生事件的一方只管 bus_publish()，
 * 需要数据的一方从队列或历史环形缓冲里取。
 *
 * 队列 16 条（满则丢最旧），供主循环 bus_poll() 取用；
 * 另有一个 48 条的只读历史环形缓冲，多个模块可以各自按 seq 追读，
 * 互相不抢事件（mod_lora / mod_msgs / mod_nodes 都靠它）。
 *
 * 字段约定：
 *   EV_LORA_TEXT  a = rssi(低8位有符号) | snr4<<8      b = from 节点号   text = 文本
 *   EV_NODE_UP    a = from 节点号                      b = hops<<8 | gps text = 短名
 *   EV_FRAME_RX   a = 游戏 id                          b = 保留
 *   EV_KEY        a = 0 短按 / 1 长按                   b = 按压时长 ms
 *   EV_TIME       a = utc 秒                            b = 来源(0无 1GPS 2手动)
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_NONE = 0,
    EV_LORA_TEXT,
    EV_NODE_UP,
    EV_FRAME_RX,
    EV_KEY,
    EV_TIME,
} bus_ev_type_t;

typedef struct {
    bus_ev_type_t type;
    uint32_t a;
    uint32_t b;
    char text[96];
} bus_ev_t;

#define BUS_QUEUE_LEN 16
#define BUS_HIST_LEN  48

void bus_publish(const bus_ev_t* ev);          /* 投递到队列 + 历史 */
int  bus_poll(bus_ev_t* out, uint32_t now_ms); /* 主循环取，非阻塞，1=取到 */

/* 历史环形缓冲：seq 从 1 开始单调递增 */
uint32_t bus_seq(void);
int      bus_history_get(uint32_t seq, bus_ev_t* out); /* 1=该序号仍在缓冲里 */

#ifdef __cplusplus
}
#endif

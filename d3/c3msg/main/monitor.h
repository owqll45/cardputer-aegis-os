#pragma once

#include <stdint.h>
#include "display.h"

/*
 * 五子棋监视器（GMK 镜像）—— 与 LoRa 消息/节点页共存的第二套界面。
 * 端口自 zd/gomoku_c3：GMKBEAC/GMKHI1/GMKWELC/GMKAPP/GMKSTATE(0x20) 协议，
 * 画面由本机按状态包本地重绘（与主机端 320x240 布局一致）。
 */

/* 单播发送：由 main.c 提供，内部按需注册 ESP-NOW peer */
typedef void (*mon_send_fn)(const uint8_t mac[6], const uint8_t *data, int len);

/* 开机调用一次；send 可为 NULL（只收不发） */
void monitor_init(mon_send_fn send);

/* ESP-NOW 收包回调里把 GMK* 帧转进来（任意任务上下文，内部只做拷贝解析） */
void monitor_on_frame(const uint8_t src_mac[6], const uint8_t *data, int len, int rssi);

/* 画整页到 display 帧缓冲（不调用 display_flush） */
void monitor_draw(void);

/* 统计：收到多少 GMK 帧 + 当前主机 app（现场排查链路用） */
void monitor_get_stats(int *beac, int *state, int *app, int *gid, char *name, int cap);

/* 当前屏是否支持监视器页（320x240 大屏） */
int monitor_supported(void);

/* 文本绘制：复用 main.c 的 12px 字库实现 */
void ui_text(int x, int y, const char *s, color_t fg, int maxw);
int  ui_text_w(const char *s);

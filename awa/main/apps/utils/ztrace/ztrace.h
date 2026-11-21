#pragma once
/*
 * 全链路日志（自测/对账用）
 *
 * 统一前缀 ZDTRC，行内固定 "ev=<步骤> k=v ..."：
 *   rx_msg  via= from= msgid= seq= rssi= fg= heap= lat= text=
 *   ntf     mode=full|simple|skip unread= heap= dt=
 *   jump    key== fg_before= opened= dt=
 *   panel   ev=panel_open located= msgid=
 *   tx      msgid= seq= via= len= fg=
 *   ack     msgid= ok= dt=
 *   nack    msgid= reason=
 *   flow    step= result=PASS|FAIL why= ...
 *   ledger  rx_text= rx_lora= dup= bad= notice= shown= skip= ...
 *
 * 这些行只用于串口取证，别在热路径里逐帧打。
 */
#include <esp_log.h>

#define ZT_TAG "ZDTRC"
#define ZT(step, fmt, ...) ESP_LOGI(ZT_TAG, "ev=" step " " fmt, ##__VA_ARGS__)

/* main.cpp: 当前前台的 LoRa 之外的 app 名（没有就返回 "Launcher"） */
const char* zd_fg_app();

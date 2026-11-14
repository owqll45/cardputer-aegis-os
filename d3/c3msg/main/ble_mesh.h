/*
 * ble_mesh.h — 官方 Meshtastic App 的 BLE 接口
 *
 * 对齐官方实现（src/nimble/NimbleBluetooth.cpp + PhoneAPI.cpp）：
 *   服务 6ba1b218-15a8-461f-9fa8-5dcae273eafd
 *   ToRadio  f75c76d2-129e-4dad-a1dd-7866124401e7  (write)
 *   FromRadio 2c55e69e-4993-11ed-b878-0242ac120002 (read，一次读一条)
 *   FromNum  ed9da18c-a800-4f66-a670-aa7547e34453 (notify，4B LE 计数)
 * 收到 ToRadio.want_config_id 后按官方顺序推：my_info -> deviceui -> 本机 nodeinfo
 *   -> metadata -> channel -> config -> 其他节点 -> config_complete_id
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    char     long_name[24];
    char     short_name[12];
    int8_t   rssi;
    int8_t   snr4;
    uint8_t  hops;
    uint32_t age_s;
} ble_node_t;

/* long_name/short_name 为本机在 LoRa 网里的名字 */
void ble_mesh_init(uint32_t my_node, const char *long_name, const char *short_name);

/* main 任务在节点表变化时调用，BLE 配置同步时用它生成 node_info 列表 */
void ble_mesh_set_nodes(const ble_node_t *nodes, int n);

/* LoRa 收到文本时调用：入 FromRadio 队列并 notify */
void ble_mesh_on_lora_text(uint32_t from, const char *text, int8_t rssi, int8_t snr4, uint8_t hops);

bool ble_mesh_connected(void);

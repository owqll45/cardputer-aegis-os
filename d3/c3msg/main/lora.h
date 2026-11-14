/*
 * lora.h — Meshtastic 兼容层（接收优先）
 *
 * 与 main.c 的约定：
 *   - SPI2 总线由 main.c 初始化（SCLK=10 MOSI=7 MISO=6），lora_init() 只挂一个 1MHz 设备
 *   - 收到的东西不直接改 UI 数据，而是塞进队列；main 任务负责 pop 并更新界面
 *   - 不碰 GPIO3(DIO1) 的配置：完成判据只看 IRQ 寄存器
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    LORA_EVT_TEXT = 1,      /* 频道文本（Meshtastic TEXT_MESSAGE_APP） */
    LORA_EVT_NODE = 2,      /* 节点信息（Meshtastic NODEINFO_APP） */
    LORA_EVT_TXDONE = 3,    /* 文本发送结束（供 ZD 桥回 ACK/LNACK） */
    LORA_EVT_TIME = 4,      /* LoRa 时间广播（私有 port 0x0101） */
};

typedef struct {
    uint8_t  kind;
    uint8_t  hops;          /* hop_start - hop_limit */
    uint8_t  gps;           /* 0=未知 1=无 GPS 2=有 GPS（来自 NodeInfo.User.field15） */
    int8_t   rssi;          /* dBm */
    int8_t   snr4;          /* SNR * 4（四分之一 dB） */
    uint32_t from;          /* Meshtastic NodeNum */
    uint16_t pkt_lo;        /* TEXT: 空口包 id 低 16 位；TXDONE: 调用方 tag */
    uint8_t  tx_ok;         /* TXDONE: 1=空口发送完成, 0=失败/超时 */
    uint8_t  tsrc;          /* TIME: 0=未同步 1=GPS 2=手动 */
    uint32_t utc;           /* TIME: UTC 秒 */
    char     short_name[12];
    char     long_name[24];
    char     text[97];
} lora_evt_t;

/* my_node = 本机 NodeNum（由 MAC 后 4 字节推导，与 main 的 s_my_id 一致） */
void lora_init(uint32_t my_node);

/* 非阻塞取一条事件；有返回 true */
bool lora_evt_pop(lora_evt_t *out);

/* 当前是否允许 LoRa 发射（AA:D0 无天线时恒为 false；UI 与桥接层必须先查） */
bool lora_tx_allowed(void);

/* 以 Meshtastic 文本包发到 LoRa（to=0xFFFFFFFF 为广播）。
 * 返回 0=已入队，-1=被安全策略拒绝，-2=队列满。实际发射在 lora 任务里串行执行。
 * tag 会在 LORA_EVT_TXDONE 事件里原样带回（0 表示不关心，桥接层用非 0）。 */
int lora_send_text(uint32_t to, const char *text);
int lora_send_text_tagged(uint32_t to, const char *text, uint16_t tag);

/* 时间广播到 LoRa（广播地址，私有 port 0x0101）：utc=UTC 秒，src 0=未同步 1=GPS 2=手动 */
int lora_send_time(uint32_t utc, uint8_t src);

/* GMK 镜像兜底帧（私有 port 0x0102）。只走单槽镜像队列：
 * 主队列（文本/时间）非空或正在发射时返回 -2（让路），不排队。 */
int lora_send_mirror(const uint8_t *pay, int n);

/* 1=空口空闲（没有正在发射的帧、主队列和镜像槽都空） */
int lora_tx_idle(void);

/* 发射功率（dBm）；无天线设备会被拒绝并保持 -9dBm */
void lora_set_tx_power(int dbm);
int  lora_tx_power_dbm(void);

/* 频道参数（供 BLE 的 Channel 配置同步使用） */
void lora_get_psk(uint8_t out[32]);
const char *lora_channel_name(void);

/* 调制档位：0=LONG_FAST（默认，SF11/BW250）1=LONG_SLOW（SF12/BW125）
 * 三台必须一致，否则互相收不到。改档会立即重配空口，不需要重启。 */
int  lora_preset_count(void);
int  lora_preset_idx(void);
void lora_set_preset(int idx);
const char *lora_preset_name(void);

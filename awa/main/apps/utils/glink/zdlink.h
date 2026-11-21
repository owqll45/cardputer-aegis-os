/*
 * zdlink — ZD v1 消息协议(C3 侧 c3msg M1 的权威实现见 d3/c3msg/main/main.c)
 *
 * 与 glink(GMK 游戏镜像) 共用同一个 ESP-NOW 实例与 channel 1:
 *   - 收包: glink 的 recv 回调按 magic 分流, 'Z','D' 交给本模块, 'GMK*' 仍走原逻辑
 *   - 发包: 直接用 glink 已注册的广播 peer(FF:FF:FF:FF:FF:FF)
 * 帧: 24B 头 + payload<=226, 小端, CRC16-CCITT(0x1021, init 0xFFFF) 覆盖 [0..21]+payload
 *     type: 1=GAME 2=TEXT 3=BEAT 4=NODETAB 5=ACK 6=LTEXT 7=LNACK(v1.1)
 *     flags bit0=WANT_ACK bit1=FORWARDED bit2=LORA(v1.1)
 *     src/dst 为节点 ID(MAC 低 4 字节), dst 广播 0xFFFFFFFF, 单播=目标 NodeNum, ttl 固定 3
 * 去重: 每源 32 条滑动窗口, 最多跟踪 8 个源
 *
 * 线程模型: zdlink_on_frame 运行在 WiFi 任务里, 只解析/入队, 绝不绘制或阻塞;
 *           zdlink_poll / zdlink_send_text 在主任务里调用。
 */
#pragma once
#include <stdint.h>
#include <esp_now.h>

#define ZD_HDR          24
#define ZD_MAX_PAYLOAD  226
#define ZD_VER          1

enum {
    ZT_GAME    = 1,
    ZT_TEXT    = 2,
    ZT_BEAT    = 3,
    ZT_NODETAB = 4,
    ZT_ACK     = 5,
    ZT_LTEXT   = 6,  /* v1.1: LoRa 收到的文本回传给 ADV */
    ZT_LNACK   = 7,  /* v1.1: LoRa 转发失败通知 */
    ZT_TIME    = 8,  /* v1.2: 时间同步(UTC + 来源), ADV 与 LoRa 网双向 */
};

#define ZF_WANT_ACK  0x01
#define ZF_FORWARDED 0x02
#define ZF_LORA      0x04  /* v1.1: 仅 ZT_TEXT, 请求 C3 转发到 LoRa */

/* 给 UI 的收包事件(只放小结构, 大 payload 已截断到 96B, 与 C3 的 MSG_LEN 一致) */
enum {
    ZD_EV_TEXT  = 1,
    ZD_EV_BEAT  = 2,
    ZD_EV_ACK   = 3,
    ZD_EV_LTEXT = 4,  /* 来自 LoRa 空口, via=ZD_VIA_L */
    ZD_EV_LNACK = 5,  /* LoRa 转发失败, reason 见协议 §5.3 */
    ZD_EV_TIME  = 6,  /* v1.2: 时间同步帧(UTC + 来源) */
};

/* ZT_LNACK reason(协议 §5.3) */
enum {
    ZD_LNACK_TX_DISABLED = 1,  /* 无天线安全策略 */
    ZD_LNACK_QUEUE_FULL  = 2,  /* 发送队列满 */
    ZD_LNACK_AIR_TIMEOUT = 3,  /* 空口超时 */
    ZD_LNACK_UNSUPPORTED = 4,  /* 不支持的请求 */
};

/* 消息到达路径(UI 上显示成 E/L/R):
 *   E = ESP-NOW 直连, L = LoRa 空口(v1.1 ZT_LTEXT), R = 本机经 C3 桥转发。 */
enum {
    ZD_VIA_E = 1,
    ZD_VIA_L = 2,
    ZD_VIA_R = 3,
};

struct ZdEvent {
    uint8_t  kind;      /* ZD_EV_* */
    uint8_t  flags;     /* 帧 flags */
    uint8_t  hops;      /* kind=LTEXT: LoRa hops_away(255=未知) */
    uint8_t  via;       /* ZD_VIA_* */
    int8_t   rssi;      /* kind=LTEXT: 空口 RSSI(帧头 RSSI 是 C3 的) */
    int8_t   snr4;      /* kind=LTEXT: SNR x 4 */
    uint32_t src;       /* 对端节点 ID(LTEXT 为 lora_from) */
    uint32_t lora_from; /* kind=LTEXT: LoRa 原始发送方 NodeNum */
    uint16_t ack_seq;   /* kind=ACK/LNACK: 对应的 seq */
    uint16_t ack_msgid; /* kind=ACK/LNACK: 对应的 msgid */
    uint16_t msgid;     /* kind=TEXT/LTEXT: 本帧 msgid(与 C3 对账用) */
    uint16_t seq;       /* kind=TEXT/LTEXT: 本帧序号 */
    uint16_t batt_mv;   /* kind=BEAT */
    uint8_t  batt_pct;  /* kind=BEAT */
    uint8_t  role;      /* kind=BEAT: C3 定义 1=节点/中继 */
    uint8_t  reason;    /* kind=LNACK: ZD_LNACK_* */
    uint8_t  loopback;  /* kind=LTEXT: 1 = 空口文本带本机 [A:xxxx] 前缀(自己发出去的回环) */
    uint32_t utc;       /* kind=TIME: UTC 秒(协议 v1.2 §1.2) */
    uint8_t  tsrc;      /* kind=TIME: 0=未同步 1=GPS 2=手动 */
    uint8_t  tacc;      /* kind=TIME: 精度(秒) */
    char     text[97];  /* kind=TEXT/LTEXT, UTF-8 */
};

struct ZdNodeInfo {
    uint32_t id;
    int8_t   rssi;       /* 最近一帧(通常是 ESP-NOW)的 RSSI */
    uint8_t  hops;
    uint16_t batt_mv;
    uint8_t  batt_pct;
    uint8_t  role;
    uint8_t  espnow;     /* 1 = 在 ESP-NOW 上收到过该节点的帧(ZD 网内设备) */
    int8_t   snr;        /* SNR x 4(协议 §3 的 snr4), 127 = 未知 */
    uint8_t  lora;       /* 1 = 该节点经 LoRa 空口出现过 */
    int8_t   lora_rssi;  /* 最近一次 LoRa 空口的 RSSI */
    uint32_t seen_ms;    /* 最近一次收到该节点帧的时刻 */
};

/* C3 桥(BEAT 里带 caps 的节点, 协议 §6) */
struct ZdBridge {
    uint32_t id;       /* NodeNum, 直接用作 ZF_LORA 单播的 dst */
    uint8_t  caps;     /* bit0=具备桥能力 bit1=当前允许发射 */
    uint32_t seen_ms;
};

/* glink 初始化 ESP-NOW 成功后调用; 也用于 WiFi 被杀后标记不可用 */
void zdlink_init();
void zdlink_set_ready(bool ready);
bool zdlink_ready();

/* 由 glink 的 ESP-NOW 收包回调调用(WiFi 任务上下文) */
void zdlink_on_frame(const esp_now_recv_info_t* info, const uint8_t* data, int len);

/* 主循环里调用: 心跳(BEAT)、补发 ACK、发送超时判定 */
void zdlink_poll();

uint32_t zdlink_my_id();
uint32_t zdlink_now_ms();

/* 发送文本(UTF-8, 自动截断到 226B), 带 WANT_ACK。成功返回 true */
bool zdlink_send_text(const char* text, int len, uint16_t* out_msgid, uint16_t* out_seq);

/* 经 C3 桥转发到 LoRa(协议 §4):
 *   type=ZT_TEXT, flags=ZF_LORA|ZF_WANT_ACK, dst=bridge_id(桥 NodeNum, 不能广播),
 *   ttl=3, frag=0/frags=1, 文本按 72B UTF-8 边界截断。
 * 等待空口 ACK/LNACK 的时间上限为 15s(协议 §5.2)。 */
bool zdlink_send_lora_text(const char* text, int len, uint32_t bridge_id, uint16_t* out_msgid,
                           uint16_t* out_seq);

/* 最近一条文本发送状态: 0=等待 ACK 1=已确认 2=超时/失败 3=未知 msgid */
uint8_t zdlink_tx_state(uint16_t msgid);

/* 失败原因(LNACK reason, 协议 §5.3); 0 = 没有 LNACK 记录 */
uint8_t zdlink_tx_reason(uint16_t msgid);

/* 广播时间(协议 v1.2 §1.2 / §1.3): type=ZT_TIME, flags=0, dst=广播, msgid=0,
 * payload 6B = utc32 LE + 来源 + 精度秒。src=0 或 utc=0 不发。 */
bool zdlink_send_time(uint32_t utc, uint8_t src, uint8_t acc);

/* 收到文本事件(TEXT/LTEXT)时的只读钩子: 跑在 WiFi 接收任务里,
 * 只允许拷贝数据, 不能绘制/阻塞。用于全局通知层, 不影响事件队列。 */
using zdlink_event_hook_t = void (*)(const ZdEvent& ev);
void zdlink_set_event_hook(zdlink_event_hook_t cb);

/* 桥表快照(近 30s 内发过 BEAT 且带 caps 的 C3), 按最近活跃排序 */
int zdlink_get_bridges(ZdBridge* out, int max);

/* 就近选桥: 只在 30s 内有效、具备桥能力(caps.bit0)的桥里挑,
 * 依次按 可发射(bit1) > 新鲜度 > 节点表 RSSI/hops 打分;
 * prefer 指定的桥只在仍然新鲜时加分, 陈旧/禁发会自动回落到更近的桥。
 * caps_out 回传选中桥的 caps[0](0 表示没有可用桥)。 */
uint32_t zdlink_pick_bridge(uint32_t prefer, uint8_t* caps_out);

/* 是否参与中继转发(本地配置, 存 NVS 的 "zd_relay"; 等 LoRa 扩展说明到位后由转发逻辑使用) */
void zdlink_set_relay(bool on);
bool zdlink_relay_enabled();

/* 取事件(主任务调用), 无事件返回 false */
bool zdlink_pop_event(ZdEvent& out);

/* 节点表快照(含本机), 返回写入条数 */
int zdlink_get_nodes(ZdNodeInfo* out, int max);

/* 诊断计数 */
void zdlink_stats(uint32_t* rx_ok, uint32_t* rx_bad, uint32_t* tx, uint32_t* ack_rx);

/* 文本类计数(用于通知层对账): text=ESP-NOW 文本帧, lora=经桥空口文本帧,
 * dup=被去重窗口丢掉的帧, loop=空口回环(本机发出的) */
void zdlink_text_stats(uint32_t* text, uint32_t* lora, uint32_t* dup, uint32_t* loop);

/* 桌面实时面板用的链路快照(全部取自真实收发打点, 不做估算)。
 * link: ZD_VIA_E / ZD_VIA_L / 0(无活动); snr4=127 表示该链路无 SNR。 */
struct ZdLinkInfo {
    uint8_t  link;
    int8_t   rssi;
    int8_t   snr4;
    uint32_t last_rx_ms;  /* 最近一次收到有效 ZD 帧(0=从未) */
    uint32_t last_tx_ms;  /* 最近一次成功发出 ZD 帧(0=从未) */
    uint32_t ack_rtt_ms;  /* 最近一次 ACK 往返(0=还没收到过) */
    uint8_t  nodes;       /* 60s 内在线的节点数 */
    uint8_t  bridges;     /* 30s 内有效的桥数 */
};
int zdlink_link_info(ZdLinkInfo* out);

/* 当前正在使用的链路(状态栏用): 返回 ZD_VIA_E / ZD_VIA_L, 都没有活动返回 0。
 * window_ms 内(0 取 5s)有收发活动才算"在用"; 两路都活跃时取更近的那次。
 * 只读, 可任意任务调用。 */
uint8_t zdlink_active_link(int8_t* rssi, uint32_t window_ms);

/* 诊断: 本地注入一条 TEXT 帧(不经空口, 不污染其它节点),
 * 用于验证 解析→去重→事件→面板列表 的完整链路 */
void zdlink_selftest_rx_text(uint32_t src, const char* text);

/* 诊断: 注入一条 LTEXT(模拟 C3 回传空口文本), 走完整的收包解析/节点表/事件链路 */
void zdlink_selftest_rx_ltext(uint32_t bridge, uint32_t lora_from, int8_t rssi, int8_t snr4,
                              uint8_t hops, const char* text);

/* 诊断: 给最近一条待确认发送注入 LNACK, 用于验证失败原因显示 */
void zdlink_selftest_rx_lnack(uint8_t reason);

/* 诊断: 注入一条 ZT_TIME(协议 v1.2), 验证 解析→采纳规则→顶栏时间 链路 */
void zdlink_selftest_rx_time(uint32_t utc, uint8_t src, uint8_t acc);

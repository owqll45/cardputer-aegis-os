#include "zdlink.h"

#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <M5Unified.hpp>
#include <hal/hal.h>
#include <string.h>

static const char* TAG = "zdlink";

/* 广播地址与信道: 必须和 glink / C3 完全一致 */
static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
#define ZD_CH 1

static volatile bool s_ready = false;
static uint32_t s_my_id = 0;
static uint16_t s_seq = 0;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* 文本事件只读钩子(全局通知层用), 注册后不改变原有事件流 */
static zdlink_event_hook_t s_ev_hook = nullptr;

void zdlink_set_event_hook(zdlink_event_hook_t cb)
{
    s_ev_hook = cb;
}

static uint32_t s_rx_ok = 0, s_rx_bad = 0, s_tx = 0, s_ack_rx = 0;
/* 文本类计数: 用于"通知条次数 == 收到消息条数"的对账 */
static uint32_t s_rx_text = 0, s_rx_lora = 0, s_dup_drop = 0, s_loop = 0;

/* 活跃链路打点: 状态栏据此显示"当前用 ESP-NOW 还是 LoRa"。
 * 只在 WiFi 任务写, app 任务读; 32 位对齐读写原子, 不额外加锁。 */
static volatile uint32_t s_act_e_ms = 0, s_act_l_ms = 0;
static volatile int8_t s_act_e_rssi = 0, s_act_l_rssi = 0;

/* 桌面实时面板打点: 最近收发时刻 / 最近 ACK 往返 / LoRa 空口 SNR(127=未知)。
 * WiFi 任务写, app 任务读; 32 位对齐读写原子。 */
static volatile uint32_t s_last_rx_ms = 0, s_last_tx_ms = 0, s_last_ack_rtt_ms = 0;
static volatile int8_t s_act_l_snr4 = 127;

/* ==================== 小端 / CRC ==================== */
static void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void wr32(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t crc16_ccitt(const uint8_t* d, size_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint32_t zdlink_now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 记一次链路活动(rssi=0 表示这次不知道信号, 保留上次的值) */
static void link_mark(uint8_t link, int8_t rssi)
{
    uint32_t t = zdlink_now_ms();
    if (link == ZD_VIA_L) {
        s_act_l_ms = t;
        if (rssi) s_act_l_rssi = rssi;
    } else {
        s_act_e_ms = t;
        if (rssi) s_act_e_rssi = rssi;
    }
}

/* ==================== 去重窗口(与 C3 一致: 8 源 x 32 条) ==================== */
#define DEDUP_SRC 8
#define DEDUP_N   32

struct DedupSlot {
    uint32_t src;
    uint16_t last[DEDUP_N];
    uint8_t cnt;
    uint8_t head;
};
static DedupSlot s_dd[DEDUP_SRC];

/* 调用方必须持有 s_mux。返回 true = 首次见到 */
static bool dedup_check_locked(uint32_t src, uint16_t seq)
{
    int slot = -1, empty = -1;
    for (int i = 0; i < DEDUP_SRC; i++) {
        if (s_dd[i].cnt && s_dd[i].src == src) { slot = i; break; }
        if (!s_dd[i].cnt && empty < 0) empty = i;
    }
    if (slot < 0) {
        slot = (empty >= 0) ? empty : 0;
        memset(&s_dd[slot], 0, sizeof(s_dd[slot]));
        s_dd[slot].src = src;
    }
    DedupSlot* d = &s_dd[slot];
    for (int i = 0; i < d->cnt; i++)
        if (d->last[i] == seq) return false;
    d->last[d->head] = seq;
    d->head = (uint8_t)((d->head + 1) % DEDUP_N);
    if (d->cnt < DEDUP_N) d->cnt++;
    return true;
}

/* ==================== 节点表 ==================== */
#define NODE_MAX 8
static ZdNodeInfo s_nodes[NODE_MAX];
static uint8_t s_node_cnt = 0;

static void node_touch_locked(uint32_t id, int8_t rssi, uint8_t hops)
{
    for (int i = 0; i < s_node_cnt; i++) {
        if (s_nodes[i].id == id) {
            /* NODETAB 里有些条目 RSSI 是 0(未知), 不能把已知的信号强度冲掉 */
            if (rssi != 0) s_nodes[i].rssi = rssi;
            s_nodes[i].hops = hops;
            s_nodes[i].espnow = 1;
            s_nodes[i].seen_ms = zdlink_now_ms();
            return;
        }
    }
    int slot = (s_node_cnt < NODE_MAX) ? s_node_cnt++ : 0;
    if (s_node_cnt == NODE_MAX) {
        /* 满了踢最久没见到的 */
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < NODE_MAX; i++) {
            if (s_nodes[i].seen_ms < oldest) { oldest = s_nodes[i].seen_ms; slot = i; }
        }
    }
    memset(&s_nodes[slot], 0, sizeof(s_nodes[slot]));
    s_nodes[slot].id = id;
    s_nodes[slot].rssi = rssi;
    s_nodes[slot].hops = hops;
    s_nodes[slot].batt_pct = 255;
    s_nodes[slot].snr = 127;  /* 未知 */
    s_nodes[slot].espnow = 1;
    s_nodes[slot].seen_ms = zdlink_now_ms();
}

static void node_batt_locked(uint32_t id, int8_t rssi, uint16_t mv, uint8_t pct, uint8_t role)
{
    for (int i = 0; i < s_node_cnt; i++) {
        if (s_nodes[i].id == id) {
            /* 心跳本身是 ESP-NOW 直收帧, 带的是本跳 RSSI; 只有 TEXT 不发心跳的
             * 节点 rssi 会一直是 0, 这里补上才能显示真实信号强度 */
            if (rssi != 0) s_nodes[i].rssi = rssi;
            s_nodes[i].batt_mv = mv;
            s_nodes[i].batt_pct = pct;
            s_nodes[i].role = role;
            s_nodes[i].seen_ms = zdlink_now_ms();
            return;
        }
    }
}

/* LoRa 空口节点: 记录真实来源 + 空口 RSSI/SNR/hops(协议 §3) */
static void node_lora_locked(uint32_t id, int8_t rssi, int8_t snr4, uint8_t hops)
{
    for (int i = 0; i < s_node_cnt; i++) {
        if (s_nodes[i].id == id) {
            s_nodes[i].rssi = rssi;
            s_nodes[i].lora_rssi = rssi;
            s_nodes[i].snr = snr4;
            s_nodes[i].hops = hops;
            s_nodes[i].lora = 1;
            s_nodes[i].seen_ms = zdlink_now_ms();
            return;
        }
    }
    node_touch_locked(id, rssi, hops);
    for (int i = 0; i < s_node_cnt; i++) {
        if (s_nodes[i].id == id) {
            s_nodes[i].snr = snr4;
            s_nodes[i].lora_rssi = rssi;
            s_nodes[i].lora = 1;
            s_nodes[i].espnow = 0;  /* 只有空口消息 => 不是 ZD 网内邻居 */
            break;
        }
    }
}

/* 协议 §4.4: C3 上空口时给文本加 "[A:xxxx] " 前缀, xxxx = 发送方 ADV 的 NodeNum 低 16 位。
 * 命中本机 => 自己发出去、又被别的 LoRa 节点收回来的回环; §3 要求不得因此丢弃。 */
static bool lora_prefix_is_self(const uint8_t* t, int len, uint32_t my_id)
{
    if (len < 8) return false;
    if (t[0] != '[' || t[1] != 'A' || t[2] != ':' || t[7] != ']') return false;
    uint32_t v = 0;
    for (int i = 3; i < 7; i++) {
        uint8_t c = t[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (uint32_t)d;
    }
    return v == (my_id & 0xFFFFu);
}

/* ==================== 事件环 ==================== */
#define EV_RING 8
static ZdEvent s_ev[EV_RING];
static volatile int s_ev_head = 0;
static volatile int s_ev_tail = 0;

static void ev_push_locked(const ZdEvent& e)
{
    int next = (s_ev_tail + 1) % EV_RING;
    if (next == s_ev_head) s_ev_head = (s_ev_head + 1) % EV_RING;  /* 满了丢最旧 */
    s_ev[s_ev_tail] = e;
    s_ev_tail = next;
}

bool zdlink_pop_event(ZdEvent& out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_ev_head != s_ev_tail) {
        out = s_ev[s_ev_head];
        s_ev_head = (s_ev_head + 1) % EV_RING;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

/* ==================== 待发 ACK 队列 ==================== */
struct AckReq { uint16_t seq, msgid; uint8_t mac[6]; };
static AckReq s_ackq[4];
static volatile int s_ack_head = 0, s_ack_tail = 0;

static void ack_enqueue_locked(uint16_t seq, uint16_t msgid, const uint8_t* mac)
{
    int next = (s_ack_tail + 1) % 4;
    if (next == s_ack_head) return;  /* 满则丢弃(ACK 不是关键路径) */
    s_ackq[s_ack_tail].seq = seq;
    s_ackq[s_ack_tail].msgid = msgid;
    if (mac) memcpy(s_ackq[s_ack_tail].mac, mac, 6);
    else memset(s_ackq[s_ack_tail].mac, 0, 6);
    s_ack_tail = next;
}

/* ==================== 待确认的文本发送 ==================== */
#define TX_MAX 4
#define TX_WAIT_ESPNOW 6000   /* 本跳 ACK */
#define TX_WAIT_LORA   15000  /* 协议 §5.2: 空口 TX-DONE(SF12 单帧约 4.6s) + 排队 */

struct PendingTx {
    bool used;
    uint16_t seq;
    uint16_t msgid;
    uint32_t at;
    uint16_t wait_ms;  /* 等待上限 */
    uint8_t state;     /* 0=等待 1=已确认 2=超时/失败 */
    uint8_t reason;    /* state=2 且来自 LNACK 时的 reason(协议 §5.3) */
};
static PendingTx s_pending[TX_MAX];

static void pending_add(uint16_t seq, uint16_t msgid, uint16_t wait_ms)
{
    portENTER_CRITICAL(&s_mux);
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < TX_MAX; i++) {
        if (!s_pending[i].used) { slot = i; break; }
        if (s_pending[i].at < oldest) { oldest = s_pending[i].at; slot = i; }
    }
    if (slot >= 0) {
        s_pending[slot].used = true;
        s_pending[slot].seq = seq;
        s_pending[slot].msgid = msgid;
        s_pending[slot].at = zdlink_now_ms();
        s_pending[slot].wait_ms = wait_ms;
        s_pending[slot].state = 0;
        s_pending[slot].reason = 0;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ==================== C3 桥表(v1.1 BEAT caps) ==================== */
#define BRIDGE_MAX 4
static ZdBridge s_bridges[BRIDGE_MAX];
static uint8_t s_bridge_cnt = 0;

static void bridge_update_locked(uint32_t id, uint8_t caps)
{
    uint32_t now = zdlink_now_ms();
    for (int i = 0; i < s_bridge_cnt; i++) {
        if (s_bridges[i].id == id) {
            s_bridges[i].caps = caps;
            s_bridges[i].seen_ms = now;
            return;
        }
    }
    int slot = (s_bridge_cnt < BRIDGE_MAX) ? s_bridge_cnt++ : 0;
    if (s_bridge_cnt == BRIDGE_MAX) {
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < BRIDGE_MAX; i++) {
            if (s_bridges[i].seen_ms < oldest) { oldest = s_bridges[i].seen_ms; slot = i; }
        }
    }
    s_bridges[slot].id = id;
    s_bridges[slot].caps = caps;
    s_bridges[slot].seen_ms = now;
}

/* ==================== 发包 ==================== */
static size_t frame_build(uint8_t* buf, uint8_t type, uint32_t src, uint32_t dst, uint16_t seq,
                          uint16_t msgid, uint8_t flags, const uint8_t* pl, uint16_t plen)
{
    if (plen > ZD_MAX_PAYLOAD) plen = ZD_MAX_PAYLOAD;
    buf[0] = 'Z';
    buf[1] = 'D';
    buf[2] = ZD_VER;
    buf[3] = type;
    wr32(buf + 4, src);
    wr32(buf + 8, dst);          /* 协议 §7: 广播 0xFFFFFFFF, 单播=目标 NodeNum */
    buf[12] = 3;                 /* ttl */
    buf[13] = flags;
    wr16(buf + 14, seq);
    wr16(buf + 16, msgid);
    buf[18] = 0;                 /* frag */
    buf[19] = 1;                 /* frags */
    wr16(buf + 20, plen);
    if (plen) memcpy(buf + ZD_HDR, pl, plen);
    uint16_t crc = crc16_ccitt(buf, 22, 0xFFFF);
    crc = crc16_ccitt(buf + ZD_HDR, plen, crc);
    wr16(buf + 22, crc);
    return (size_t)ZD_HDR + plen;
}

/* 所有帧都从已注册的广播 peer 发出:
 * ESP-NOW 的单播需要对方完整 MAC, 而 NodeNum 只含 MAC 低 4 字节;
 * C3 侧也是在广播地址上收包, 再按 header.dst 判定 for_me(见 d3/c3msg/main/main.c)。
 * 所以"单播"由 header.dst 表达, 空口地址仍用广播。 */
static bool zd_send_ex(uint8_t type, uint32_t dst, const uint8_t* pl, uint16_t plen, uint16_t msgid,
                       uint8_t flags, uint16_t* out_seq)
{
    if (!s_ready) return false;
    if (plen > ZD_MAX_PAYLOAD) plen = ZD_MAX_PAYLOAD;

    uint8_t buf[ZD_HDR + ZD_MAX_PAYLOAD];
    uint16_t seq;
    portENTER_CRITICAL(&s_mux);
    seq = s_seq++;
    portEXIT_CRITICAL(&s_mux);

    size_t n = frame_build(buf, type, s_my_id, dst, seq, msgid, flags, pl, plen);
    esp_err_t e = esp_now_send(kBroadcast, buf, n);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "send type=%u len=%u fail %d", type, (unsigned)plen, (int)e);
        return false;
    }
    s_tx++;
    s_last_tx_ms = zdlink_now_ms();
    if (out_seq) *out_seq = seq;
    return true;
}

static bool zd_send(uint8_t type, const uint8_t* pl, uint16_t plen, uint16_t msgid, uint8_t flags,
                    uint16_t* out_seq)
{
    return zd_send_ex(type, 0xFFFFFFFFu, pl, plen, msgid, flags, out_seq);
}

/* ==================== 初始化 / 状态 ==================== */
void zdlink_init()
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];

    portENTER_CRITICAL(&s_mux);
    s_my_id = id;
    /* 协议 §0: 接收方按 (from, seq) 去重, 每源保留 32 条。
     * 本机重启后如果 seq 又从 0 开始, 前若干条会被对端当成重复帧静默丢弃,
     * 所以起点取随机值; 之后仍严格递增。 */
    s_seq = (uint16_t)(esp_random() & 0xFFFF);
    memset(s_dd, 0, sizeof(s_dd));
    s_node_cnt = 0;
    s_ev_head = 0;
    s_ev_tail = 0;
    s_ack_head = 0;
    s_ack_tail = 0;
    memset(s_pending, 0, sizeof(s_pending));
    s_bridge_cnt = 0;
    memset(s_bridges, 0, sizeof(s_bridges));
    s_ready = true;
    portEXIT_CRITICAL(&s_mux);

    /* 广播 peer 由 glink 添加; 这里只做幂等兜底 */
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, kBroadcast, 6);
    p.channel = ZD_CH;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_add_peer(&p);

    ESP_LOGI(TAG, "ZD v1 ready, id=%08lX", (unsigned long)id);
}

void zdlink_set_ready(bool ready)
{
    s_ready = ready;
    if (!ready) ESP_LOGW(TAG, "ZD link down");
}

bool zdlink_ready() { return s_ready; }
uint32_t zdlink_my_id() { return s_my_id; }

/* ==================== 收包(WiFi 任务) ==================== */
void zdlink_on_frame(const esp_now_recv_info_t* info, const uint8_t* data, int len)
{
    if (!info || !data || len < ZD_HDR) return;
    if (data[0] != 'Z' || data[1] != 'D') return;  /* 正常不会到这: glink 已按 magic 分流 */
    if (data[2] != ZD_VER) return;

    uint16_t plen = rd16(data + 20);
    if ((int)plen + ZD_HDR != len) return;

    uint16_t crc = crc16_ccitt(data, 22, 0xFFFF);
    crc = crc16_ccitt(data + ZD_HDR, plen, crc);
    if (crc != rd16(data + 22)) {
        s_rx_bad++;
        ESP_LOGW(TAG, "crc bad len=%d", len);
        return;
    }

    uint32_t src = rd32(data + 4);
    if (src == s_my_id) return;  /* 自己的广播回环 */

    /* 协议 §7: 0 保留; 单播给别人的帧只应由中继处理, ADV 不做 ZD 中继, 直接忽略 */
    uint32_t dst = rd32(data + 8);
    if (dst == 0) return;
    if (dst != 0xFFFFFFFFu && dst != s_my_id) return;

    uint16_t seq = rd16(data + 14);
    uint8_t type = data[3];
    uint8_t flags = data[13];
    int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;

    bool first;
    portENTER_CRITICAL(&s_mux);
    first = dedup_check_locked(src, seq);
    if (first) node_touch_locked(src, rssi, 0);
    portEXIT_CRITICAL(&s_mux);
    if (!first) {
        if (type == ZT_TEXT || type == ZT_LTEXT) s_dup_drop++;
        return;
    }

    s_rx_ok++;
    s_last_rx_ms = zdlink_now_ms();
    /* ESP-NOW 上收到的帧算 E 活跃; LTEXT 是经桥从 LoRa 空口来的, 记在 L 上不重复计 */
    if (type != ZT_LTEXT) link_mark(ZD_VIA_E, rssi);
    const uint8_t* pl = data + ZD_HDR;

    switch (type) {
        case ZT_TEXT: {
            /* 中文按字节截断, 与 C3 的 96B 显示上限一致 */
            uint16_t c = plen > 96 ? 96 : plen;
            /* 只有真发生截断时才回退, 避免读到 payload 之外 */
            while (c && c < plen && (pl[c] & 0xC0) == 0x80) c--;
            ZdEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = ZD_EV_TEXT;
            ev.flags = flags;
            ev.via = ZD_VIA_E;  /* 直收 ESP-NOW; LoRa 回传走 ZT_LTEXT, 标 L */
            ev.src = src;
            ev.rssi = rssi;
            ev.msgid = rd16(data + 16);
            ev.seq = seq;
            s_rx_text++;
            memcpy(ev.text, pl, c);
            ev.text[c] = 0;
            for (uint16_t i = 0; i < c; i++) {
                if (ev.text[i] == '\n' || ev.text[i] == '\r' || ev.text[i] == '\t') ev.text[i] = ' ';
            }
            ESP_LOGI(TAG, "rx TEXT %08lX rssi=%d: %s", (unsigned long)src, rssi, ev.text);
            portENTER_CRITICAL(&s_mux);
            ev_push_locked(ev);
            if (flags & ZF_WANT_ACK) ack_enqueue_locked(seq, rd16(data + 16), info->src_addr);
            portEXIT_CRITICAL(&s_mux);
            if (s_ev_hook) s_ev_hook(ev);  /* 只读钩子, 跑在 WiFi 任务里 */
            break;
        }
        case ZT_BEAT: {
            if (plen >= 4) {
                uint16_t mv = rd16(pl);
                uint8_t pct = pl[2];
                uint8_t role = pl[3];
                /* v1.1 §6: plen>=5 时追加 caps, 老 C3 只有 4B */
                uint8_t caps = (plen >= 5) ? pl[4] : 0;
                ESP_LOGI(TAG, "rx BEAT %08lX rssi=%d mv=%u pct=%u role=%u caps=0x%02X",
                         (unsigned long)src, rssi, (unsigned)mv, (unsigned)pct, (unsigned)role,
                         (unsigned)caps);
                portENTER_CRITICAL(&s_mux);
                node_batt_locked(src, rssi, mv, pct, role);
                if (plen >= 5 && (caps & 0x01)) bridge_update_locked(src, caps);
                portEXIT_CRITICAL(&s_mux);
            }
            break;
        }
        case ZT_NODETAB: {
            int n = plen / 6;
            for (int i = 0; i < n && i < 16; i++) {
                uint32_t id = rd32(pl + i * 6);
                int8_t r = (int8_t)pl[i * 6 + 4];
                uint8_t hops = pl[i * 6 + 5];
                if (id == s_my_id) continue;
                portENTER_CRITICAL(&s_mux);
                node_touch_locked(id, r, hops);
                portEXIT_CRITICAL(&s_mux);
                ESP_LOGI(TAG, "rx NODE %08lX rssi=%d hops=%u", (unsigned long)id, (int)r, (unsigned)hops);
            }
            break;
        }
        case ZT_ACK: {
            if (plen >= 4) {
                uint16_t aseq = rd16(pl);
                uint16_t amid = rd16(pl + 2);
                s_ack_rx++;
                ESP_LOGI(TAG, "rx ACK %08lX seq=%u msgid=%u", (unsigned long)src, (unsigned)aseq,
                         (unsigned)amid);
                portENTER_CRITICAL(&s_mux);
                uint32_t ack_now = zdlink_now_ms();
                for (int i = 0; i < TX_MAX; i++) {
                    if (!s_pending[i].used) continue;
                    /* seq 或 msgid 任一命中就确认: 桥转发回来的 ACK 可能只带 msgid,
                     * 只认 seq+msgid 双匹配会让已送达的消息一直显示"等 ACK"。 */
                    if ((aseq && s_pending[i].seq == aseq) || (amid && s_pending[i].msgid == amid)) {
                        s_pending[i].state = 1;
                        s_last_ack_rtt_ms = ack_now - s_pending[i].at;
                    }
                }
                portEXIT_CRITICAL(&s_mux);
                ZdEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = ZD_EV_ACK;
                ev.src = src;
                ev.rssi = rssi;
                ev.ack_seq = aseq;
                ev.ack_msgid = amid;
                portENTER_CRITICAL(&s_mux);
                ev_push_locked(ev);
                portEXIT_CRITICAL(&s_mux);
            }
            break;
        }
        case ZT_LTEXT: {
            /* 协议 §3: payload[0..3]=lora_from [4]=rssi [5]=snr4 [6]=hops [7]=lflags [8..]=text
             * 帧头 from 是 C3 桥自己, 真实来源只看 payload; 本帧不回 ACK。 */
            if (plen < 8) break;
            uint32_t lora_from = rd32(pl);
            int8_t lrssi = (int8_t)pl[4];
            int8_t snr4 = (int8_t)pl[5];
            uint8_t lhops = pl[6];
            const uint8_t* tx = pl + 8;
            int tlen = (int)plen - 8;
            if (tlen > 96) tlen = 96;
            /* 截断时退到 UTF-8 字符边界(tx[tlen] 在原始 payload 内) */
            if (tlen < (int)plen - 8) {
                while (tlen > 0 && (tx[tlen] & 0xC0) == 0x80) tlen--;
            }

            ZdEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = ZD_EV_LTEXT;
            ev.flags = flags;
            ev.via = ZD_VIA_L;
            ev.src = lora_from;
            ev.lora_from = lora_from;
            ev.rssi = lrssi;
            ev.snr4 = snr4;
            ev.hops = lhops;
            ev.loopback = lora_prefix_is_self(tx, tlen, s_my_id) ? 1 : 0;
            ev.msgid = rd16(data + 16);
            ev.seq = seq;
            s_rx_lora++;
            s_act_l_snr4 = snr4;
            link_mark(ZD_VIA_L, lrssi);
            if (ev.loopback) s_loop++;
            memcpy(ev.text, tx, tlen);
            ev.text[tlen] = 0;
            for (int i = 0; i < tlen; i++) {
                if (ev.text[i] == '\n' || ev.text[i] == '\r' || ev.text[i] == '\t') ev.text[i] = ' ';
            }

            portENTER_CRITICAL(&s_mux);
            node_lora_locked(lora_from, lrssi, snr4, lhops);
            ev_push_locked(ev);
            portEXIT_CRITICAL(&s_mux);

            ESP_LOGI(TAG, "rx LTEXT bridge=%08lX from=%08lX rssi=%d snr4=%d hops=%u: %s",
                     (unsigned long)src, (unsigned long)lora_from, (int)lrssi, (int)snr4,
                     (unsigned)lhops, ev.text);
            if (s_ev_hook) s_ev_hook(ev);
            break;
        }
        case ZT_TIME: {
            /* 协议 v1.2 §1.2: payload 6B = utc32 LE + src + acc(plen>=5; 缺第 6 字节按 5s)
             * 本帧不回 ACK, 也不进聊天列表; 采纳规则由 UI 侧按本地时钟来源判断。 */
            if (plen < 5) break;
            uint32_t utc = rd32(pl);
            uint8_t tsrc = pl[4];
            uint8_t tacc = (plen >= 6) ? pl[5] : 5;
            if (!utc || !tsrc) {
                ESP_LOGW(TAG, "rx TIME ignored utc=%lu src=%u", (unsigned long)utc, (unsigned)tsrc);
                break;
            }
            ZdEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = ZD_EV_TIME;
            ev.flags = flags;
            ev.src = src;
            ev.rssi = rssi;
            ev.utc = utc;
            ev.tsrc = tsrc;
            ev.tacc = tacc;
            portENTER_CRITICAL(&s_mux);
            ev_push_locked(ev);
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "rx TIME %08lX utc=%lu src=%u acc=%u", (unsigned long)src,
                     (unsigned long)utc, (unsigned)tsrc, (unsigned)tacc);
            break;
        }
        case ZT_LNACK: {
            /* 协议 §5.3: payload = seq(2) + msgid(2) + reason(1); 明确失败, 不能当成功 */
            uint16_t nseq = rd16(pl);
            uint16_t nmid = rd16(pl + 2);
            uint8_t reason = pl[4];
            ESP_LOGW(TAG, "rx LNACK %08lX seq=%u msgid=%u reason=%u", (unsigned long)src,
                     (unsigned)nseq, (unsigned)nmid, (unsigned)reason);
            portENTER_CRITICAL(&s_mux);
            for (int i = 0; i < TX_MAX; i++) {
                if (s_pending[i].used && s_pending[i].seq == nseq && s_pending[i].msgid == nmid) {
                    s_pending[i].state = 2;
                    s_pending[i].reason = reason;
                }
            }
            ZdEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = ZD_EV_LNACK;
            ev.flags = flags;
            ev.src = src;
            ev.ack_seq = nseq;
            ev.ack_msgid = nmid;
            ev.reason = reason;
            ev_push_locked(ev);
            portEXIT_CRITICAL(&s_mux);
            break;
        }
        default:
            break;
    }
}

/* ACK 单播回给请求来源：来源 MAC 未知/加 peer 失败/发送失败时退回广播。
 * 其它帧仍走广播(单播语义由 header.dst 表达), 这里只针对 ACK 做优化。 */
static bool zd_send_ack_to(const uint8_t* mac, uint16_t seq, uint16_t msgid)
{
    if (!s_ready) return false;
    uint8_t buf[ZD_HDR + 4];
    uint16_t s;
    portENTER_CRITICAL(&s_mux);
    s = s_seq++;
    portEXIT_CRITICAL(&s_mux);
    wr16(buf + ZD_HDR, seq);
    wr16(buf + ZD_HDR + 2, msgid);
    size_t n = frame_build(buf, ZT_ACK, s_my_id, 0xFFFFFFFFu, s, msgid, 0,
                           buf + ZD_HDR, 4);
    if (mac && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) &&
        (mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) != 0xFF) {
        if (!esp_now_is_peer_exist(mac)) {
            esp_now_peer_info_t p;
            memset(&p, 0, sizeof(p));
            memcpy(p.peer_addr, mac, 6);
            p.channel = ZD_CH;
            p.ifidx = WIFI_IF_STA;
            p.encrypt = false;
            esp_now_add_peer(&p);
        }
        if (esp_now_is_peer_exist(mac) && esp_now_send(mac, buf, n) == ESP_OK) {
            s_tx++;
            return true;
        }
        ESP_LOGW(TAG, "ack uni fail -> bcast");
    }
    if (esp_now_send(kBroadcast, buf, n) == ESP_OK) {
        s_tx++;
        return true;
    }
    return false;
}

/* ==================== 主循环 ==================== */
void zdlink_poll()
{
    if (!s_ready) return;
    uint32_t now = zdlink_now_ms();

    /* 1) 补发 ACK(收包回调里不发, 避免在 WiFi 回调里嵌套发送) */
    for (;;) {
        AckReq a;
        bool has = false;
        portENTER_CRITICAL(&s_mux);
        if (s_ack_head != s_ack_tail) {
            a = s_ackq[s_ack_head];
            s_ack_head = (s_ack_head + 1) % 4;
            has = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!has) break;
        zd_send_ack_to(a.mac, a.seq, a.msgid);
    }

    /* 2) 心跳: 5s 一次, 带本机电量(C3 用 pl[0..1]=mV, pl[2]=pct, pl[3]=role) */
    static uint32_t s_beat_last = 0;
    if (now - s_beat_last >= 5000) {
        s_beat_last = now;
        int level = (int)GetHAL().getBatLevel();
        int v = (int)M5.Power.getBatteryVoltage();
        uint16_t mv = (v > 0 && v < 6000) ? (uint16_t)v : 0;
        uint8_t pct = (level < 0) ? 0 : (level > 100 ? 100 : (uint8_t)level);
        uint8_t pl[4];
        wr16(pl, mv);
        pl[2] = pct;
        pl[3] = 2;  /* 2 = 终端(本机); C3 只定义了 1=节点/中继, 不校验该值 */
        zd_send(ZT_BEAT, pl, 4, 0, 0, nullptr);
    }

    /* 3) 发送超时(ESP-NOW 6s, LoRa 15s) */
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < TX_MAX; i++) {
        if (s_pending[i].used && s_pending[i].state == 0 &&
            now - s_pending[i].at > s_pending[i].wait_ms) {
            s_pending[i].state = 2;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

bool zdlink_send_text(const char* text, int len, uint16_t* out_msgid, uint16_t* out_seq)
{
    if (!s_ready || !text || len <= 0) return false;
    if (len > ZD_MAX_PAYLOAD) len = ZD_MAX_PAYLOAD;

    uint16_t msgid = (uint16_t)(esp_random() & 0xFFFF);
    uint16_t seq = 0;
    if (!zd_send(ZT_TEXT, (const uint8_t*)text, (uint16_t)len, msgid, ZF_WANT_ACK, &seq)) return false;
    pending_add(seq, msgid, TX_WAIT_ESPNOW);
    link_mark(ZD_VIA_E, 0);

    if (out_msgid) *out_msgid = msgid;
    if (out_seq) *out_seq = seq;
    ESP_LOGI(TAG, "tx TEXT seq=%u msgid=%u len=%d", (unsigned)seq, (unsigned)msgid, len);
    return true;
}

bool zdlink_send_lora_text(const char* text, int len, uint32_t bridge_id, uint16_t* out_msgid,
                           uint16_t* out_seq)
{
    if (!s_ready || !text || len <= 0) return false;
    if (bridge_id == 0 || bridge_id == 0xFFFFFFFFu) return false;  /* §4.1 必须单播到桥 */

    /* §4.2: ADV 自己先限制在 72B, 避免被 C3 截断 */
    if (len > 72) {
        len = 72;
        while (len > 0 && ((uint8_t)text[len] & 0xC0) == 0x80) len--;
    }
    if (len <= 0) return false;

    uint16_t msgid = (uint16_t)(esp_random() & 0xFFFF);
    uint16_t seq = 0;
    if (!zd_send_ex(ZT_TEXT, bridge_id, (const uint8_t*)text, (uint16_t)len, msgid,
                    ZF_LORA | ZF_WANT_ACK, &seq)) {
        return false;
    }
    pending_add(seq, msgid, TX_WAIT_LORA);
    link_mark(ZD_VIA_L, 0);

    if (out_msgid) *out_msgid = msgid;
    if (out_seq) *out_seq = seq;
    ESP_LOGI(TAG, "tx LORA text bridge=%08lX seq=%u msgid=%u len=%d", (unsigned long)bridge_id,
             (unsigned)seq, (unsigned)msgid, len);
    return true;
}

/* 协议 v1.2 §1.3: ZT_TIME 广播帧, flags=0(不要 ACK), msgid=0, payload 6B */
bool zdlink_send_time(uint32_t utc, uint8_t src, uint8_t acc)
{
    if (!utc || !src) return false;
    uint8_t pl[6];
    wr32(pl, utc);
    pl[4] = src;
    pl[5] = acc;
    bool ok = zd_send(ZT_TIME, pl, sizeof(pl), 0, 0, nullptr);
    ESP_LOGI(TAG, "tx TIME utc=%lu src=%u acc=%u ok=%d", (unsigned long)utc, (unsigned)src,
             (unsigned)acc, (int)ok);
    return ok;
}

uint8_t zdlink_tx_state(uint16_t msgid)
{
    uint8_t st = 3;
    uint32_t now = zdlink_now_ms();
    portENTER_CRITICAL(&s_mux);
    uint32_t newest = 0;
    for (int i = 0; i < TX_MAX; i++) {
        if (!s_pending[i].used || s_pending[i].msgid != msgid) continue;
        if (s_pending[i].state == 0 && now - s_pending[i].at > s_pending[i].wait_ms) {
            s_pending[i].state = 2;
        }
        if (i == 0 || s_pending[i].at >= newest) {
            newest = s_pending[i].at;
            st = s_pending[i].state;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return st;
}

uint8_t zdlink_tx_reason(uint16_t msgid)
{
    uint8_t reason = 0;
    portENTER_CRITICAL(&s_mux);
    uint32_t newest = 0;
    for (int i = 0; i < TX_MAX; i++) {
        if (!s_pending[i].used || s_pending[i].msgid != msgid) continue;
        if (i == 0 || s_pending[i].at >= newest) {
            newest = s_pending[i].at;
            reason = s_pending[i].reason;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return reason;
}

int zdlink_get_bridges(ZdBridge* out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    uint32_t now = zdlink_now_ms();
    portENTER_CRITICAL(&s_mux);
    /* 插入排序: 最近活跃在前(条数 <= 4) */
    for (int i = 0; i < s_bridge_cnt && n < max; i++) {
        if (now - s_bridges[i].seen_ms > 30000) continue;  /* BEAT 5s 一次, 30s 算掉线 */
        int j = n++;
        while (j > 0 && out[j - 1].seen_ms < s_bridges[i].seen_ms) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = s_bridges[i];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* ==================== 就近选桥 ==================== */
uint32_t zdlink_pick_bridge(uint32_t prefer, uint8_t* caps_out)
{
    ZdBridge br[4];
    int n = zdlink_get_bridges(br, 4);
    if (caps_out) *caps_out = 0;
    if (n <= 0) return 0;

    uint32_t now = zdlink_now_ms();
    int best = -1;
    int best_score = -100000;

    for (int i = 0; i < n; i++) {
        if (!(br[i].caps & 0x01)) continue;  /* 没有桥能力, 不参与 */
        int age_s = (int)((now - br[i].seen_ms) / 1000);
        int score = 0;
        score += (br[i].caps & 0x02) ? 500 : 0;   /* 能发射的优先 */
        score += 200 - age_s * 6;                  /* 新鲜度: 越近越优先 */
        if (prefer && br[i].id == prefer) score += 150;

        portENTER_CRITICAL(&s_mux);
        for (int j = 0; j < s_node_cnt; j++) {
            if (s_nodes[j].id != br[i].id) continue;
            int rr = s_nodes[j].lora ? s_nodes[j].lora_rssi : s_nodes[j].rssi;
            if (rr) score += (rr + 100);           /* -50dBm -> +50 */
            if (s_nodes[j].hops != 255) score += (10 - (int)s_nodes[j].hops) * 2;
            break;
        }
        portEXIT_CRITICAL(&s_mux);

        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    if (best < 0) return 0;
    if (caps_out) *caps_out = br[best].caps;
    /* 调用方每帧都会问一次(用于界面显示), 只在"选择变了"或"每 3s"打一行,
     * 否则串口日志会被刷爆(实测 9 秒 565 行)。 */
    static uint32_t s_pick_log_ms = 0;
    static uint32_t s_pick_last_id = 0;
    static int s_pick_last_score = 0;
    if (br[best].id != s_pick_last_id || best_score != s_pick_last_score ||
        (uint32_t)(now - s_pick_log_ms) > 3000) {
        s_pick_log_ms = now;
        s_pick_last_id = br[best].id;
        s_pick_last_score = best_score;
        ESP_LOGI("ZDLINK", "pick_bridge prefer=%08lX -> %08lX caps=%u score=%d",
                 (unsigned long)prefer, (unsigned long)br[best].id, (unsigned)br[best].caps, best_score);
    }
    return br[best].id;
}

/* ==================== 中继开关(本地配置) ==================== */
static volatile bool s_relay_on = true;

void zdlink_set_relay(bool on)
{
    s_relay_on = on;
}

bool zdlink_relay_enabled()
{
    return s_relay_on;
}

int zdlink_get_nodes(ZdNodeInfo* out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    uint32_t now = zdlink_now_ms();
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_node_cnt && n < max; i++) {
        /* 超过 60s 没消息的节点不再展示 */
        if (now - s_nodes[i].seen_ms > 60000) continue;
        out[n++] = s_nodes[i];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void zdlink_stats(uint32_t* rx_ok, uint32_t* rx_bad, uint32_t* tx, uint32_t* ack_rx)
{
    if (rx_ok) *rx_ok = s_rx_ok;
    if (rx_bad) *rx_bad = s_rx_bad;
    if (tx) *tx = s_tx;
    if (ack_rx) *ack_rx = s_ack_rx;
}

void zdlink_text_stats(uint32_t* text, uint32_t* lora, uint32_t* dup, uint32_t* loop)
{
    if (text) *text = s_rx_text;
    if (lora) *lora = s_rx_lora;
    if (dup) *dup = s_dup_drop;
    if (loop) *loop = s_loop;
}

uint8_t zdlink_active_link(int8_t* rssi, uint32_t window_ms)
{
    if (!window_ms) window_ms = 5000;
    uint32_t now = zdlink_now_ms();
    uint32_t e = s_act_e_ms, l = s_act_l_ms;
    bool e_ok = e && (now - e) <= window_ms;
    bool l_ok = l && (now - l) <= window_ms;

    uint8_t link = 0;
    int8_t r = 0;
    if (e_ok && l_ok) {
        /* 两路都通: 显示最近一次真正在用的那条 */
        if ((int32_t)(l - e) > 0) {
            link = ZD_VIA_L;
            r = s_act_l_rssi;
        } else {
            link = ZD_VIA_E;
            r = s_act_e_rssi;
        }
    } else if (l_ok) {
        link = ZD_VIA_L;
        r = s_act_l_rssi;
    } else if (e_ok) {
        link = ZD_VIA_E;
        r = s_act_e_rssi;
    }
    if (rssi) *rssi = r;
    return link;
}

int zdlink_link_info(ZdLinkInfo* out)
{
    if (!out) return 0;
    int8_t rssi = 0;
    int8_t snr4 = 127;

    uint8_t link = zdlink_active_link(&rssi, 0);
    if (link == ZD_VIA_L) snr4 = s_act_l_snr4;

    uint32_t now = zdlink_now_ms();
    uint8_t nodes = 0, bridges = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_node_cnt; i++) {
        if (now - s_nodes[i].seen_ms <= 60000) nodes++;
    }
    for (int i = 0; i < s_bridge_cnt; i++) {
        if (now - s_bridges[i].seen_ms <= 30000) bridges++;
    }
    portEXIT_CRITICAL(&s_mux);

    out->link       = link;
    out->rssi       = rssi;
    out->snr4       = snr4;
    out->last_rx_ms = s_last_rx_ms;
    out->last_tx_ms = s_last_tx_ms;
    out->ack_rtt_ms = s_last_ack_rtt_ms;
    out->nodes      = nodes;
    out->bridges    = bridges;
    return nodes;
}

static void selftest_inject(uint32_t src, uint8_t type, const uint8_t* pl, uint16_t plen,
                            const char* what)
{
    uint8_t buf[ZD_HDR + ZD_MAX_PAYLOAD];
    static uint16_t s_test_seq = 50000;
    size_t len = frame_build(buf, type, src, 0xFFFFFFFFu, s_test_seq++, 0xABCD, 0, pl, plen);

    /* 注意: 本 IDF 版本的 esp_now_recv_info_t 地址字段是"指针"(uint8_t*),
     * 不能当数组用, 必须指向真实缓冲区 */
    uint8_t sa[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint8_t da[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    wifi_pkt_rx_ctrl_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.rssi = -42;

    esp_now_recv_info_t info;
    memset(&info, 0, sizeof(info));
    info.src_addr = sa;
    info.des_addr = da;
    info.rx_ctrl = &ctrl;

    zdlink_on_frame(&info, buf, (int)len);
    ESP_LOGI(TAG, "selftest inject %s", what ? what : "");
}

void zdlink_selftest_rx_text(uint32_t src, const char* text)
{
    if (!text) return;
    selftest_inject(src, ZT_TEXT, (const uint8_t*)text, (uint16_t)strlen(text), "rx text");
}

void zdlink_selftest_rx_ltext(uint32_t bridge, uint32_t lora_from, int8_t rssi, int8_t snr4,
                              uint8_t hops, const char* text)
{
    if (!text) return;
    int tl = (int)strlen(text);
    if (tl > 96) tl = 96;
    uint8_t pl[8 + 96];
    memset(pl, 0, sizeof(pl));
    wr32(pl, lora_from);
    pl[4] = (uint8_t)rssi;
    pl[5] = (uint8_t)snr4;
    pl[6] = hops;
    pl[7] = 0;
    memcpy(pl + 8, text, tl);
    selftest_inject(bridge, ZT_LTEXT, pl, (uint16_t)(8 + tl), "rx LTEXT");
}

/* 诊断: 注入一条 ZT_TIME(协议 v1.2 §1.2), 走完整 解析→事件→采纳 链路 */
void zdlink_selftest_rx_time(uint32_t utc, uint8_t src, uint8_t acc)
{
    uint8_t pl[6];
    wr32(pl, utc);
    pl[4] = src;
    pl[5] = acc;
    selftest_inject(0x7DF331D0u, ZT_TIME, pl, sizeof(pl), "rx TIME");
}

void zdlink_selftest_rx_lnack(uint8_t reason)
{
    /* 给最近一条待确认发送回 LNACK, 走完整 解析→待确认状态→事件 链路 */
    uint16_t seq = 0, msgid = 0x1234;
    uint32_t newest = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < TX_MAX; i++) {
        if (!s_pending[i].used) continue;
        if (newest == 0 || s_pending[i].at <= newest) {
            newest = s_pending[i].at;
            seq = s_pending[i].seq;
            msgid = s_pending[i].msgid;
        }
    }
    portEXIT_CRITICAL(&s_mux);

    uint8_t pl[5];
    wr16(pl, seq);
    wr16(pl + 2, msgid);
    pl[4] = reason;
    selftest_inject(0x7DF331D0u, ZT_LNACK, pl, 5, "rx LNACK");
}

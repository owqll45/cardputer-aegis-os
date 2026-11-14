/*
 * ble_mesh.c — 官方 Meshtastic App 的 BLE 接口（NimBLE）
 *
 * 协议对齐官方固件：
 *   - 服务/特征 UUID 见 ble_mesh.h
 *   - FromRadio 一次读一条（配置阶段同步填满队列，避免 0 长度读）
 *   - FromNum 用 4 字节小端计数 notify
 *   - ToRadio.want_config_id 触发配置同步；ToRadio.packet(decoded/text) 转 LoRa 发送
 */
#include "ble_mesh.h"
#include "lora.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BLE_TAG "BLE"
#define FR_MAX   24
#define FR_MAXLEN 512
#define TXT_MAX  72

/* ---------- 官方 UUID ---------- */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0xfd, 0xea, 0x73, 0xe2, 0xae, 0x5d, 0xa8, 0x9f,
                     0x1f, 0x46, 0xa8, 0x15, 0x18, 0xb2, 0xa1, 0x6b);
static const ble_uuid128_t s_toradio_uuid =
    BLE_UUID128_INIT(0xe7, 0x01, 0x44, 0x12, 0x66, 0x78, 0xdd, 0xa1,
                     0xad, 0x4d, 0x9e, 0x12, 0xd2, 0x76, 0x5c, 0xf7);
static const ble_uuid128_t s_fromradio_uuid =
    BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x78, 0xb8,
                     0xed, 0x11, 0x93, 0x49, 0x9e, 0xe6, 0x55, 0x2c);
static const ble_uuid128_t s_fromnum_uuid =
    BLE_UUID128_INIT(0x53, 0x44, 0xe3, 0x47, 0x75, 0xaa, 0x70, 0xa6,
                     0x66, 0x4f, 0x00, 0xa8, 0x8c, 0xa1, 0x9d, 0xed);

/* ---------- 状态 ---------- */
static uint32_t s_my_node;
static char     s_long[24], s_short[12];
static char     s_ble_name[20];
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_fromnum_handle;
static uint8_t  s_addr_type;

static uint8_t  s_fr[FR_MAX][FR_MAXLEN];
static uint16_t s_fr_len[FR_MAX];
static int      s_fr_head, s_fr_count;
static uint32_t s_fromnum;
static portMUX_TYPE s_mtx = portMUX_INITIALIZER_UNLOCKED;

static ble_node_t s_nodes[16];
static int        s_node_cnt;

/* ---------- protobuf 编码 ---------- */
static int pb_varint(uint8_t *o, uint64_t v)
{
    int n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        o[n++] = b | (v ? 0x80 : 0x00);
    } while (v);
    return n;
}

static int pb_tag(uint8_t *o, int field, int wt)
{
    return pb_varint(o, ((uint64_t)field << 3) | (uint64_t)wt);
}

static int pb_u32(uint8_t *o, int field, uint32_t v)
{
    int n = pb_tag(o, field, 0);
    return n + pb_varint(o + n, v);
}

static int pb_i32(uint8_t *o, int field, int32_t v)
{
    int n = pb_tag(o, field, 0);
    /* 负数按 64 位补码发 10 字节 varint */
    return n + pb_varint(o + n, (uint64_t)(int64_t)v);
}

static int pb_f32(uint8_t *o, int field, float f)
{
    int n = pb_tag(o, field, 5);
    uint32_t v;
    memcpy(&v, &f, 4);
    o[n++] = (uint8_t)(v);
    o[n++] = (uint8_t)(v >> 8);
    o[n++] = (uint8_t)(v >> 16);
    o[n++] = (uint8_t)(v >> 24);
    return n;
}

static int pb_bytes(uint8_t *o, int field, const void *p, int len)
{
    int n = pb_tag(o, field, 2);
    n += pb_varint(o + n, (uint64_t)len);
    if (len) memcpy(o + n, p, (size_t)len);
    return n + len;
}

static int pb_str(uint8_t *o, int field, const char *s)
{
    return pb_bytes(o, field, s, s ? (int)strlen(s) : 0);
}

/* ---------- FromRadio 队列 ---------- */
static void fr_push(const uint8_t *buf, int len)
{
    if (len <= 0 || len > FR_MAXLEN) return;
    bool notify = false;
    portENTER_CRITICAL(&s_mtx);
    if (s_fr_count == FR_MAX) {          /* 丢最旧 */
        s_fr_head = (s_fr_head + 1) % FR_MAX;
        s_fr_count--;
    }
    int idx = (s_fr_head + s_fr_count) % FR_MAX;
    memcpy(s_fr[idx], buf, (size_t)len);
    s_fr_len[idx] = (uint16_t)len;
    s_fr_count++;
    s_fromnum++;
    notify = (s_conn != BLE_HS_CONN_HANDLE_NONE);
    portEXIT_CRITICAL(&s_mtx);

    if (notify) {
        uint8_t v[4] = {(uint8_t)s_fromnum, (uint8_t)(s_fromnum >> 8),
                        (uint8_t)(s_fromnum >> 16), (uint8_t)(s_fromnum >> 24)};
        struct os_mbuf *om = ble_hs_mbuf_from_flat(v, sizeof(v));
        if (om) ble_gatts_notify_custom(s_conn, s_fromnum_handle, om);
    }
}

static int fr_pop(uint8_t *out, int cap)
{
    int n = 0;
    portENTER_CRITICAL(&s_mtx);
    if (s_fr_count > 0) {
        n = s_fr_len[s_fr_head];
        if (n > cap) n = cap;
        memcpy(out, s_fr[s_fr_head], (size_t)n);
        s_fr_head = (s_fr_head + 1) % FR_MAX;
        s_fr_count--;
    }
    portEXIT_CRITICAL(&s_mtx);
    return n;
}

static void fr_reset(void)
{
    portENTER_CRITICAL(&s_mtx);
    s_fr_head = 0;
    s_fr_count = 0;
    portEXIT_CRITICAL(&s_mtx);
}

/* ---------- FromRadio 消息构造 ---------- */
static void fr_my_info(void)
{
    uint8_t in[16], out[32];
    int n = 0;
    n += pb_u32(in + n, 1, s_my_node);       /* MyNodeInfo/MyInfo.my_node_num */
    int m = pb_bytes(out, 3, in, n);         /* FromRadio.my_info */
    fr_push(out, m);
}

static void fr_deviceui(void)
{
    uint8_t out[8];
    int m = pb_bytes(out, 17, NULL, 0);      /* 空 DeviceUIConfig，老 App 当未知字段跳过 */
    fr_push(out, m);
}

static void fr_node_info(uint32_t id, const char *ln, const char *sn,
                         int8_t rssi, int8_t snr4, uint8_t hops, uint32_t last_heard)
{
    uint8_t user[96], ni[160], out[200];
    int un = 0;
    char ids[12];
    snprintf(ids, sizeof(ids), "!%08lx", (unsigned long)id);
    un += pb_str(user + un, 1, ids);
    un += pb_str(user + un, 2, ln);
    un += pb_str(user + un, 3, sn);
    uint8_t mac[6];
    mac[0] = (uint8_t)(id >> 24); mac[1] = (uint8_t)(id >> 16); mac[2] = (uint8_t)(id >> 8);
    mac[3] = (uint8_t)(id);       mac[4] = 0; mac[5] = 0;
    un += pb_bytes(user + un, 4, mac, 6);
    un += pb_u32(user + un, 5, 255);        /* hw_model: 未知/自定义，避免 App 套用别人的引脚能力 */

    int n = 0;
    n += pb_u32(ni + n, 1, id);
    n += pb_bytes(ni + n, 2, user, un);
    n += pb_f32(ni + n, 4, (float)snr4 / 4.0f);
    if (last_heard) n += pb_u32(ni + n, 5, last_heard);
    n += pb_u32(ni + n, 9, hops);
    int m = pb_bytes(out, 4, ni, n);         /* FromRadio.node_info */
    fr_push(out, m);
    (void)rssi;
}

static void fr_metadata(void)
{
    uint8_t md[96], out[120];
    int n = 0;
    n += pb_str(md + n, 1, "c3msg 1.0 (meshtastic-compat)");
    n += pb_u32(md + n, 2, 23);              /* device_state_version */
    n += pb_u32(md + n, 3, 1);               /* canShutdown */
    n += pb_u32(md + n, 4, 1);               /* hasWifi */
    n += pb_u32(md + n, 5, 1);               /* hasBluetooth */
    n += pb_u32(md + n, 7, 0);               /* role CLIENT */
    n += pb_u32(md + n, 9, 255);             /* hw_model */
    int m = pb_bytes(out, 13, md, n);        /* FromRadio.metadata */
    fr_push(out, m);
}

static void fr_channel(void)
{
    uint8_t psk[32], cs[64], ch[80], out[100];
    lora_get_psk(psk);
    int cn = 0;
    cn += pb_bytes(cs + cn, 2, psk, 32);     /* ChannelSettings.psk */
    cn += pb_str(cs + cn, 3, lora_channel_name());

    int n = 0;
    n += pb_u32(ch + n, 1, 0);               /* index */
    n += pb_bytes(ch + n, 2, cs, cn);
    n += pb_u32(ch + n, 3, 1);               /* role PRIMARY */
    int m = pb_bytes(out, 10, ch, n);        /* FromRadio.channel */
    fr_push(out, m);
}

static void fr_config_lora(void)
{
    uint8_t lc[32], cfg[48], out[64];
    int n = 0;
    n += pb_u32(lc + n, 1, 1);               /* use_preset */
    n += pb_u32(lc + n, 2, 1);               /* modem_preset LONG_SLOW */
    n += pb_u32(lc + n, 7, 16);              /* region MY_433 */
    n += pb_u32(lc + n, 8, 3);               /* hop_limit */
    n += pb_u32(lc + n, 9, lora_tx_allowed() ? 1 : 0);
    n += pb_u32(lc + n, 10, (uint32_t)lora_tx_power_dbm());
    n += pb_u32(lc + n, 11, 0);              /* channel_num */
    int m = pb_bytes(cfg, 6, lc, n);         /* Config.lora */
    int mm = pb_bytes(out, 5, cfg, m);       /* FromRadio.config */
    fr_push(out, mm);
}

static void fr_config_bluetooth(void)
{
    uint8_t bc[16], cfg[24], out[32];
    int n = 0;
    n += pb_u32(bc + n, 1, 1);               /* enabled */
    n += pb_u32(bc + n, 2, 0);               /* mode NO_PIN */
    int m = pb_bytes(cfg, 7, bc, n);         /* Config.bluetooth */
    int mm = pb_bytes(out, 5, cfg, m);
    fr_push(out, mm);
}

static void fr_config_device(void)
{
    uint8_t dc[16], cfg[24], out[32];
    int n = 0;
    n += pb_u32(dc + n, 1, 0);               /* role CLIENT */
    n += pb_u32(dc + n, 7, 1800);            /* node_info_broadcast_secs */
    int m = pb_bytes(cfg, 1, dc, n);         /* Config.device */
    int mm = pb_bytes(out, 5, cfg, m);
    fr_push(out, mm);
}

static void fr_complete(uint32_t want_id)
{
    uint8_t out[16];
    int m = pb_u32(out, 7, want_id);         /* FromRadio.config_complete_id */
    fr_push(out, m);
}

static void fr_packet_text(uint32_t from, uint32_t to, uint32_t id, const char *text,
                           int8_t rssi, int8_t snr4, uint8_t hops, uint32_t rx_time)
{
    uint8_t data[96], pkt[160], out[200];
    int dn = 0;
    dn += pb_u32(data + dn, 1, 1);            /* PortNum_TEXT_MESSAGE_APP */
    dn += pb_str(data + dn, 2, text);

    int n = 0;
    n += pb_u32(pkt + n, 1, from);
    n += pb_u32(pkt + n, 2, to);
    n += pb_u32(pkt + n, 3, 0);               /* channel index */
    n += pb_bytes(pkt + n, 4, data, dn);      /* MeshPacket.decoded */
    n += pb_u32(pkt + n, 6, id);
    n += pb_u32(pkt + n, 7, rx_time);
    n += pb_f32(pkt + n, 8, (float)snr4 / 4.0f);
    n += pb_i32(pkt + n, 9, rssi);
    n += pb_u32(pkt + n, 15, hops);           /* hop_start */
    int m = pb_bytes(out, 2, pkt, n);         /* FromRadio.packet */
    fr_push(out, m);
}

/* 完整配置序列（顺序与 PhoneAPI 一致） */
static void queue_config(uint32_t want_id)
{
    fr_reset();
    fr_my_info();
    fr_deviceui();
    fr_node_info(s_my_node, s_long, s_short, 0, 0, 0, 0);
    fr_metadata();
    fr_channel();
    fr_config_lora();
    fr_config_bluetooth();
    fr_config_device();

    ble_node_t snap[16];
    int cnt;
    portENTER_CRITICAL(&s_mtx);
    cnt = s_node_cnt;
    if (cnt > 16) cnt = 16;
    memcpy(snap, s_nodes, sizeof(ble_node_t) * (size_t)cnt);
    portEXIT_CRITICAL(&s_mtx);
    for (int i = 0; i < cnt; i++) {
        uint32_t lh = snap[i].age_s ? (uint32_t)(esp_timer_get_time() / 1000000 - snap[i].age_s) : 0;
        fr_node_info(snap[i].id, snap[i].long_name, snap[i].short_name,
                     snap[i].rssi, snap[i].snr4, snap[i].hops, lh);
    }
    fr_complete(want_id);
    ESP_LOGI(BLE_TAG, "want_config id=%lu -> my_info/ui/self/metadata/ch/%d nodes/complete, queued=%d",
             (unsigned long)want_id, cnt, FR_MAX);
}

/* ---------- ToRadio 解析 ---------- */
static int pb_read_varint(const uint8_t *p, int n, int *pos, uint64_t *v)
{
    uint64_t r = 0;
    int sh = 0;
    while (*pos < n && sh < 64) {
        uint8_t b = p[(*pos)++];
        r |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80)) { *v = r; return 0; }
        sh += 7;
    }
    return -1;
}

/* 解析 MeshPacket 里的 text；返回 1 表示是文本包 */
static int parse_packet(const uint8_t *p, int n, uint32_t *to, char *txt, int cap)
{
    int pos = 0;
    *to = 0xFFFFFFFFu;
    txt[0] = 0;
    int is_text = 0;
    while (pos < n) {
        uint64_t key, v;
        if (pb_read_varint(p, n, &pos, &key)) break;
        int field = (int)(key >> 3), wt = (int)(key & 7);
        if (wt == 0) {
            if (pb_read_varint(p, n, &pos, &v)) break;
            if (field == 2) *to = (uint32_t)v;
        } else if (wt == 2) {
            if (pb_read_varint(p, n, &pos, &v)) break;
            int len = (int)v;
            if (len < 0 || pos + len > n) break;
            if (field == 4) {                       /* MeshPacket.decoded */
                int dp = pos, dend = pos + len;
                int portnum = 0;
                while (dp < dend) {
                    uint64_t dk, dv;
                    if (pb_read_varint(p, dend, &dp, &dk)) break;
                    int df = (int)(dk >> 3), dwt = (int)(dk & 7);
                    if (dwt == 0) {
                        if (pb_read_varint(p, dend, &dp, &dv)) break;
                        if (df == 1) portnum = (int)dv;
                    } else if (dwt == 2) {
                        if (pb_read_varint(p, dend, &dp, &dv)) break;
                        int dl = (int)dv;
                        if (dl < 0 || dp + dl > dend) break;
                        if (df == 2 && portnum == 1) {
                            int cp = dl < cap - 1 ? dl : cap - 1;
                            memcpy(txt, p + dp, (size_t)cp);
                            /* 避免截断 UTF-8 多字节字符 */
                            while (cp > 0 && ((uint8_t)txt[cp] & 0xC0) == 0x80) cp--;
                            txt[cp] = 0;
                            is_text = 1;
                        }
                        dp += dl;
                    } else {
                        break;
                    }
                }
            }
            pos += len;
        } else if (wt == 5) {
            pos += 4;
        } else if (wt == 1) {
            pos += 8;
        } else {
            break;
        }
    }
    return is_text;
}

static void handle_toradio(const uint8_t *p, int n)
{
    int pos = 0;
    while (pos < n) {
        uint64_t key, v;
        if (pb_read_varint(p, n, &pos, &key)) break;
        int field = (int)(key >> 3), wt = (int)(key & 7);
        if (wt == 0) {
            if (pb_read_varint(p, n, &pos, &v)) break;
            if (field == 3) {
                ESP_LOGI(BLE_TAG, "ToRadio want_config id=%lu", (unsigned long)v);
                queue_config((uint32_t)v);
            } else if (field == 4) {
                ESP_LOGI(BLE_TAG, "ToRadio disconnect");
            } else if (field == 7) {
                ESP_LOGI(BLE_TAG, "ToRadio heartbeat");
            }
        } else if (wt == 2) {
            if (pb_read_varint(p, n, &pos, &v)) break;
            int len = (int)v;
            if (len < 0 || pos + len > n) break;
            if (field == 1) {                       /* ToRadio.packet */
                uint32_t to;
                char txt[TXT_MAX + 1];
                if (parse_packet(p + pos, len, &to, txt, sizeof(txt))) {
                    int rc = lora_send_text(to, txt);
                    ESP_LOGI(BLE_TAG, "ToRadio text to=!%08lx len=%d rc=%d",
                             (unsigned long)to, (int)strlen(txt), rc);
                }
            }
            pos += len;
        } else if (wt == 5) {
            pos += 4;
        } else if (wt == 1) {
            pos += 8;
        } else {
            break;
        }
    }
}

/* ---------- GATT ---------- */
static int fromradio_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[FR_MAXLEN];
    int n = fr_pop(buf, sizeof(buf));
    if (n > 0) {
        if (os_mbuf_append(ctxt->om, buf, (uint16_t)n)) return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int fromnum_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint32_t v = s_fromnum;
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return os_mbuf_append(ctxt->om, b, sizeof(b)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int toradio_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0 || len > FR_MAXLEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t buf[FR_MAXLEN];
    if (os_mbuf_copydata(ctxt->om, 0, len, buf)) return BLE_ATT_ERR_UNLIKELY;
    handle_toradio(buf, len);
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    { .uuid = &s_toradio_uuid.u, .access_cb = toradio_access,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &s_fromradio_uuid.u, .access_cb = fromradio_access,
      .flags = BLE_GATT_CHR_F_READ },
    { .uuid = &s_fromnum_uuid.u, .access_cb = fromnum_access,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &s_fromnum_handle },
    { 0 }
};

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_svc_uuid.u, .characteristics = s_chrs },
    { 0 }
};

/* ---------- GAP ---------- */
static void start_adv(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) ESP_LOGW(BLE_TAG, "adv fields rc=%d", rc);

    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)s_ble_name;
    rsp.name_len = (uint8_t)strlen(s_ble_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc) ESP_LOGW(BLE_TAG, "scan rsp rc=%d", rc);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, NULL, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(BLE_TAG, "adv start rc=%d", rc);
    else ESP_LOGI(BLE_TAG, "advertising as '%s'", s_ble_name);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(BLE_TAG, "client connected handle=%d", s_conn);
        } else {
            ESP_LOGW(BLE_TAG, "connect failed status=%d", event->connect.status);
            start_adv();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(BLE_TAG, "client disconnected reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        start_adv();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_adv();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(BLE_TAG, "mtu=%d", event->mtu.value);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(BLE_TAG, "subscribe attr=%d notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) { ESP_LOGE(BLE_TAG, "ensure_addr rc=%d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc) { ESP_LOGE(BLE_TAG, "infer addr rc=%d", rc); return; }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_addr_type, addr, NULL);
    ESP_LOGI(BLE_TAG, "host synced, addr=%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    start_adv();
}

static void on_reset(int reason)
{
    ESP_LOGW(BLE_TAG, "host reset reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------- 对外接口 ---------- */
void ble_mesh_init(uint32_t my_node, const char *long_name, const char *short_name)
{
    s_my_node = my_node;
    snprintf(s_long, sizeof(s_long), "%s", long_name ? long_name : "c3msg");
    snprintf(s_short, sizeof(s_short), "%s", short_name ? short_name : "c3");
    snprintf(s_ble_name, sizeof(s_ble_name), "Meshtastic_%04lx", (unsigned long)(my_node & 0xFFFF));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(BLE_TAG, "nimble_port_init failed: %d", err);
        return;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = NULL;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_ble_name);
    ble_att_set_preferred_mtu(517);

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc) { ESP_LOGE(BLE_TAG, "count_cfg rc=%d", rc); return; }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc) { ESP_LOGE(BLE_TAG, "add_svcs rc=%d", rc); return; }

    nimble_port_freertos_init(host_task);
    ESP_LOGI(BLE_TAG, "init done, name='%s' node=!%08lx", s_ble_name, (unsigned long)my_node);
}

void ble_mesh_set_nodes(const ble_node_t *nodes, int n)
{
    portENTER_CRITICAL(&s_mtx);
    s_node_cnt = n > 16 ? 16 : (n < 0 ? 0 : n);
    if (s_node_cnt) memcpy(s_nodes, nodes, sizeof(ble_node_t) * (size_t)s_node_cnt);
    portEXIT_CRITICAL(&s_mtx);
}

void ble_mesh_on_lora_text(uint32_t from, const char *text, int8_t rssi, int8_t snr4, uint8_t hops)
{
    if (!text || !text[0]) return;
    fr_packet_text(from, 0xFFFFFFFFu, (uint32_t)(esp_timer_get_time() / 1000), text,
                   rssi, snr4, hops, 0);
    ESP_LOGI(BLE_TAG, "queue text from=!%08lx rssi=%d snr=%d.%d", (unsigned long)from,
             rssi, snr4 / 4, (snr4 % 4) * 25);
}

bool ble_mesh_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

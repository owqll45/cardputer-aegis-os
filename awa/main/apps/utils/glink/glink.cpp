#include "glink.h"
#include <string.h>
#include <hal/hal.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "glink";

#define GMK_CH 1   // 两端固定信道

static uint8_t s_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t s_peer[6];
static bool s_linked = false;
static bool s_ok = false;
static uint32_t s_beacon_last = 0;
static uint32_t s_hb_last = 0;
static uint32_t s_last_rx = 0;   // 最近一次收到对端包的时刻(收包超时判定掉线)
static int s_fail = 0;

// 状态包发送: 限频 + 暂存补发
static uint8_t s_tx[240];
static uint8_t s_stash[240];
static int s_stash_len = 0;
static uint32_t s_state_last = 0;
static uint8_t s_seq = 0;

// 联机对战: C3(手柄)控制通道
static uint16_t s_btn = 0;
static uint32_t s_btn_seq = 0;
static uint8_t s_rflags = 0;
static uint32_t s_taunt_seq = 0;

// 当前前台 app 登记(GMKAPP 内容)
static char s_app_name[17] = "Launcher";
static uint8_t s_game_id = GLINK_GID_LAUNCHER;
static uint8_t s_icon_id = GLINK_ICON_GENERIC;

static void add_peer(const uint8_t* mac)
{
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, mac, 6);
    p.channel = GMK_CH;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_del_peer(mac);
    esp_now_add_peer(&p);
}

// GMKAPP: 'G','M',0x10,seq, game_id, icon_id, name[16] 定长24B
static void send_app_info()
{
    if (!s_ok || !s_linked) return;
    uint8_t pkt[24];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 'G'; pkt[1] = 'M'; pkt[2] = 0x10; pkt[3] = ++s_seq;
    pkt[4] = s_game_id;
    pkt[5] = s_icon_id;
    strncpy((char*)pkt + 6, s_app_name, 16);
    esp_now_send(s_peer, pkt, sizeof(pkt));
}

static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_linked && memcmp(info->src_addr, s_peer, 6) == 0) s_last_rx = now;

    if (len >= 6 && memcmp(data, "GMKHI1", 6) == 0) {
        if (!s_linked) {
            memcpy(s_peer, info->src_addr, 6);
            add_peer(s_peer);
            esp_now_send(s_peer, (const uint8_t*)"GMKWELC", 7);
            s_linked = true;
            s_fail = 0;
            s_last_rx = now;
            ESP_LOGI(TAG, "linked %02x:%02x:%02x:%02x:%02x:%02x",
                     s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5]);
            send_app_info(); // 新连接立刻告知当前 app
        }
    } else if (len >= 6 && memcmp(data, "GMKBYE", 6) == 0) {
        if (s_linked && memcmp(info->src_addr, s_peer, 6) == 0) {
            s_linked = false;
            ESP_LOGI(TAG, "bye");
        }
    } else if (len == 11 && memcmp(data, "GMKPIN1", 7) == 0) {
        // C3 的延迟探测: 原样回传时间戳, 不带其他开销
        uint8_t r[11];
        memcpy(r, "GMKPON1", 7);
        memcpy(r + 7, data + 7, 4);
        esp_now_send(info->src_addr, r, sizeof(r));
    } else if (len == 8 && memcmp(data, "GMKBTN", 6) == 0) {
        // 手柄按键掩码(变化才发)
        uint16_t m = (uint16_t)(data[6] | (data[7] << 8));
        if (m != s_btn) {
            s_btn = m;
            s_btn_seq++;
        }
    } else if (len == 7 && memcmp(data, "GMKPVP", 6) == 0) {
        s_rflags = data[6];
    } else if (len == 6 && memcmp(data, "GMKTAU", 6) == 0) {
        s_taunt_seq++;
    }
}

static void on_sent(const wifi_tx_info_t* info, esp_now_send_status_t st)
{
    (void)info;
    if (st != ESP_NOW_SEND_SUCCESS) {
        if (++s_fail > 60) {
            s_fail = 0;
            s_linked = false; // 掉线回广播
            ESP_LOGW(TAG, "link lost");
        }
    } else {
        s_fail = 0;
    }
}

static void send_now(const uint8_t* data, int len)
{
    if (len > 240) len = 240;
    memcpy(s_tx, data, len);
    s_tx[3] = ++s_seq; // 包序号
    esp_now_send(s_peer, s_tx, len);
    s_state_last = (uint32_t)(esp_timer_get_time() / 1000);
}

static void glink_init_internal()
{
    GetHAL().wifiInit(); // STA 已完整启动(幂等, 被 deinit 过也能重新拉起)
    esp_wifi_set_channel(GMK_CH, WIFI_SECOND_CHAN_NONE);
    esp_now_deinit();    // 防御: 残留实例先清掉(未初始化时会报错, 忽略)
    esp_err_t e = esp_now_init();
    if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "espnow init fail %d", e);
        return;
    }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_sent);
    add_peer(s_bcast);
    s_ok = true;
    s_linked = false;
    s_stash_len = 0;
    ESP_LOGI(TAG, "host beacon on ch%d", GMK_CH);
}

void glink_acquire(const char* app_name, uint8_t game_id, uint8_t icon_id)
{
    if (app_name && app_name[0]) {
        strncpy(s_app_name, app_name, 16);
        s_app_name[16] = 0;
    }
    s_game_id = game_id;
    s_icon_id = icon_id;
    // 上一个 app 可能把 WiFi 停掉/切成 AP/重新初始化过:
    // 只要当前不是可用的 STA 状态, 就当作死亡并重建(否则会一直假在线, C3 收不到任何包)
    if (s_ok) {
        wifi_mode_t m;
        if (esp_wifi_get_mode(&m) != ESP_OK || m != WIFI_MODE_STA) {
            s_ok = false;
            s_linked = false;
            s_stash_len = 0;
        }
    }
    if (!s_ok) glink_init_internal(); // WiFi 被杀过/首次使用 → 重建
    send_app_info();                  // 已连接则立即通知(未连接时等心跳补发)
}

void glink_release()
{
    // 不断链不反初始化: launcher 会立刻 acquire 自己
}

void glink_poll()
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    // WiFi 被外部 app deinit 的探测: 标记死亡, 不自动重建(省 RAM),
    // 等下个 app glink_acquire 时再拉起
    if (s_ok) {
        wifi_mode_t m;
        if (esp_wifi_get_mode(&m) != ESP_OK || m != WIFI_MODE_STA) {
            s_ok = false;
            s_linked = false;
            s_stash_len = 0;
            s_btn = 0;
            s_rflags = 0;
            ESP_LOGW(TAG, "wifi deinit by app, link down");
        }
    }
    if (!s_ok) return;

    if (!s_linked) {
        if (now - s_beacon_last > 500) {
            s_beacon_last = now;
            uint8_t mac[6];
            esp_wifi_get_mac(WIFI_IF_STA, mac);
            uint8_t pkt[13];
            memcpy(pkt, "GMKBEAC", 7);
            memcpy(pkt + 7, mac, 6);
            esp_now_send(s_bcast, pkt, 13);
        }
        return;
    }
    // 收包超时: C3 每秒回 GMKACK, 3.5 秒没有任何包判定掉线, 立即回到广播
    if (now - s_last_rx > 3500) {
        s_linked = false;
        s_stash_len = 0;
        s_btn = 0;
        s_rflags = 0;
        ESP_LOGW(TAG, "link timeout, back to beacon");
        return;
    }
    // 补发被限频暂存的状态包
    if (s_stash_len && now - s_state_last > 50) {
        int len = s_stash_len;
        s_stash_len = 0;
        send_now(s_stash, len);
    }
    if (now - s_hb_last > 1000) {
        s_hb_last = now;
        esp_now_send(s_peer, (const uint8_t*)"GMKBEAT", 7);
        send_app_info(); // 心跳顺带重发当前 app 信息, C3 一秒内必定同步
    }
}

bool glink_is_linked() { return s_linked; }

uint16_t glink_buttons() { return s_btn; }
uint32_t glink_btn_seq() { return s_btn_seq; }
uint8_t glink_remote_flags() { return s_rflags; }
uint32_t glink_taunt_seq() { return s_taunt_seq; }

void glink_send_state(const uint8_t* data, int len)
{
    if (!s_ok || !s_linked || !data || len < 4 || len > 240) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_state_last < 50) {
        // 太频繁: 暂存最后一包, poll 里补发
        memcpy(s_stash, data, len);
        s_stash_len = len;
        return;
    }
    s_stash_len = 0;
    send_now(data, len);
}

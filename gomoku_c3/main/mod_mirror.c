/*
 * mod_mirror.c — LoRa 镜像兜底（监视器侧，可插拔模块）
 *
 * 职责：
 *   1) 注册 LoRa 私有 port 0x0102 回调，重组桥侧发来的 GMK 状态帧；
 *   2) 只在 ESP-NOW 主链路 3s 无包时，每秒广播一次 GMKMON 请求兜底；
 *   3) 主链路恢复后停止请求，桥侧 6s 内自动停发，界面无缝切回 ESP-NOW；
 *   4) 高频动作类应用不请求镜像，并在屏幕上明确提示“链路不支持镜像”。
 *
 * 与基座的关系：模块只调用 ui.h 暴露的通用入口（ui_gmk_feed / ui_espnow_alive /
 * ui_set_notice），main.c 不知道本模块存在；拔掉本模块不影响任何原有功能。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "module.h"
#include "ui.h"
#include "lora.h"

static const char *TAG = "mirm";

#define ST_LEN     240
#define CHUNK      120
#define MAX_CHUNKS 2

/* 基座解析状态（重组结果在主任务里喂给 ui_gmk_feed） */
static uint8_t  s_shadow[ST_LEN];
static int      s_have;
static uint8_t  s_full_tmp[ST_LEN];
static uint32_t s_full_mask;
static uint16_t s_full_seq;
static int      s_full_total;
static uint16_t s_seq;
static uint8_t  s_gid = 0xFF;

/* LoRa 接收任务 -> 主任务 的单向交接（双缓冲 + 临界区） */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t  s_pend[2][ST_LEN];
static volatile int s_pend_idx;
static volatile int s_pend_ready;
static volatile int s_app_ready;
static uint8_t  s_pend_gid;
static char     s_pend_app[17];

static volatile int s_rssi = -127;
static uint32_t s_last_frame_ms;
static uint32_t s_n_full, s_n_delta, s_n_bad, s_n_skip;
static uint32_t s_req_last;
static int s_req_on;
static volatile int s_need_full;   /* 差分失序 -> 下次 GMKMON 请求全量 */

/* 与桥侧保持一致：低频状态类才允许镜像 */
static int app_supported(uint8_t gid)
{
    switch (gid) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
    case 8: case 9: case 11: case 12: case 17: case 0xFF:
        return 1;
    default:
        return 0;
    }
}

static const char *gid_name(uint8_t gid)
{
    switch (gid) {
    case 0:  return "Launcher";
    case 1:  return "五子棋";
    case 2:  return "Pong";
    case 3:  return "黑白棋";
    case 4:  return "坦克";
    case 5:  return "UNO";
    case 6:  return "四子棋";
    case 7:  return "跳棋";
    case 8:  return "俄罗斯方块";
    case 9:  return "贪吃蛇";
    case 10: return "打砖块";
    case 11: return "2048";
    case 12: return "扫雷";
    case 13: return "Flappy";
    case 14: return "小蜜蜂";
    case 15: return "赛车";
    case 16: return "FPS";
    case 17: return "将棋";
    default: return "未知";
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void push_pending(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_pend_idx ^= 1;
    memcpy(s_pend[s_pend_idx], s_shadow, ST_LEN);
    s_pend_ready = 1;
    taskEXIT_CRITICAL(&s_mux);
}

/* LoRa 接收任务上下文：只做重组/拷贝，不碰屏 */
static void mirror_lora_cb(const uint8_t *pay, int len, int rssi)
{
    if (!pay || len < 7) return;
    s_rssi = rssi;
    uint8_t kind = pay[1] >> 4;
    uint8_t gid = pay[2];
    uint16_t seq = (uint16_t)(pay[3] | (pay[4] << 8));

    if (kind == 3) {                       /* 应用信息 */
        taskENTER_CRITICAL(&s_mux);
        s_pend_gid = gid;
        memcpy(s_pend_app, pay + 7, 16);
        s_pend_app[16] = 0;
        s_app_ready = 1;
        taskEXIT_CRITICAL(&s_mux);
        return;
    }

    if (kind == 1) {                       /* 全量分块 */
        int idx = pay[5], total = pay[6];
        if (total <= 0 || total > MAX_CHUNKS || idx < 0 || idx >= total) return;
        if (seq != s_full_seq || total != s_full_total) {
            s_full_seq = seq;
            s_full_total = total;
            s_full_mask = 0;
        }
        int off = idx * CHUNK;
        int n = len - 7;
        if (n > CHUNK) n = CHUNK;
        if (off + n > ST_LEN) n = ST_LEN - off;
        if (n <= 0) return;
        memcpy(s_full_tmp + off, pay + 7, (size_t)n);
        s_full_mask |= 1u << idx;
        if (s_full_mask == ((1u << total) - 1)) {
            memcpy(s_shadow, s_full_tmp, ST_LEN);
            s_have = 1;
            s_seq = seq;
            s_need_full = 0;
            s_last_frame_ms = now_ms();
            s_n_full++;
            push_pending();
        }
        return;
    }

    if (kind == 2) {                       /* 差分 */
        if (!s_have) { s_n_skip++; s_need_full = 1; return; }
        if ((uint16_t)(seq - (uint16_t)(s_seq + 1)) != 0) { s_n_bad++; s_need_full = 1; return; }
        int nr = pay[5], p = 7;
        uint8_t tmp[ST_LEN];
        memcpy(tmp, s_shadow, ST_LEN);
        for (int i = 0; i < nr; i++) {
            if (p + 2 > len) { s_n_bad++; return; }
            int off = pay[p], l = pay[p + 1];
            p += 2;
            if (l <= 0 || p + l > len || off + l > ST_LEN) { s_n_bad++; return; }
            memcpy(tmp + off, pay + p, (size_t)l);
            p += l;
        }
        memcpy(s_shadow, tmp, ST_LEN);
        s_seq = seq;
        s_last_frame_ms = now_ms();
        s_n_delta++;
        push_pending();
        return;
    }
}

static int mod_init(void)
{
    memset(s_shadow, 0, sizeof(s_shadow));
    s_have = 0;
    s_seq = 0;
    s_full_mask = 0;
    s_full_seq = 0;
    s_full_total = 0;
    s_last_frame_ms = 0;
    s_n_full = s_n_delta = s_n_bad = s_n_skip = 0;
    s_pend_ready = 0;
    s_app_ready = 0;
    s_rssi = -127;
    s_req_last = 0;
    s_req_on = 0;
    s_need_full = 1;
    lora_set_mirror_cb(mirror_lora_cb);
    ESP_LOGI(TAG, "mirror fallback ready (port 0x0102, chunk=%d)", CHUNK);
    return 0;
}

static void mod_tick(uint32_t now_ms_arg)
{
    uint32_t now = now_ms_arg;
    uint8_t frame[ST_LEN];

    /* 1) 应用/状态帧交给基座原有解析器 */
    if (s_app_ready) {
        uint8_t app[24];
        uint8_t gid;
        char name[17];
        taskENTER_CRITICAL(&s_mux);
        gid = s_pend_gid;
        memcpy(name, s_pend_app, sizeof(name));
        s_app_ready = 0;
        taskEXIT_CRITICAL(&s_mux);
        memset(app, 0, sizeof(app));
        app[0] = 'G'; app[1] = 'M'; app[2] = 0x10; app[4] = gid;
        memcpy(app + 6, name, 16);
        ui_gmk_feed(app, sizeof(app));
        s_gid = gid;
    }
    if (s_pend_ready) {
        taskENTER_CRITICAL(&s_mux);
        memcpy(frame, s_pend[s_pend_idx], ST_LEN);
        s_pend_ready = 0;
        taskEXIT_CRITICAL(&s_mux);
        ui_gmk_feed(frame, ST_LEN);
    }

    /* 2) 请求/停止兜底 + 提示文案 */
    int esp = ui_espnow_alive(now);
    if (esp) {
        ui_set_notice(NULL);
    } else if (!app_supported(s_gid)) {
        static char msg[48];
        snprintf(msg, sizeof(msg), "%s（动作类）", gid_name(s_gid));
        ui_set_notice(msg);
    } else if (s_last_frame_ms && now - s_last_frame_ms <= 6000) {
        ui_set_notice(NULL);           /* 兜底帧正常，直接显示画面 */
    } else {
        ui_set_notice("正在寻找附近 C3 的 LoRa 兜底…");
    }

    /* 3) ESP-NOW 断开且应用允许镜像时，每秒广播一次 GMKMON（12B）；
     *    链路恢复立刻发一帧 flag=0，桥侧马上停发，不用等 6s 超时。 */
    int need = !esp && app_supported(s_gid);
    if (need && now - s_req_last >= 1000) {
        uint8_t b[12] = {'G', 'M', 'K', 'M', 'O', 'N', 1, 0, 0, 0, 0, 0};
        b[6] = (uint8_t)(1 | (s_need_full ? 2 : 0));
        b[7] = s_gid;
        ui_espnow_bcast(b, sizeof(b));
        s_req_last = now;
        s_req_on = 1;
    } else if (!need && s_req_on) {
        uint8_t b[12] = {'G', 'M', 'K', 'M', 'O', 'N', 0, 0, 0, 0, 0, 0};
        b[7] = s_gid;
        ui_espnow_bcast(b, sizeof(b));
        s_req_on = 0;
        s_req_last = now;
    }
}

static void mod_draw(int x, int y, int w, int h)
{
    char line[48];
    int esp = ui_espnow_alive(now_ms());
    int cy = y + 18;
    uint32_t age = s_last_frame_ms ? (now_ms() - s_last_frame_ms) : 0;

    ui_fill(x, y, w, h, UI_BG);
    ui_text_cx(UI_F14, x, w, y + 2, "LoRa 镜像兜底", UI_CYAN);
    ui_hline(x + 8, y + 22, w - 16, UI_TREND);

    snprintf(line, sizeof(line), "主链路: %s", esp ? "ESP-NOW 正常" : "已断开");
    ui_text_cx(UI_F12, x, w, cy, line, esp ? UI_GREEN : UI_ORANGE);
    cy += 20;

    snprintf(line, sizeof(line), "应用: %s", gid_name(s_gid));
    ui_text_cx(UI_F12, x, w, cy, line, UI_WHITE);
    cy += 18;
    snprintf(line, sizeof(line), "镜像: %s", app_supported(s_gid) ? "允许" : "不支持");
    ui_text_cx(UI_F12, x, w, cy, line, app_supported(s_gid) ? UI_GREEN : UI_RED);
    cy += 22;

    snprintf(line, sizeof(line), "全量 %lu  差分 %lu", (unsigned long)s_n_full,
             (unsigned long)s_n_delta);
    ui_text_cx(UI_F10, x, w, cy, line, UI_LGRAY);
    cy += 16;
    snprintf(line, sizeof(line), "丢弃 %lu  失序 %lu", (unsigned long)s_n_skip,
             (unsigned long)s_n_bad);
    ui_text_cx(UI_F10, x, w, cy, line, UI_LGRAY);
    cy += 16;
    snprintf(line, sizeof(line), "RSSI %d dBm  帧龄 %lus", s_rssi,
             (unsigned long)(age / 1000));
    ui_text_cx(UI_F10, x, w, cy, line, UI_DGRAY);
    cy += 20;

    if (!app_supported(s_gid)) {
        ui_text_cx(UI_F12, x, w, h - 40, "链路不支持镜像", UI_ORANGE);
        ui_text_cx(UI_F10, x, w, h - 22, "请靠近主机恢复 ESP-NOW", UI_DGRAY);
    } else if (!esp) {
        ui_text_cx(UI_F10, x, w, h - 22, "等待桥侧 C3 转发…", UI_YELLOW);
    }
}

module_t g_mod_mirror = {
    .id = "mirror",
    .label = "镜像",
    .init = mod_init,
    .tick = mod_tick,
    .draw = mod_draw,
    .on_key = NULL,
    .enabled = 1,
};

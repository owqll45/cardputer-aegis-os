/*
 * mirror.c — LoRa 镜像兜底（桥侧实现）
 *
 * 帧格式（Meshtastic 私有 portnum 0x0102 的 payload）：
 *   [0] 'M'
 *   [1] 高 4 位 kind：1=全量分块 2=差分 3=应用信息；低 4 位 version=1
 *   [2] gid（前台 app id，0xFF=未知）
 *   [3..4] seq（LE）；全量快照的各分块 seq 相同，发完才 +1
 *   [5] 全量=分块号 / 差分=run 数 / 应用=图标 id
 *   [6] 全量=分块总数 / 差分=保留 / 应用=保留
 *   [7..] 数据
 *
 * 差分：把 240B 状态切成若干「连续变化段」，每段 [off u8][len u8][bytes]，
 * 最多 8 段、数据 96B；超过就退回全量分块（120B/块，共 2 块）。
 * 这样一次落子通常只有 16~40B，空口时间约为全量的 1/5。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_log.h"

#include "mirror.h"
#include "lora.h"

static const char *TAG = "mir";

/* 一次性空口实测：烧到 31:D0（14dBm）后自动跑，测完恢复 MIRROR_BENCH=0 重烧。
 * 测的是本固件真实发射路径（含 DIO1 轮询与 300ms 判完成开销），不是理论值。 */
#ifndef MIRROR_BENCH
#define MIRROR_BENCH 0
#endif

#define ST_LEN        240
#define FULL_CHUNK    120    /* 2 块装完 240B：单帧 payload 127B，空口约 3s */
#define MAX_RUNS      8
#define MAX_RUN_DATA  96
#define MIN_GAP_MS    900      /* 镜像帧最小间隔：给文本/时间留出空口 */
#define FULL_PERIOD_MS 60000   /* 至少每 60s 补一次全量，丢差分可自愈 */

static uint8_t  s_state[ST_LEN];
static uint8_t  s_inbuf[ST_LEN];      /* ESP-NOW 回调写入，主任务取走 */
static int      s_in_valid;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t  s_sent[ST_LEN];
static int      s_valid;
static int      s_sent_valid;
static uint16_t s_seq;
static uint8_t  s_gid = 0xFF;
static uint8_t  s_sent_gid = 0xFE;
static char     s_app[17] = "";

static uint32_t s_need_until;      /* 监视器最近一次请求镜像的截止时刻 */
static int      s_need_prev;
static int      s_chunk = -1;      /* >=0 表示全量快照发送中 */
static uint32_t s_last_tx;
static uint32_t s_last_full;

static uint32_t s_n_full, s_n_delta, s_n_drop, s_n_skip;
static uint32_t s_log_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 低频状态类允许兜底；高频动作类（Race/FPS/Breakout/Flappy/Invaders）不镜像 */
int mirror_app_supported(uint8_t gid)
{
    switch (gid) {
    case 0:  /* Launcher */
    case 1:  /* Gomoku */
    case 2:  /* Pong */
    case 3:  /* Reversi */
    case 4:  /* Tank */
    case 5:  /* UNO */
    case 6:  /* Connect4 */
    case 7:  /* Checkers */
    case 8:  /* Tetris */
    case 9:  /* Snake */
    case 11: /* 2048 */
    case 12: /* Minesweeper */
    case 17: /* Shogi */
    case 0xFF:
        return 1;
    default:
        return 0;
    }
}

#if MIRROR_BENCH
static void bench_task(void *arg)
{
    static const int kPay[] = {16, 23, 45, 87, 127, 160};
    uint8_t pay[160];

    vTaskDelay(pdMS_TO_TICKS(8000));
    for (int i = 0; i < (int)(sizeof(kPay) / sizeof(kPay[0])); i++) {
        int n = kPay[i];
        for (int r = 0; r < 2; r++) {
            for (int k = 0; k < n; k++) pay[k] = (uint8_t)(k * 7 + r);
            uint32_t t0 = now_ms();
            int rc, tries = 0;
            do {
                rc = lora_send_mirror(pay, n);
                if (rc) vTaskDelay(pdMS_TO_TICKS(20));
            } while (rc && ++tries < 300);
            if (rc) {
                ESP_LOGW(TAG, "BENCH pay=%dB send failed rc=%d", n, rc);
                break;
            }
            while (!lora_tx_idle()) vTaskDelay(pdMS_TO_TICKS(5));
            uint32_t wall = now_ms() - t0;
            ESP_LOGI(TAG, "BENCH pay=%dB frame=%dB wall=%lums",
                     n, 21 + n, (unsigned long)wall);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
    {   /* 连续帧吞吐：典型差分 45B payload 连发 4 帧 */
        int n = 45;
        for (int k = 0; k < n; k++) pay[k] = (uint8_t)k;
        uint32_t t0 = now_ms();
        for (int r = 0; r < 4; r++) {
            int rc, tries = 0;
            do {
                rc = lora_send_mirror(pay, n);
                if (rc) vTaskDelay(pdMS_TO_TICKS(10));
            } while (rc && ++tries < 300);
            while (!lora_tx_idle()) vTaskDelay(pdMS_TO_TICKS(5));
        }
        uint32_t wall = now_ms() - t0;
        ESP_LOGI(TAG, "BENCH BURST 4x45B total=%lums avg=%lums fps=%.2f",
                 (unsigned long)wall, (unsigned long)(wall / 4),
                 wall ? 4000.0 / (double)wall : 0.0);
    }
    ESP_LOGI(TAG, "BENCH DONE");
    vTaskDelete(NULL);
}
#endif

void mirror_init(void)
{
    memset(s_state, 0, sizeof(s_state));
    memset(s_sent, 0, sizeof(s_sent));
    s_valid = s_sent_valid = 0;
    s_seq = 1;
    s_gid = 0xFF;
    s_sent_gid = 0xFE;
    s_need_until = 0;
    s_need_prev = 0;
    s_chunk = -1;
    s_last_tx = s_last_full = 0;
    s_n_full = s_n_delta = s_n_drop = s_n_skip = 0;
    s_log_ms = now_ms();
    ESP_LOGI(TAG, "mirror ready (port 0x0102, chunk=%dB, max %d runs/%dB)",
             FULL_CHUNK, MAX_RUNS, MAX_RUN_DATA);
#if MIRROR_BENCH
    xTaskCreate(bench_task, "mirbench", 4096, NULL, 3, NULL);
#endif
}

void mirror_on_frame(const uint8_t *d, int len)
{
    if (!d) return;

    /* 监视器的镜像请求（ESP-NOW 广播，不是 LoRa）：
     * "GMKMON" + flags + gid + seq16 + 保留2B */
    if (len == 12 && memcmp(d, "GMKMON", 6) == 0) {
        int need = d[6] & 1;
        int need_full = d[6] & 2;
        uint8_t gid = d[7];
        uint32_t t = now_ms();
        taskENTER_CRITICAL(&s_mux);
        if (need) {
            s_need_until = t + 6000;
            if (!s_need_prev) {          /* 0->1：立刻补一份全量，之后跟差分 */
                s_chunk = -1;
                s_sent_valid = 0;
                s_sent_gid = 0xFE;
            }
            if (need_full) {             /* 监视器报差分失序，强制全量 */
                s_chunk = -1;
                s_sent_valid = 0;
            }
        } else {
            s_need_until = 0;
        }
        s_need_prev = need;
        if (gid != s_gid) {
            s_gid = gid;
            s_sent_valid = 0;
            s_chunk = -1;
        }
        taskEXIT_CRITICAL(&s_mux);
        return;
    }

    if (len < 6 || d[0] != 'G' || d[1] != 'M') return;

    if (len == 24 && d[2] == 0x10) {          /* GMKAPP */
        uint8_t gid = d[4];
        taskENTER_CRITICAL(&s_mux);
        memcpy(s_app, d + 6, 16);
        s_app[16] = 0;
        if (gid != s_gid) {
            s_gid = gid;
            s_sent_valid = 0;
            s_sent_gid = 0xFE;
            s_chunk = -1;
        }
        taskEXIT_CRITICAL(&s_mux);
        return;
    }

    if (len == ST_LEN && d[2] == 0x20) {      /* GMKSTATE 全量状态 */
        taskENTER_CRITICAL(&s_mux);
        memcpy(s_inbuf, d, ST_LEN);
        s_in_valid = 1;
        taskEXIT_CRITICAL(&s_mux);
        return;
    }
}

static int build_delta(uint8_t *pay)
{
    int p = 7, nr = 0, i = 0;

    while (i < ST_LEN && nr < MAX_RUNS) {
        int start, n = 0;
        while (i < ST_LEN && s_state[i] == s_sent[i]) i++;
        if (i >= ST_LEN) break;
        start = i;
        while (i < ST_LEN && s_state[i] != s_sent[i] &&
               n < 32 && p + 2 + n < 7 + MAX_RUN_DATA) {
            i++;
            n++;
        }
        pay[p++] = (uint8_t)start;
        pay[p++] = (uint8_t)n;
        memcpy(pay + p, s_state + start, (size_t)n);
        p += n;
        nr++;
        /* 段与段之间至少跳过一个相同字节，避免把整段切成 32B 的碎片 */
        while (i < ST_LEN && s_state[i] == s_sent[i]) i++;
    }
    for (int k = i; k < ST_LEN; k++)
        if (s_state[k] != s_sent[k]) return -1;   /* 剩下的装不下 -> 退回全量 */

    if (nr == 0) return 0;                        /* 没有变化 */
    pay[0] = 'M';
    pay[1] = 0x21;
    pay[2] = s_gid;
    pay[3] = (uint8_t)(s_seq & 0xFF);
    pay[4] = (uint8_t)(s_seq >> 8);
    pay[5] = (uint8_t)nr;
    pay[6] = 0;
    return p;
}

static int build_full(uint8_t *pay, int idx, int total)
{
    int off = idx * FULL_CHUNK;
    int n = ST_LEN - off;
    if (n > FULL_CHUNK) n = FULL_CHUNK;
    pay[0] = 'M';
    pay[1] = 0x11;
    pay[2] = s_gid;
    pay[3] = (uint8_t)(s_seq & 0xFF);
    pay[4] = (uint8_t)(s_seq >> 8);
    pay[5] = (uint8_t)idx;
    pay[6] = (uint8_t)total;
    memcpy(pay + 7, s_state + off, (size_t)n);
    return 7 + n;
}

static int build_app(uint8_t *pay)
{
    pay[0] = 'M';
    pay[1] = 0x31;
    pay[2] = s_gid;
    pay[3] = (uint8_t)(s_seq & 0xFF);
    pay[4] = (uint8_t)(s_seq >> 8);
    pay[5] = 0;
    pay[6] = 0;
    memcpy(pay + 7, s_app, 16);
    return 23;
}

void mirror_tick(uint32_t t)
{
    uint8_t pay[160];
    int n, total = (ST_LEN + FULL_CHUNK - 1) / FULL_CHUNK;
    int want_full;

    if (t - s_log_ms >= 30000) {
        s_log_ms = t;
        if (s_n_full || s_n_delta || s_n_drop || s_n_skip)
            ESP_LOGI(TAG, "stats full=%lu delta=%lu drop=%lu skip=%lu need=%d",
                     (unsigned long)s_n_full, (unsigned long)s_n_delta,
                     (unsigned long)s_n_drop, (unsigned long)s_n_skip,
                     s_need_until && (int32_t)(t - s_need_until) < 0);
    }

    if (!s_need_until || (int32_t)(t - s_need_until) > 0) return;

    /* 取走 ESP-NOW 回调最新的整帧（避免差分读到一半被新帧改掉） */
    taskENTER_CRITICAL(&s_mux);
    if (s_in_valid) {
        memcpy(s_state, s_inbuf, ST_LEN);
        s_in_valid = 0;
        if (!s_valid) {
            s_valid = 1;
            s_sent_valid = 0;
            s_chunk = -1;
        }
    }
    taskEXIT_CRITICAL(&s_mux);

    if (!s_valid) return;
    if (!mirror_app_supported(s_gid)) {
        s_n_skip++;
        return;
    }
    if (s_last_tx && t - s_last_tx < MIN_GAP_MS) return;

    /* 应用信息改了先补一条，监视器好显示正确的 app 名 */
    if (s_gid != s_sent_gid) {
        n = build_app(pay);
        if (lora_send_mirror(pay, n) == 0) {
            s_sent_gid = s_gid;
            s_last_tx = t;
        } else {
            s_n_drop++;
        }
        return;
    }

    want_full = (s_chunk >= 0) || !s_sent_valid ||
                !s_last_full || (t - s_last_full >= FULL_PERIOD_MS);
    if (!want_full) {
        n = build_delta(pay);
        if (n < 0) {
            want_full = 1;
        } else if (n == 0) {
            return;                      /* 状态没变，不占空口 */
        }
    }

    if (want_full) {
        if (s_chunk < 0) {
            s_chunk = 0;
            s_last_full = t;
        }
        n = build_full(pay, s_chunk, total);
    }

    if (lora_send_mirror(pay, n) != 0) {
        s_n_drop++;
        return;
    }
    s_last_tx = t;

    if (want_full) {
        s_chunk++;
        if (s_chunk >= total) {
            s_chunk = -1;
            memcpy(s_sent, s_state, ST_LEN);
            s_sent_valid = 1;
            s_seq++;
            s_n_full++;
        }
    } else {
        memcpy(s_sent, s_state, ST_LEN);
        s_seq++;
        s_n_delta++;
    }
}

void mirror_get_stats(uint32_t *full, uint32_t *delta, uint32_t *drop, uint32_t *skip)
{
    if (full) *full = s_n_full;
    if (delta) *delta = s_n_delta;
    if (drop) *drop = s_n_drop;
    if (skip) *skip = s_n_skip;
}

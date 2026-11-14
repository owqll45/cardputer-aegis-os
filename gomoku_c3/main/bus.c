/*
 * bus.c — 极简事件总线实现
 *
 * 单生产者/多消费者都能用：队列 + 历史环形缓冲。
 * 保护用 portMUX 自旋锁（临界区里只有一次 memcpy，耗时在微秒级）。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "bus.h"

static QueueHandle_t s_q = NULL;
static StaticQueue_t s_qbuf;
static bus_ev_t s_qstorage[BUS_QUEUE_LEN] __attribute__((aligned(4)));
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bus_ev_t s_hist[BUS_HIST_LEN];
static uint32_t s_hist_seq[BUS_HIST_LEN]; /* 该槽位存的是第几号事件，0=空 */
static uint32_t s_seq = 0;                /* 已发布事件总数 */
static int s_pos = 0;

/* 静态队列：lora 任务与主循环可能同时首次调用，用自旋锁保证只建一次 */
static void bus_lazy_init(void)
{
    if (s_q) return;
    taskENTER_CRITICAL(&s_mux);
    if (!s_q)
        s_q = xQueueCreateStatic(BUS_QUEUE_LEN, sizeof(bus_ev_t), (uint8_t*)s_qstorage, &s_qbuf);
    taskEXIT_CRITICAL(&s_mux);
}

void bus_publish(const bus_ev_t* ev)
{
    if (!ev || ev->type == EV_NONE) return;
    bus_lazy_init();

    if (s_q) {
        bus_ev_t dropped;
        /* 队列满则丢最旧的一条，保证主循环永远拿到最新事件 */
        if (xQueueSend(s_q, ev, 0) != pdTRUE) {
            xQueueReceive(s_q, &dropped, 0);
            xQueueSend(s_q, ev, 0);
        }
    }

    taskENTER_CRITICAL(&s_mux);
    s_seq++;
    s_hist[s_pos] = *ev;
    s_hist_seq[s_pos] = s_seq;
    s_pos = (s_pos + 1) % BUS_HIST_LEN;
    taskEXIT_CRITICAL(&s_mux);
}

int bus_poll(bus_ev_t* out, uint32_t now_ms)
{
    (void)now_ms;
    if (!out) return 0;
    bus_lazy_init();
    if (!s_q) return 0;
    return xQueueReceive(s_q, out, 0) == pdTRUE ? 1 : 0;
}

uint32_t bus_seq(void)
{
    uint32_t v;
    taskENTER_CRITICAL(&s_mux);
    v = s_seq;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

int bus_history_get(uint32_t seq, bus_ev_t* out)
{
    int ok = 0;
    if (!out || seq == 0) return 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < BUS_HIST_LEN; i++) {
        if (s_hist_seq[i] == seq) {
            *out = s_hist[i];
            ok = 1;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

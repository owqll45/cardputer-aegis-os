/*
 * 轻量性能打点
 * 用途: 启动阶段耗时 / 帧率 / 按键延迟 / 堆水位的统一测量口径。
 * 输出全部带 "PERF" tag，PC 端直接 grep 即可对比改前改后。
 */
#pragma once

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

// 上电以来的毫秒数（含 bootloader 之后的 ROM 时间）
static inline uint32_t perf_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline uint32_t perf_free_kb()
{
    return (uint32_t)(esp_get_free_heap_size() / 1024);
}

static inline uint32_t perf_largest_kb()
{
    return (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024);
}

// 启动里程碑: boot <ms> <tag> free=<KB> largest=<KB>
static inline void perf_boot(const char* tag)
{
    ESP_LOGI("PERF", "boot %u ms %s free=%uKB largest=%uKB",
             (unsigned)perf_ms(), tag,
             (unsigned)perf_free_kb(), (unsigned)perf_largest_kb());
}

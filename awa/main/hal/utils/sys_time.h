/*
 * sys_time.h - 时区应用（系统设置里的"时区"）
 *
 * 全系统之前从没调用过 setenv("TZ")/tzset()，本地时间实际是 UTC。
 * 这里把偏移量写进 TZ 环境变量，Clock 等用 localtime() 的地方立刻跟着变。
 * 注意 POSIX 的符号是反的：UTC+8 要写成 "UTC-8"。
 */
#pragma once
#include <cstdio>
#include <cstdlib>
#include <ctime>

inline void applyTimeZone(int offset_hours)
{
    if (offset_hours < -12) offset_hours = -12;
    if (offset_hours > 14) offset_hours = 14;

    char tz[16];
    if (offset_hours == 0) {
        snprintf(tz, sizeof(tz), "UTC0");
    } else if (offset_hours > 0) {
        snprintf(tz, sizeof(tz), "UTC-%d", offset_hours);
    } else {
        snprintf(tz, sizeof(tz), "UTC+%d", -offset_hours);
    }
    setenv("TZ", tz, 1);
    tzset();
}

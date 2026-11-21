/*
 * ui_theme.h - 全局强调色（系统设置里的"配色"）
 *
 * 设置写入 NVS 键 accent（0..4），这里做一次缓存，避免每帧读 NVS。
 * 改动后调用 uiAccentRefresh() 让下次取色重新读。
 */
#pragma once
#include <M5GFX.h>
#include <hal/hal.h>

#define UI_ACCENT_COUNT 5

inline uint16_t uiAccentPalette(int idx)
{
    switch (idx) {
        case 1:  return TFT_GREEN;
        case 2:  return TFT_ORANGE;
        case 3:  return TFT_MAGENTA;
        case 4:  return TFT_PINK;
        default: return TFT_CYAN;
    }
}

inline const char* uiAccentName(int idx)
{
    static const char* names[UI_ACCENT_COUNT] = {"青色", "绿色", "橙色", "紫色", "粉色"};
    if (idx < 0 || idx >= UI_ACCENT_COUNT) idx = 0;
    return names[idx];
}

inline int& uiAccentCacheRef()
{
    static int idx = -1;
    return idx;
}

inline int uiAccentIndex()
{
    int& idx = uiAccentCacheRef();
    if (idx < 0) {
        idx = GetHAL().getSettings().GetInt("accent", 0);
        if (idx < 0 || idx >= UI_ACCENT_COUNT) idx = 0;
    }
    return idx;
}

inline uint16_t uiAccentColor()
{
    return uiAccentPalette(uiAccentIndex());
}

inline void uiAccentRefresh()
{
    uiAccentCacheRef() = -1;
}

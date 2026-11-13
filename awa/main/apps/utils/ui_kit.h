/*
 * ui_kit.h - 统一 UI 组件（header-only）
 *
 * 所有应用的界面统一使用本组件绘制：
 *   标题栏(22px) / 内容卡片 / 大数值 / 进度条 / 底部键位提示 / 引导提示页
 *
 * 约定画布尺寸 240x109（共享 canvas），推屏一律用 GetHAL().pushCanvas()。
 */
#pragma once

#include <M5GFX.h>

// ---------------- 统一配色 ----------------
#define UI_BG       0x0841   // 近黑背景
#define UI_CARD     0x18E3   // 卡片底
#define UI_CARD_HI  0x2965   // 选中卡片
#define UI_TEXT     TFT_WHITE
#define UI_SUB      0x8C71   // 次要文字
#define UI_ACCENT   TFT_CYAN
#define UI_WARN     TFT_ORANGE
#define UI_OK       TFT_GREEN
#define UI_BAD      TFT_RED
#define UI_LINE     0x4208

#define UI_TITLE_H  22       // 标题栏高度
#define UI_HINT_H   13       // 底部提示高度（efontCN_12，与五子棋同字体）

// 统一字体：全部使用 efontCN 系列（中文点阵字库完整，不会缺笔画）
//   efontCN_14 —— 大数值 / 强调（与五子棋同款）
//   efontCN_12 —— 正文 / 标签 / 提示
#define UI_FONT_BIG    (&fonts::efontCN_14)
#define UI_FONT_BODY   (&fonts::efontCN_12)

// ---------------- 标题栏 ----------------
static inline void ui_title(LGFX_Sprite& cv, const char* title)
{
    cv.fillRect(0, 0, cv.width(), UI_TITLE_H, UI_BG);
    cv.fillRect(0, 0, 3, UI_TITLE_H, UI_ACCENT);
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(UI_TEXT);
    cv.setCursor(9, (UI_TITLE_H - 12) / 2 + 1);
    cv.print(title);
    cv.drawFastHLine(0, UI_TITLE_H, cv.width(), UI_LINE);
}

// ---------------- 底部键位提示 ----------------
static inline void ui_hint(LGFX_Sprite& cv, const char* text)
{
    cv.setFont(UI_FONT_BODY);
    cv.setTextColor(0x6B4D);
    cv.setCursor(5, cv.height() - UI_HINT_H + 1);
    cv.print(text);
}

// ---------------- 卡片 ----------------
static inline void ui_card(LGFX_Sprite& cv, int x, int y, int w, int h, bool hi = false)
{
    cv.fillRoundRect(x, y, w, h, 6, hi ? UI_CARD_HI : UI_CARD);
    cv.drawRoundRect(x, y, w, h, 6, hi ? UI_ACCENT : UI_LINE);
}

// ---------------- 键值行（左标签 右数值） ----------------
static inline void ui_kv(LGFX_Sprite& cv, int x, int y, int w, const char* label, const char* value,
                         uint16_t value_color = UI_TEXT)
{
    cv.setFont(UI_FONT_BODY);
    cv.setTextColor(UI_SUB);
    cv.setCursor(x, y);
    cv.print(label);

    cv.setTextColor(value_color);
    int vw = cv.textWidth(value);
    cv.setCursor(x + w - vw, y);
    cv.print(value);
}

// ---------------- 大数值（居中，单位小字） ----------------
static inline void ui_big_value(LGFX_Sprite& cv, int cx, int y, const char* value,
                                const char* unit = nullptr, uint16_t color = UI_TEXT)
{
    cv.setFont(UI_FONT_BIG);
    int vw = cv.textWidth(value);
    int uw = 0;
    if (unit) {
        cv.setFont(UI_FONT_BODY);
        uw = cv.textWidth(unit) + 3;
    }
    int x0 = cx - (vw + uw) / 2;

    cv.setFont(UI_FONT_BIG);
    cv.setTextColor(color);
    cv.setCursor(x0, y);
    cv.print(value);

    if (unit) {
        cv.setFont(UI_FONT_BODY);
        cv.setTextColor(UI_SUB);
        cv.setCursor(x0 + vw + 3, y + 2);
        cv.print(unit);
    }
}

// ---------------- 进度 / 信号条 ----------------
static inline void ui_bar(LGFX_Sprite& cv, int x, int y, int w, int h, float ratio, uint16_t color)
{
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    cv.fillRoundRect(x, y, w, h, h / 2, 0x2104);
    int fw = (int)(w * ratio);
    if (fw > 0) cv.fillRoundRect(x, y, fw, h, h / 2, color);
}

// ---------------- 引导提示页（未接入传感器 / 内存不足等） ----------------
static inline void ui_msg(LGFX_Sprite& cv, const char* title, const char* line1,
                          const char* line2 = nullptr, uint16_t accent = UI_WARN)
{
    int w = cv.width();
    int h = cv.height();
    cv.fillScreen(UI_BG);

    // 顶部警示图标：圆环 + 感叹号
    int cx = w / 2;
    int cy = 30;
    cv.drawCircle(cx, cy, 13, accent);
    cv.drawCircle(cx, cy, 12, accent);
    cv.fillRect(cx - 1, cy - 8, 3, 11, accent);
    cv.fillRect(cx - 1, cy + 5, 3, 3, accent);

    cv.setFont(UI_FONT_BODY);
    cv.setTextColor(accent);
    cv.setTextDatum(top_center);
    cv.drawString(title, cx, 50);

    cv.setTextColor(UI_SUB);
    cv.drawString(line1, cx, 68);
    if (line2) cv.drawString(line2, cx, 83);
    cv.setTextDatum(top_left);

    cv.setTextColor(0x6B4D);
    cv.setTextDatum(bottom_center);
    cv.drawString("接好后自动恢复    G0 返回", cx, h - 2);
    cv.setTextDatum(top_left);
}

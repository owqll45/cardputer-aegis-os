/*
 * ui.h — 模块外壳绘图 API
 *
 * 基座 main.c 的帧缓冲/字体/推屏都是 static 的，模块文件不能直接访问。
 * 这里声明一组薄封装（实现在 main.c），模块只用这套 API 往给定矩形里画，
 * 不触碰基座任何渲染逻辑。
 *
 * 所有坐标都是屏幕绝对坐标；ui_clip() 之后的绘制会被裁到矩形内。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t ui_color_t;

/* 颜色索引与基座 kPal 一一对应（顺序不可改） */
enum {
    UI_BLACK = 0, UI_WHITE, UI_RED, UI_GREEN, UI_BLUE, UI_YELLOW, UI_CYAN,
    UI_GRAY, UI_DGRAY, UI_LGRAY, UI_ORANGE, UI_MAGENTA, UI_BG, UI_WOOD,
    UI_PANEL, UI_TREND, UI_TMID, UI_SELBG, UI_DGREEN, UI_BRICK, UI_STEEL,
};

/* 字体（定义在 efont_data.c） */
extern const uint8_t lgfx_efont_cn_10_b[];
extern const uint8_t lgfx_efont_cn_12[];
extern const uint8_t lgfx_efont_cn_14[];
#define UI_F10 lgfx_efont_cn_10_b
#define UI_F12 lgfx_efont_cn_12
#define UI_F14 lgfx_efont_cn_14

/* 屏幕尺寸（ST7789 横屏 320x240，与基座一致） */
#define UI_W 320
#define UI_H 240

void ui_clip(int x, int y, int w, int h); /* 设定裁剪矩形（绝对坐标） */
void ui_clip_reset(void);
void ui_clear(ui_color_t c); /* 只清帧缓冲，不推屏 */

void ui_px(int x, int y, ui_color_t c);
void ui_fill(int x, int y, int w, int h, ui_color_t c);
void ui_rect(int x, int y, int w, int h, ui_color_t c);
void ui_hline(int x, int y, int n, ui_color_t c);
void ui_vline(int x, int y, int n, ui_color_t c);
void ui_line(int x0, int y0, int x1, int y1, ui_color_t c);
void ui_disc(int cx, int cy, int r, ui_color_t c);

int  ui_text(const uint8_t* font, int x, int y, const char* s, ui_color_t c);
void ui_text_cx(const uint8_t* font, int x0, int w, int y, const char* s, ui_color_t c);
int  ui_text_w(const uint8_t* font, const char* s);

void ui_push(void);         /* 整屏推一次（内部分块 DMA，不闪） */
void ui_present_now(void);  /* 立刻按渲染分流重画一帧（镜像页或外壳页） */
void ui_open_settings(void); /* 打开基座原有设置菜单 */

/* ---- 镜像兜底模块用的通用入口（基座不感知具体模块） ---- */
/* 把一帧 GMK 状态/应用帧喂给基座原有解析与渲染（LoRa 兜底通道用） */
void ui_gmk_feed(const uint8_t *d, int len);
/* ESP-NOW 主链路 3s 内是否有包（镜像模块据此决定是否请求兜底） */
int  ui_espnow_alive(uint32_t now_ms);
/* 主链路断开且应用不支持镜像时显示提示；传 NULL 清除 */
void ui_set_notice(const char *text);
/* ESP-NOW 广播（模块用，例如镜像兜底请求；基座不解析内容） */
void ui_espnow_bcast(const void *data, int len);

#ifdef __cplusplus
}
#endif

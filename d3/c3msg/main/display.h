#pragma once

#include <stdint.h>
#include <stdbool.h>

/* 8bit 调色板索引；实际 565 值在 display.c 的 kPal565[] 里，两种屏共用 */
typedef uint8_t color_t;

enum {
    C_BG = 0,
    C_BAR,
    C_LINE,
    C_FG,
    C_DIM,
    C_IN,
    C_OUT,
    C_WARN,
    C_ERR,
    C_SEL,
    /* 五子棋镜像页专用（追加在末尾，已有索引不变） */
    C_BOARD,        /* 棋盘木色 */
    C_STONE_B,      /* 黑子 */
    C_STONE_W,      /* 白子 */
    C_CYAN,
    C_MAGENTA,
    C_ORANGE,
    /* 最小验证专用：纯红/纯绿/纯蓝（走与 UI 完全相同的 fill+flush 通路） */
    C_RGB_R,
    C_RGB_G,
    C_RGB_B,
    C_COUNT
};

/* 屏参描述：新增面板只加一条 profile，不改绘图代码 */
typedef struct {
    const char *name;
    int      phys_w, phys_h;     /* 面板物理像素 */
    int      scr_w, scr_h;       /* 逻辑像素（横持） */
    int      off_x, off_y;       /* 窗口偏移 */
    uint8_t  madctl;             /* 0x36 实际值 */
    bool     invert;             /* true=INVON(0x21), false=INVOFF(0x20) */
    bool     swap_rb;            /* 面板按 BGR 解释 565 时需要交换 R/B */
    bool     sw_rotate;          /* 小屏：逻辑(x,y) -> 物理(y, H-1-x) */
} panel_profile_t;

extern const panel_profile_t *g_panel;
extern int g_scr_w, g_scr_h;

#define SCR_W g_scr_w
#define SCR_H g_scr_h

/* 引脚（两块面板同一条 SPI2 总线） */
#define PIN_SCLK       10
#define PIN_MOSI       7
#define PIN_MISO       6
#define PIN_LCD_CS     1
#define PIN_LCD_DC     0
#define PIN_LCD_BL     13

/* 初始化：探测屏型 -> 选 profile -> 按 profile 初始化面板 -> 分配帧缓冲 */
void display_init(void);

/* 整帧推送（窗口 0..phys-1，MSB-first，按 profile 决定是否交换 R/B） */
void display_flush(void);

/* 运行时覆盖窗口偏移（小屏列/行偏移自检用；产线走 profile 里的值） */
void display_set_offsets(int off_x, int off_y);

/* 逻辑坐标写点/读点，内部按 profile 做旋转与边界裁剪 */
void display_put(int x, int y, color_t c);
color_t display_get(int x, int y);
void display_fill(color_t c);

/* 帧缓冲自检：@FBRLE tag w h order，按逻辑坐标、面板实际 565 值输出 */
void display_dump(const char *tag);

/* 送屏最小验证：全红/全绿/全蓝各 2 秒循环，永不返回。
 * 只用 display_fill()+display_flush()，与 UI 走同一条推送通路。
 * 必须由人眼确认屏上是纯正的红绿蓝；dump 不作数。 */
void display_rgb_cycle(void);

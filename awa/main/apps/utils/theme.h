/**
 * @file theme_define.h
 * @author Forairaaaaa
 * @brief 
 * @version 0.6
 * @date 2023-09-20
 * 
 * @copyright Copyright (c) 2023
 * 
 */
#pragma once

#define THEME_COLOR_BG                      (uint32_t)(0x333333)
#define THEME_COLOR_SYSTEM_BAR              (uint32_t)(0x99FF00)
#define THEME_COLOR_SYSTEM_BAR_TEXT         TFT_BLACK
#define THEME_COLOR_KB_BAR                  TFT_LIGHTGREY
#define THEME_COLOR_KB_BAR_ICON_BG          TFT_DARKGREY
#define THEME_COLOR_KB_BAR_ICON_TEXT        TFT_WHITE
#define THEME_COLOR_ICON                    (uint32_t)(0xE6E6E6)
#define ICON_WIDTH                          48
#define ICON_GAP                            20
#define ICON_MARGIN_TOP                     20
#define ICON_TAG_MARGIN_TOP                 5
#define ICON_SELECTED_WIDTH                 64
#define FONT_BASIC                          &fonts::efontCN_10
#define FONT_HEIGHT                         16

#define THEME_COLOR_KB_CAPS_LOCK            TFT_SKYBLUE
#define THEME_COLOR_KB_ALT                  TFT_YELLOW
#define THEME_COLOR_KB_CTRL                 TFT_PINK
#define THEME_COLOR_KB_FN                   TFT_ORANGE
#define THEME_COLOR_KB_OPT                  TFT_DARKGREEN

/* 链路来源配色(状态栏 / 通知条 / 面板共用, 与 C3/LoRa 节点同口径)
 * 默认: ESP-NOW = 青, LoRa = 橙; 改配色只改这三行 */
#define THEME_COLOR_LINK_ESPNOW             (uint32_t)(0x07FF)
#define THEME_COLOR_LINK_LORA               (uint32_t)(0xFD20)
#define THEME_COLOR_LINK_NONE               (uint32_t)(0x7BEF)

#define THEME_COLOR_REPL_TEXT               TFT_WHITE
// #define FONT_REPL                           &fonts::Font0
#define FONT_REPL                           &fonts::efontCN_16
#define FONT_SIZE_REPL                      1
// #define FONT_REPL_WIDTH                     _canvas->fontWidth()
#define FONT_REPL_WIDTH                     8
#define FONT_REPL_HEIGHT                    16


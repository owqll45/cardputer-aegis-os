#pragma once

#include <lgfx/v1/LGFX_Sprite.hpp>

/*
 * 全局通知层：任何 app 在前台时，收到 LoRa / 经桥转来的消息都在顶部系统栏画一条提示。
 *
 * 三条硬规矩（与 ui_toast 一致）：
 * 1. post（zdlink 的 WiFi 任务里）只把内容拷进临界区变量：不绘制、不分配、不阻塞；
 * 2. update / draw 只在 app 主任务里跑；
 * 3. 不新开画布，直接复用 240x26 的系统栏画布——每次推屏前叠加绘制，超时后恢复成简洁状态栏。
 *    堆紧张时自动降级成"一行：来源 + 未读数"，不向用户提示内存不足。
 */
namespace ui_notify {

// 注册到 zdlink 的事件钩子（只拷数据）
void init();

// app_task 每帧调用：处理新通知、推屏、超时恢复
void update();

// 通知条是否正在显示
bool active();

// 由 DisplayManager::pushSystemBar() 在推屏前调用（任何任务都可能调到，只碰画布）
void draw(lgfx::LGFX_Sprite& bar);

// 全屏 app 用：把通知压成一行画进顶部 kInlineH 条里(不覆盖游戏内容)
void draw_inline(lgfx::LGFX_Sprite& bar);

// 未读计数：消息面板进入列表页/打开时清零
uint16_t unread();
void clear_unread();

// '=' / '+' 跳转：键盘回调只置标志，app 任务里真正打开面板
void request_open();
bool take_open_request();

// 消息面板在前台时不必再盖它自己的顶栏：静默丢弃本次通知（未读仍然计数）
void set_suppressed(bool on);

// 自测用：临时抬高/恢复降级阈值（0 = 用默认 46KB），用于实机取证降级样式
void set_low_heap(uint32_t bytes);

// 取证/自测查询
bool simple_mode();          // 当前这条是否走了降级样式
const char* current_text();  // 当前提示的摘要
uint32_t count_rx();         // 钩子收到的文本条数
uint32_t count_shown();      // 真正弹了几次
uint32_t count_skip();       // 面板在前台被跳过几次
uint32_t count_simple();     // 其中降级的次数

}  // namespace ui_notify

#pragma once
/* 全局顶栏（链路来源 / 信号 / 电量）
 *
 * 两种画法，按前台画布自动切换：
 *  - 全屏 app/游戏：内容画布高度 = 屏高 - kInlineH，app 自己 pushSprite 到 y=kInlineH。
 *    顶部这 kInlineH 条由 drawInline() 画，属于画面排版的一部分，不再用 26px 系统栏
 *    盖在游戏内容上（这就是之前"重叠两份"的根因）。
 *  - 普通 109 高 app：仍复用 240x26 系统栏画布，由 pushSystemBar 叠加。
 *
 * 换配色只改 apps/utils/theme.h 里的 THEME_COLOR_LINK_*。
 */
#include <cstdint>

namespace ui_sysbar {

/* 全屏 app 顶部内联条的高度（内容画布让出这么多） */
constexpr int kInlineH = 14;

void init();          // 注册成系统栏叠加层（和通知层是互斥的两层）
void update();        // app 任务每帧调用
void invalidate();    // 标记顶栏内容失效：下一次 pushSystemBar 会重画（桌面用）
void drawInline();    // 全屏 app 推屏前调用：按需重画顶部内联条（只占 kInlineH）
void afterFrame();    // 全屏 app 推屏后调用：通知活跃时把通知条压在最上层
bool inlineMode();    // 前台画布是否为"全屏 app 画布"
/* 全屏画布登记：app_canvas 建好 sprite 后调用。有的 app 用自己的 sprite、
 * 共享画布是 0x0，只看共享画布会误判成"没有全屏画布"(顶栏就不画了)。 */
void noteAppCanvas(const void* sprite, int h);
void clearAppCanvas(const void* sprite);
const void* appCanvasSprite();  // 帧缓冲取证用：当前全屏画布(可能是 app 自己的 sprite)
int appCanvasHeight();          // 当前全屏画布高度(自测断言用)

void force_link(uint8_t link, uint32_t ms);  // 自测：临时强制显示某条链路
void refresh_now();                         // 自测：立刻重采链路/RSSI/电量并重画
uint8_t link();
int8_t rssi();
uint8_t battery();
uint32_t pushes();       // 系统栏(26px)被推的次数
uint32_t inlineDraws();  // 顶部内联条被重画的次数
uint32_t statusDraws();  // 26px 统一栏被真正落笔的次数（桌面单绘制者的取证）

}  // namespace ui_sysbar

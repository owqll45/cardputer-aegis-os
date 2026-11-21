#pragma once
#include <string>
#include <cstring>
#include <esp_system.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <apps/utils/theme.h>
#include <apps/utils/ui_sysbar/ui_sysbar.h>

/*
 * 画布统一收口。
 * 全屏 app 的内容画布高度 = 屏高 - ui_sysbar::kInlineH（顶部内联状态条由 ui_sysbar 画，
 * 不属于 app 内容），8bpp 只占 29KB，优先申请；只有真的申请失败才退到 109 高。
 * 以前每个 app 各写一套 createSprite 兜底，降级时既没有日志、行为也不一致，
 * 于是出现"画面底部缺一截"却查不到原因。这里统一成一处，并打印剩余堆。
 */
inline int appCanvasTargetH()
{
    return (int)GetHAL().display().height() - ui_sysbar::kInlineH;
}

inline bool appCanvasBegin(LGFX_Sprite* c, const std::string& tag)
{
    if (!c) return false;
    c->deleteSprite();
    c->setColorDepth(8);
    c->setPsram(false);
    c->setTextDatum(top_left);
    c->setFont(FONT_BASIC);  // 默认中文字体，避免 app 忘记 setFont 时画出豆腐块
    if (c->createSprite(GetHAL().display().width(), appCanvasTargetH())) {
        ui_sysbar::noteAppCanvas(c, (int)c->height());
        return true;
    }
    mclog::tagError(tag, "full-screen sprite failed (heap={}), fallback 109",
                    (unsigned)esp_get_free_heap_size());
    c->createSprite(GetHAL().display().width(), 109);
    ui_sysbar::noteAppCanvas(c, (int)c->height());
    return false;
}

// 共享画布（GetHAL().canvas()）版本
inline bool appCanvasBegin(const std::string& tag)
{
    return appCanvasBegin(&GetHAL().canvas(), tag);
}

/* 这块画布是不是"让出了顶部内联条"的全屏画布 */
inline bool appCanvasFits(const LGFX_Sprite* c)
{
    return c && c->getBuffer() && c->height() >= appCanvasTargetH();
}

/* 全屏 app 统一推屏：顶部 kInlineH 条归 ui_sysbar，内容从它下面开始，互不重叠 */
inline void appCanvasPush(LGFX_Sprite* c)
{
    if (!c || !c->getBuffer()) return;
    if (!appCanvasFits(c)) {
        /* 堆不足降级成 109 高：这时顶栏还是 26px 系统栏，内容必须从 26 开始推，
         * 不然系统栏会盖住游戏自己的顶部(老毛病)。109+26 = 135，正好铺满。 */
        int y = (int)GetHAL().display().height() - (int)c->height();
        if (y < 0) y = 0;
        c->pushSprite(0, y);
        return;
    }
    ui_sysbar::drawInline();
    c->pushSprite(0, ui_sysbar::kInlineH);
    ui_sysbar::afterFrame();
}

// 退出时恢复系统画布：HAL 默认是 16bpp / 240x109（Launcher 依赖这个尺寸）
inline void appCanvasRestore(const std::string& tag)
{
    extern const char* zd_fg_app();
    LGFX_Sprite* c = &GetHAL().canvas();
    /* mooncake 的 close 是异步的：旧 app 的 onClose 可能晚于新 app 的 onOpen。
     * 这时共享画布已经被新 app 改成 121 高的全屏画布, 再删就会把新 app 的画面整块抹掉
     * (实测: 从 FPS 快速切到 Gomoku 时 Gomoku 直接白屏)。 */
    const char* fg = zd_fg_app();
    if (fg && strcmp(fg, tag.c_str()) != 0 && strcmp(fg, "Launcher") != 0) return;
    ui_sysbar::clearAppCanvas(c);
    c->deleteSprite();
    c->setColorDepth(16);
    if (!c->createSprite(GetHAL().display().width(), 109)) {
        mclog::tagError(tag, "restore system canvas failed (heap={})",
                        (unsigned)esp_get_free_heap_size());
    }
}

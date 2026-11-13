#pragma once
#include <string>
#include <esp_system.h>
#include <hal/hal.h>
#include <mooncake_log.h>

/*
 * 画布统一收口。
 * 全屏 240x135 @8bpp 只占 32.4KB，优先申请；只有真的申请失败才退到 109 高。
 * 以前每个 app 各写一套 createSprite 兜底，降级时既没有日志、行为也不一致，
 * 于是出现"画面底部缺一截"却查不到原因。这里统一成一处，并打印剩余堆。
 */
inline bool appCanvasBegin(LGFX_Sprite* c, const std::string& tag)
{
    if (!c) return false;
    c->deleteSprite();
    c->setColorDepth(8);
    c->setPsram(false);
    if (c->createSprite(GetHAL().display().width(), GetHAL().display().height())) {
        return true;
    }
    mclog::tagError(tag, "full-screen sprite failed (heap={}), fallback 109",
                    (unsigned)esp_get_free_heap_size());
    c->createSprite(GetHAL().display().width(), 109);
    return false;
}

// 共享画布（GetHAL().canvas()）版本
inline bool appCanvasBegin(const std::string& tag)
{
    return appCanvasBegin(&GetHAL().canvas(), tag);
}

// 退出时恢复系统画布：HAL 默认是 16bpp / 240x109（Launcher 依赖这个尺寸）
inline void appCanvasRestore(const std::string& tag)
{
    LGFX_Sprite* c = &GetHAL().canvas();
    c->deleteSprite();
    c->setColorDepth(16);
    if (!c->createSprite(GetHAL().display().width(), 109)) {
        mclog::tagError(tag, "restore system canvas failed (heap={})",
                        (unsigned)esp_get_free_heap_size());
    }
}

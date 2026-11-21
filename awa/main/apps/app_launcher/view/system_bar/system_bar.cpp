/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "../../app_launcher.h"
#include <apps/utils/ui_sysbar/ui_sysbar.h>
#include <hal/hal.h>

/* 桌面顶栏不再由 Launcher 自绘。
 *
 * 以前这里画的是另一套 240x26 顶栏(矩阵雨 + MEM + WiFi 格), 而全局 ui_sysbar 的
 * overlay 也会往同一块 canvasSystemBar 上画统一栏(E 青/L 橙 + 时间 + 电量)。
 * 两个绘制者轮流覆盖: ui_sysbar 只在数据变脏时画, 其余时候推的是 Launcher 刚画的内容,
 * 表现为桌面顶栏两套来回闪、换栏瞬间出现中间态。
 *
 * 现在唯一绘制者是 ui_sysbar::draw_status(由 pushCanvasSystemBar 内的 overlay 触发):
 * Launcher 只请求"重画 + 推屏", 保证桌面上永远只有一条栏, 且与 app 内是同一套。 */

void Launcher::start_system_bar()
{
    render_system_bar();
}

void Launcher::update_system_bar()
{
    /* 1Hz 看门狗重推: 正常情况下 ui_sysbar 只在链路/电量/分钟变化时重画,
     * 这里兜底一次, 保证时钟跳分钟、链路 E/L 切换后桌面会跟上。 */
    if ((GetHAL().millis() - _data.system_bar_update_count) > _data.system_bar_update_period) {
        render_system_bar();
        _data.system_bar_update_count = GetHAL().millis();
    }
}

void Launcher::render_system_bar()
{
    /* 换屏/通知结束/画布重建后, 系统栏画布里的内容可能是旧的, 必须让 ui_sysbar 重画。
     * invalidate() 只置脏, 真正的绘制在 pushCanvasSystemBar → overlay 里完成。 */
    ui_sysbar::invalidate();
    GetHAL().pushCanvasSystemBar();
}

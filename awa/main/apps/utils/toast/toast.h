#pragma once

/*
 * 全局提示条（静音 / 有声）
 *
 * 两条硬规矩：
 * 1. notify() 只允许在键盘回调（desktop_task 里）调用：它只置一个原子标志，
 *    绝不碰画布、屏幕或任何 sprite，避免和 app_task 抢同一块缓冲。
 * 2. update() 只允许在 app 主任务（app_task，也就是 GetMooncake().update() 所在任务）
 *    里调用，由它负责绘制、推屏、超时后恢复画面。
 */
namespace ui_toast {

// 请求显示一条"静音/有声"提示（muted=true 显示静音）
void notify(bool muted);

// 由 app_task 每帧调用一次：处理 pending、绘制提示、到点恢复画面
void update();

}  // namespace ui_toast

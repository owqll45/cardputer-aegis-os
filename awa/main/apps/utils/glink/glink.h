/*
 * glink — Cardputer 全局 ESP-NOW 镜像/联机服务(由 gmk_link 抽取通用化)
 * 流程: 主机每 500ms 广播信标 → C3 回 GMKHI1 → 主机回 GMKWELC → 连接建立
 *       之后前台 app 调 glink_send_state() 发全量状态包, C3 本地重绘
 *       心跳 GMKBEAT 1s(顺带重发 GMKAPP 当前应用信息); 连续失败判掉线回广播
 * 通用化: 任何 app 进前台调 glink_acquire(名字, 游戏id, 图标id),
 *         C3 按游戏 id 切渲染器; 桌面/非游戏时 C3 显示 app 图标+名字
 * 注意: 某些 app 会 wifiDeinit(), poll 里探测到后标记死亡,
 *       不自动重建(省 RAM), 等下个 app acquire 时再重建
 */
#pragma once
#include <stdint.h>

// 游戏 id(C3 渲染器切换依据)
#define GLINK_GID_LAUNCHER 0
#define GLINK_GID_GOMOKU   1
#define GLINK_GID_PONG     2
#define GLINK_GID_REVERSI  3
#define GLINK_GID_TANK     4
#define GLINK_GID_UNO      5
#define GLINK_GID_CONNECT4 6
#define GLINK_GID_CHECKERS 7
#define GLINK_GID_TETRIS   8
#define GLINK_GID_SNAKE    9
#define GLINK_GID_BREAKOUT 10
#define GLINK_GID_2048     11
#define GLINK_GID_MINESWEEPER 12
#define GLINK_GID_FLAPPY   13
#define GLINK_GID_INVADERS 14
#define GLINK_GID_RACE     15
#define GLINK_GID_FPS      16
#define GLINK_GID_SHOGI    17
#define GLINK_GID_DOUDIZHU 18

// 图标 id(C3 预置图标表索引, 与 gomoku_c3/main/icons.c 顺序一致)
#define GLINK_ICON_GENERIC 0
#define GLINK_ICON_GOMOKU  1
#define GLINK_ICON_PONG    2
#define GLINK_ICON_REVERSI 3
#define GLINK_ICON_TANK    4
#define GLINK_ICON_UNO     5
#define GLINK_ICON_CONNECT4 6
#define GLINK_ICON_CHECKERS 7
// 以下暂无专属图标, 用 GENERIC 兜底即可 (C3 显示名字+generic 图标)
#define GLINK_ICON_TETRIS   0
#define GLINK_ICON_SNAKE    0
#define GLINK_ICON_BREAKOUT 0
#define GLINK_ICON_2048     0
#define GLINK_ICON_MINESWEEPER 0
#define GLINK_ICON_FLAPPY   0
#define GLINK_ICON_INVADERS 0
#define GLINK_ICON_RACE     0
#define GLINK_ICON_FPS      0
#define GLINK_ICON_SHOGI    0

// 进前台调用: 登记当前 app 并(如需要)初始化 WiFi+ESP-NOW; 已连接则立即发 GMKAPP
void glink_acquire(const char* app_name, uint8_t game_id, uint8_t icon_id);
// 退出时调用: 仅注销登记, 不断开链接(launcher 会接着 acquire)
void glink_release();

void glink_poll();           // 主循环里调: 信标 / 心跳 / 限频补发 / WiFi 存活探测
bool glink_is_linked();

// 系统设置里的"无线广播"开关：关掉后立刻停止信标，并且不再自动拉起 WiFi
void glink_set_enabled(bool on);

// 发送全量状态包(data 含 'G','M',type 头, byte3 由本层填序号, len ≤ 240)
// 50ms 限频: 过频调用会暂存最后一包, 由 glink_poll 补发
void glink_send_state(const uint8_t* data, int len);

// ---- 联机对战: C3(手柄) → 主机 的控制通道 ----
uint16_t glink_buttons();      // 当前按住的键位掩码(bit0上 1下 2左 3右 4A 5B 6X 7Y 8LB 9RB)
uint32_t glink_btn_seq();      // 掩码每变化一次 +1
uint8_t  glink_remote_flags(); // bit0: 手柄已连接
uint32_t glink_taunt_seq();    // 远端每次请求嘲讽 +1

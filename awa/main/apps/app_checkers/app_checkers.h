/*
 * Checkers 西洋跳棋(美式 8x8): 单人(极小化极大AI) / 双人联机(C3+Xbox手柄=白方)
 * 规则: 强制吃子 / 连跳 / 到底升王(跳到底即停) / 王可后退 / 无子或无着判负 / 100半回合无吃子和
 * 镜像: CKSTATE 0x26 38B 全量包, C3 本地重绘
 * 操作: E/W上 S下 A左 D右(+方向键) 移光标, Enter/空格 选子/落子, 退格 取消选择; 联机 Enter=准备
 */
#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>

class AppCheckers : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // 0=模式菜单 1=单人(打AI) 2=双人联机(手柄)
    int _mode = 0;
    int _menu_sel = 0;

    uint8_t _b[64];           // 0空 1红兵 2红王 3白兵 4白王; idx=r*8+c
    int _cur = 1;             // 当前方 1红(键盘) 2白(AI/手柄)
    int _cursor = 51;         // 光标格(红方前沿附近)
    int _sel = -1;            // 选中格(-1无)
    int _locked = -1;         // 连跳锁定格(-1无, 锁定时只能继续吃)
    uint8_t _dests[8];        // 当前选中子的合法落点
    int _ndests = 0;
    bool _over = false;
    int _winner = 0;          // 1红 2白 3和
    int _quiet = 0;           // 距上次吃子的半回合数(和棋判定)
    int _moves = 0;
    int _score[2] = {0, 0};
    int _last_from = -1, _last_to = -1;
    int _hint_from = -1, _hint_to = -1;  // AI建议(C3作弊显示)

    // 联机(双人模式)
    bool _started = false;
    bool _ready_local = false, _ready_remote = false;
    uint16_t _net_btn_prev = 0;
    uint16_t _hold_dir = 0;
    uint32_t _hold_ms = 0, _rep_ms = 0;

    uint32_t _ai_timer = 0;
    uint32_t _last_mirror = 0;
    bool _dirty = true;

    void resetGame();
    void refreshDests();      // 重算 _sel 的合法落点
    void pressConfirm();      // Enter/空格: 选子/落子/取消
    void endTurn();
    void aiMove();
    void updateHint();
    void handleNet(uint32_t now);
    void draw();
    void sendMirrorState();

public:
    AppCheckers();
    ~AppCheckers();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

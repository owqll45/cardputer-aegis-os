/*
 * Connect4 四子棋: 单人(极小化极大AI) / 双人联机(C3+Xbox手柄=黄方)
 * 镜像: CFSTATE 0x25 27B 全量包, C3 本地重绘
 * 操作: A/D 或 左右 选列, Enter/S/下/空格 落子; 联机 Enter=准备
 */
#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>

class AppConnect4 : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // 0=模式菜单 1=单人(打AI) 2=双人联机(手柄)
    int _mode = 0;
    int _menu_sel = 0;

    uint8_t _b[42];           // 0空 1红(P1) 2黄(P2); idx=r*7+c, r=0为顶行
    int _sel = 3;             // 光标列
    int _cur = 1;             // 当前方 1红 2黄
    bool _over = false;
    int _winner = 0;          // 1红 2黄 3平
    int _win_idx = -1;        // 胜利线起点格
    int _win_dir = 0;         // 0横 1竖 2撇(\) 3捺(/)
    int _last_c = -1, _last_r = -1;
    int _moves = 0;
    int _score[2] = {0, 0};   // 累计胜场(退菜单清零)
    int _hint_col = -1;       // AI建议列(C3作弊显示)

    // 联机(双人模式)
    bool _started = false;
    bool _ready_local = false, _ready_remote = false;
    uint16_t _net_btn_prev = 0;
    uint16_t _hold_dir = 0;
    uint32_t _hold_ms = 0, _rep_ms = 0;

    uint32_t _ai_timer = 0;
    uint32_t _last_mirror = 0;
    bool _dirty = true;

    void tryDrop();
    void afterDrop();
    void aiMove();
    void updateHint();
    void resetGame();
    void handleNet(uint32_t now);
    void draw();
    void sendMirrorState();

public:
    AppConnect4();
    ~AppConnect4();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

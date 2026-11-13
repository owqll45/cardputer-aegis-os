/*
 * Reversi 黑白棋: 单人(贪心AI) / 双人联机(C3+Xbox手柄=白方)
 * 镜像: RVSTATE 0x22 29B 全量包, C3 本地重绘
 */
#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>

class AppReversi : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // 0=模式菜单 1=单人(打AI) 2=双人联机(手柄)
    int _mode = 0;
    int _menu_sel = 0;

    uint8_t _b[64];           // 0空 1黑 2白; idx=r*8+c
    int _cx = 3, _cy = 2;     // 光标
    int _cur = 1;             // 当前方 1黑 2白
    bool _over = false;
    int _winner = 0;          // 1黑 2白 3平
    int _last_r = -1, _last_c = -1;
    bool _must_pass = false;  // 上一手后对方无步自动pass(提示用)
    int _pass_player = 0;

    // 联机(双人模式)
    bool _started = false;
    bool _ready_local = false, _ready_remote = false;
    uint16_t _net_btn_prev = 0;
    uint16_t _hold_dir = 0;
    uint32_t _hold_ms = 0, _rep_ms = 0;

    uint32_t _ai_timer = 0;
    uint32_t _last_mirror = 0;
    bool _dirty = true;

    uint64_t legalMask(int p) const;
    int applyMove(int idx, int p); // 落子+翻转, 返回翻转数
    void tryPlace();
    void afterMove();
    void aiMove();
    void resetGame();
    void handleNet(uint32_t now);
    void draw();
    void sendMirrorState();

public:
    AppReversi();
    ~AppReversi();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

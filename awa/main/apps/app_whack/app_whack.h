#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <apps/utils/key_event_queue.h>

// 打地鼠: 3x3 地洞, 30 秒内敲得越多越好
class AppWhack : public mooncake::AppAbility {
public:
    AppWhack();
    ~AppWhack();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    KeyEventQueue _keyq;

    static const int N = 9;
    static const uint32_t GAME_MS = 30000;

    int _mole = -1;             // 当前冒头的地洞
    uint32_t _mole_until = 0;   // 它什么时候缩回去
    int _cursor = 4;            // 键盘光标(方向键+Enter 也能打)
    int _score = 0;
    int _hit = 0;
    int _miss = 0;
    int _combo = 0;
    int _best_combo = 0;
    bool _over = false;
    uint32_t _start_ms = 0;

    void handleKey(const Keyboard::KeyEvent_t& e);
    void newMole(uint32_t now);
    void hitCell(int idx, uint32_t now);
    void draw();
};

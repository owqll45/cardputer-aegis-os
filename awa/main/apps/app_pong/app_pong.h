#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>

class AppPong : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // 0=模式菜单 1=单人(打AI) 2=双人联机(手柄控制右挡板)
    int _mode = 0;
    int _menu_sel = 0;

    float _ballX, _ballY;
    float _ballDX, _ballDY;
    float _paddle1Y, _paddle2Y;
    int _score1, _score2;

    const int _paddleH = 20;
    const int _paddleW = 4;

    bool _key_w = false, _key_s = false;
    bool _key_up = false, _key_down = false;

    // 联机(双人模式): C3 手柄输入 + 胜负
    uint16_t _net_btn_prev = 0;
    bool _over = false;
    int _winner = 0; // 1=左(键盘) 2=右(手柄/AI)
    static const int WIN_SCORE = 7;

    uint32_t _last_step_ms = 0;
    float _ai_target_offset = 0;

    int _diff = 2;        // 1~5 星, 5 星为预测+无误差(无法被打破)
    bool _cheat = false;  // C 键: AI 挡板减半 + 球减速

    int aiPaddleH() const { return _cheat ? (_paddleH / 2) : (_diff >= 5 ? 24 : _paddleH); }
    float predictBallY() const;

    void updateGame();
    void updateUI();
    void resetBall();
    void restart();
    void sendMirrorState();

public:
    AppPong();
    ~AppPong();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

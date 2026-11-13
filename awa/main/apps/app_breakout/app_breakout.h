#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <vector>

class AppBreakout : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    
    struct Rect {
        float x, y, w, h;
        bool active;
    };
    
    // Game State
    float _paddleX;
    const float _paddleW = 40;
    const float _paddleH = 5;
    float _paddleY = 120;
    
    float _ballX, _ballY;
    float _ballDX, _ballDY;
    const float _ballSize = 4;
    
    std::vector<Rect> _bricks;
    int _score;
    int _lives;
    bool _game_over;
    bool _running; // Ball attached to paddle
    
    bool _key_left = false;
    bool _key_right = false;
    
    void updateUI();
    void resetLevel();
    void resetBall();
    void updateGame();
    void sendMirrorState();

public:
    AppBreakout();
    ~AppBreakout();
    
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

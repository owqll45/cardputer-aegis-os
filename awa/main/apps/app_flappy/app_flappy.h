#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <vector>

class AppFlappy : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    
    float _birdY, _birdV;
    const float _gravity = 0.4;
    const float _jump = -4.0;
    
    struct Pipe {
        float x;
        int gapY;
        bool passed;
    };
    std::vector<Pipe> _pipes;
    
    int _score;
    bool _game_over;
    
    void jump();
    void updateGame();
    void updateUI();
    void resetGame();
    void sendMirrorState();

public:
    AppFlappy();
    ~AppFlappy();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

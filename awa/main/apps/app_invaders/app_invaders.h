#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <vector>

class AppInvaders : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    
    struct Alien {
        float x, y;
        bool alive;
    };
    std::vector<Alien> _aliens;
    float _alienDX;
    
    struct Bullet {
        float x, y;
        bool active;
        bool enemy;   // true: fired by aliens
    };
    std::vector<Bullet> _bullets;
    
    float _playerX;
    bool _key_left = false, _key_right = false;
    bool _game_over;
    int _score;
    uint32_t _last_step_ms = 0;
    uint32_t _enemy_fire_timer = 0;
    
    void updateGame();
    void updateUI();
    void resetGame(bool reset_speed = true);
    void shoot();
    void sendMirrorState();

public:
    AppInvaders();
    ~AppInvaders();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

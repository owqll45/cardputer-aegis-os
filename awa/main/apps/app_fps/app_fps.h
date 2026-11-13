#pragma once
#include <mooncake.h>
#include <M5GFX.h>

class AppFPS : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // Player state
    double _posX, _posY;  // x and y start position
    double _dirX, _dirY; // initial direction vector
    double _planeX, _planeY; // the 2d raycaster version of camera plane

    // Game loop
    uint32_t _last_frame_time = 0;
    
    // Input state
    bool _key_up = false;
    bool _key_down = false;
    bool _key_left = false;
    bool _key_right = false;
    bool _key_fire = false;

    // Weapon state
    int _weapon_frame = 0; // 0: idle, >0: firing
    bool _has_weapon = true;

    // Sprite system
    struct Sprite {
        double x, y;
        int texture;
        int health;
        bool alive;
        // AI State
        int state; // 0: Idle, 1: Chase, 2: Attack
        uint32_t last_shot;
    };
    std::vector<Sprite> _sprites;
    double _zBuffer[240]; // Depth buffer for sprite occlusion

    struct Particle {
        float x, y; // Screen coords
        float vx, vy;
        int life;
        uint16_t color;
    };
    std::vector<Particle> _particles;
    
    // Map
    int _current_map[24][24];
    int _level = 1;
    bool _generating_map = false;
    uint32_t _generate_timer = 0;
    
    int _player_health = 100;
    bool _game_over = false;

    // Targeting state
    bool _is_aiming_at_enemy = false;
    
    // Feedback
    bool _show_kill_confirm = false;
    uint32_t _kill_confirm_timer = 0;

    void updateGame();
    void renderFrame();
    void drawWeapon();
    void drawCrosshair();
    void renderSprites(double* ZBuffer);
    void shoot();
    void generateMap();
    void updateEnemies(double frameTime);
    void addParticles(int x, int y, int count, uint16_t color);
    void updateParticles();
    void resetGame();
    void sendMirrorState();

public:
    AppFPS();
    ~AppFPS();
    
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <vector>

class AppRace : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // Game state
    float _playerX = 0;
    float _playerZ = 0; // Position along track
    float _speed = 0;
    float _maxSpeed = 200.0f;
    float _accel = 0;
    
    // Track
    struct Segment {
        float x, y, z;
        float X, Y, W;
        float curve; // removed scale and clip to save memory
    };
    
    // Game State
    // std::vector<Segment> _lines;
    // Use fixed array to avoid heap fragmentation
    static const int _total_lines = 300; 
    Segment _lines[_total_lines];
    int _seg_count;
    int _segL = 200; // Segment length
    int _camH = 1500; // Camera height
    float _roadW = 2000; // Road width
    static const int _total_laps = 4;
    
    // Inputs
    bool _key_up = false;
    bool _key_down = false;
    bool _key_left = false;
    bool _key_right = false;
    bool _key_gear_up = false;
    bool _key_gear_down = false;
    
    int _gear = 1; // 1-5
    bool _game_over = false;
    uint32_t _game_over_timer = 0;
    uint32_t _last_step_ms = 0;
    uint32_t _offroad_ms = 0;
    int _laps = 0;
    bool _finished = false;

    void resetGame();
    void updateGame();
    void renderFrame();
    void drawQuad(int color, int x1, int y1, int w1, int x2, int y2, int w2);
    void project(Segment& p, int camX, int camY, int camZ);
    void shiftGear(int dir);
    void sendMirrorState();

public:
    AppRace();
    ~AppRace();
    
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

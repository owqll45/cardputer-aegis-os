#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <apps/utils/key_event_queue.h>
#include <vector>
#include <deque>

class AppSnake : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    KeyEventQueue _keyq;
    void handleKey(const Keyboard::KeyEvent_t& e);
    
    struct Point {
        int x, y;
        bool operator==(const Point& other) const { return x == other.x && y == other.y; }
    };
    
    // Game State
    std::deque<Point> _snake;
    Point _food;
    int _dir; // 0: Up, 1: Right, 2: Down, 3: Left
    int _next_dir;
    bool _game_over;
    int _score;
    
    // Grid
    int _cols = 24; // 240 / 10
    int _rows = 13; // 135 / 10
    int _cell_size = 10;
    
    uint32_t _last_update;
    uint32_t _speed_ms;

    void updateUI();
    void resetGame();
    void spawnFood();
    void updateGame();
    void sendMirrorState();

public:
    AppSnake();
    ~AppSnake();
    
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

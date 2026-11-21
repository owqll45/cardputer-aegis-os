#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <apps/utils/key_event_queue.h>

// 迷宫: DFS 生成, 走到右下角过关, 关卡越多尺寸越大
class AppMaze : public mooncake::AppAbility {
public:
    AppMaze();
    ~AppMaze();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    static const int MW = 19;
    static const int MH = 8;

    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    KeyEventQueue _keyq;

    uint8_t _walls[MW * MH];
    uint8_t _seen[MW * MH];
    int _px = 0, _py = 0;
    int _level = 1;
    int _steps = 0;
    uint32_t _start_ms = 0;
    bool _won = false;
    uint32_t _win_at = 0;

    void generate();
    void move(int dx, int dy);
    void handleKey(const Keyboard::KeyEvent_t& e);
    void draw();
};

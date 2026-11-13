#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <apps/utils/key_event_queue.h>
#include <vector>

class App2048 : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    KeyEventQueue _keyq;
    void handleKey(const Keyboard::KeyEvent_t& e);

    // Game state
    static const int GRID_SIZE = 4;
    int _grid[GRID_SIZE][GRID_SIZE];
    int _score = 0;
    bool _game_over = false;
    bool _win = false;

    // UI Constants (Dynamic)
    int TILE_SIZE = 28;
    int TILE_MARGIN = 4;
    int BOARD_SIZE = 0;
    int START_X = 0;
    int START_Y = 0;

    void resetGame();
    void addRandomTile();
    void drawBoard();
    void sendMirrorState();
    
    // Logic
    bool moveUp();
    bool moveDown();
    bool moveLeft();
    bool moveRight();
    bool canMove();
    
    void handleInput(int keyCode);

public:
    App2048();
    ~App2048();
    
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

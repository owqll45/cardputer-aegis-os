#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <apps/utils/key_event_queue.h>
#include <vector>

class AppTetris : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    KeyEventQueue _keyq;
    void handleKey(const Keyboard::KeyEvent_t& e);
    
    int _cols = 10;
    int _rows = 20;
    int _cell_size = 6;
    int _grid[20][30] = {0}; // Increased max size for larger screens
    
    struct Piece {
        int x, y;
        int shape[4][4];
        int type;
        int color;
    };
    
    Piece _current_piece;
    bool _game_over;
    int _score;
    uint32_t _last_drop;
    int _speed;
    
    void spawnPiece();
    bool checkCollision(int dx, int dy, int rotation[4][4]);
    void rotatePiece();
    void mergePiece();
    void clearLines();
    void updateGame();
    void updateUI();
    void sendMirrorState();

public:
    AppTetris();
    ~AppTetris();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

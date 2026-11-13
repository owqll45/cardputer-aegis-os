#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <apps/utils/key_event_queue.h>
#include <vector>

class AppMinesweeper : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    KeyEventQueue _keyq;
    void handleKey(const Keyboard::KeyEvent_t& e);
    
    const int _cols = 20;
    const int _rows = 11;
    int _cell_size = 11; // computed from canvas size
    const int _mines_count = 30;
    
    struct Cell {
        bool mine;
        bool revealed;
        bool flagged;
        int neighbors;
    };
    Cell _grid[20][11];
    
    int _cursorX, _cursorY;
    bool _game_over;
    bool _won;
    bool _mines_placed = false;
    
    void resetGame();
    void placeMines(int safeX, int safeY);
    void reveal(int x, int y);
    void toggleFlag();
    void updateUI();
    void sendMirrorState();

public:
    AppMinesweeper();
    ~AppMinesweeper();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

#include "app_tetris.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <esp_random.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>

const int SHAPES[7][4][4] = {
    {{0,0,0,0}, {1,1,1,1}, {0,0,0,0}, {0,0,0,0}}, // I
    {{0,0,0,0}, {0,1,1,0}, {0,1,1,0}, {0,0,0,0}}, // O
    {{0,0,0,0}, {0,1,1,0}, {1,1,0,0}, {0,0,0,0}}, // S
    {{0,0,0,0}, {1,1,0,0}, {0,1,1,0}, {0,0,0,0}}, // Z
    {{0,0,0,0}, {1,1,1,0}, {0,1,0,0}, {0,0,0,0}}, // T
    {{0,0,0,0}, {1,1,1,0}, {1,0,0,0}, {0,0,0,0}}, // L
    {{0,0,0,0}, {1,1,1,0}, {0,0,1,0}, {0,0,0,0}}  // J
};

const int COLORS[8] = {
    TFT_BLACK, TFT_CYAN, TFT_YELLOW, TFT_GREEN, TFT_RED, TFT_MAGENTA, TFT_ORANGE, TFT_BLUE
};

AppTetris::AppTetris() {
    setAppInfo().name = "Tetris";
    setAppInfo().icon = nullptr;
}

AppTetris::~AppTetris() {
    // No delete
    _canvas = nullptr;
}

void AppTetris::onOpen() {
    // 不再 wifiDeinit: 镜像需要 WiFi 常驻, 反复 init/deinit 会碎片化内存
    
    // Guard: avoid key-slot accumulation if onOpen is entered twice
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
    
    // Clear Physical Screen
    GetHAL().display().fillScreen(TFT_BLACK);

    // Use Shared Canvas（全屏 8bpp 优先，失败才降级，统一走 app_canvas.h）
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);
    
    // Dynamic cell size: square cells, fit 10 cols x 20 rows
    _cols = 10;
    _cell_size = _canvas->height() / 20;
    if (_cell_size < 4) _cell_size = 4;
    if (_cols * _cell_size > _canvas->width()) {
        _cell_size = _canvas->width() / _cols;
    }
    if (_cell_size < 4) _cell_size = 4;
    _rows = _canvas->height() / _cell_size;
    
    // Init grid
    for(int x=0; x<20; x++) // Using fixed max size 20 to avoid realloc
        for(int y=0; y<30; y++)
            _grid[x][y] = 0;
            
    _score = 0;
    _game_over = false;
    _speed = 500;
    spawnPiece();
    _last_drop = GetHAL().millis();
    
    _keyq.clear();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        // 回调在 desktop_task：只入队，绝不动画布
        _keyq.push(e);
    });

    glink_acquire("Tetris", GLINK_GID_TETRIS, GLINK_ICON_TETRIS);
}

// 在 app 任务里处理按键（onRunning 消费队列时调用），这里画屏才安全
void AppTetris::handleKey(const Keyboard::KeyEvent_t& e) {
    if (!e.state || e.isModifier) return;

    if (e.keyCode == KEY_ESC || e.keyCode == KEY_BACKSPACE) {
        close();
        return;
    }

    if(_game_over && e.keyCode == KEY_ENTER) {
        // Reset in place (re-entering onOpen() from inside the signal
        // callback would deadlock on the signal mutex)
        for(int x=0; x<20; x++)
            for(int y=0; y<30; y++) _grid[x][y] = 0;
        _score = 0;
        _game_over = false;
        _speed = 500;
        spawnPiece();
        _last_drop = GetHAL().millis();
        updateUI();
        return;
    }

    if(!_game_over) {
        if(e.keyCode == KEY_LEFT || e.keyCode == KEY_A || e.keyCode == KEY_COMMA) {
            if(!checkCollision(-1, 0, _current_piece.shape)) _current_piece.x--;
        }
        if(e.keyCode == KEY_RIGHT || e.keyCode == KEY_D || e.keyCode == KEY_SEMICOLON) {
            if(!checkCollision(1, 0, _current_piece.shape)) _current_piece.x++;
        }
        if(e.keyCode == KEY_DOWN || e.keyCode == KEY_S || e.keyCode == KEY_SLASH) {
            updateGame(); // Fast drop
        }
        if(e.keyCode == KEY_UP || e.keyCode == KEY_W || e.keyCode == KEY_DOT) {
            rotatePiece();
        }
    }
}

void AppTetris::spawnPiece() {
    int type = esp_random() % 7;
    _current_piece.type = type;
    _current_piece.color = type + 1;
    _current_piece.x = _cols/2 - 2;
    _current_piece.y = 0;
    memcpy(_current_piece.shape, SHAPES[type], sizeof(_current_piece.shape));
    
    if(checkCollision(0, 0, _current_piece.shape)) {
        _game_over = true;
    }
}

bool AppTetris::checkCollision(int dx, int dy, int rotation[4][4]) {
    for(int i=0; i<4; i++) {
        for(int j=0; j<4; j++) {
            if(rotation[i][j]) {
                int newX = _current_piece.x + j + dx;
                int newY = _current_piece.y + i + dy;
                if(newX < 0 || newX >= _cols || newY >= _rows) return true;
                if(newY >= 0 && _grid[newX][newY]) return true;
            }
        }
    }
    return false;
}

void AppTetris::rotatePiece() {
    int newShape[4][4];
    for(int i=0; i<4; i++) {
        for(int j=0; j<4; j++) {
            newShape[j][3-i] = _current_piece.shape[i][j];
        }
    }
    if(!checkCollision(0, 0, newShape)) {
        memcpy(_current_piece.shape, newShape, sizeof(newShape));
    }
}

void AppTetris::mergePiece() {
    for(int i=0; i<4; i++) {
        for(int j=0; j<4; j++) {
            if(_current_piece.shape[i][j]) {
                int x = _current_piece.x + j;
                int y = _current_piece.y + i;
                if(x >= 0 && x < _cols && y >= 0 && y < _rows) {
                    _grid[x][y] = _current_piece.color;
                }
            }
        }
    }
    clearLines();
    spawnPiece();
}

void AppTetris::clearLines() {
    int cleared = 0;
    for(int y=_rows-1; y>=0; y--) {
        bool full = true;
        for(int x=0; x<_cols; x++) if(!_grid[x][y]) full = false;
        
        if(full) {
            cleared++;
            for(int yy=y; yy>0; yy--) {
                for(int x=0; x<_cols; x++) _grid[x][yy] = _grid[x][yy-1];
            }
            y++; // Check same line again
        }
    }
    
    if (cleared > 0) {
        static const int kLineScore[5] = {0, 100, 300, 500, 800};
        _score += kLineScore[cleared > 4 ? 4 : cleared];
        for (int i = 0; i < cleared && _speed > 100; i++) _speed -= 10;
    }
}

void AppTetris::updateGame() {
    if(_game_over) return;
    if(!checkCollision(0, 1, _current_piece.shape)) {
        _current_piece.y++;
    } else {
        mergePiece();
    }
}

void AppTetris::onRunning() {
    if(GetHAL().goButton().wasClicked()) { close(); return; }
    glink_poll();

    Keyboard::KeyEvent_t e;
    while (_keyq.pop(e)) handleKey(e);
    if (!_canvas) return;  // handleKey 里可能已经 close()

    if(GetHAL().millis() - _last_drop > _speed) {
        updateGame();
        _last_drop = GetHAL().millis();
        updateUI();
    } else {
        updateUI(); // Render usually faster than update
    }
}

void AppTetris::onClose() {
    glink_release();
    if(_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    _handle_key_event_slot_id = -1;
    _keyq.clear();

    // Restore System Canvas（与 HAL 默认一致：16bpp 240x109）
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

void AppTetris::updateUI() {
    if(!_canvas) return;
    _canvas->fillScreen(TFT_BLACK);
    
    int board_w = _cols * _cell_size;
    int board_h = _rows * _cell_size;
    int offsetX = (_canvas->width() - board_w) / 2;
    int offsetY = (_canvas->height() - board_h) / 2;
    
    // Draw Border
    _canvas->drawRect(offsetX-2, offsetY-2, board_w+4, board_h+4, TFT_WHITE);
    
    // Draw Grid
    for(int x=0; x<_cols; x++) {
        for(int y=0; y<_rows; y++) {
            if(_grid[x][y]) {
                _canvas->fillRect(offsetX + x*_cell_size, offsetY + y*_cell_size, _cell_size-1, _cell_size-1, COLORS[_grid[x][y]]);
            }
        }
    }
    
    // Draw Piece
    if(!_game_over) {
        for(int i=0; i<4; i++) {
            for(int j=0; j<4; j++) {
                if(_current_piece.shape[i][j]) {
                    int x = _current_piece.x + j;
                    int y = _current_piece.y + i;
                    if(y >= 0)
                        _canvas->fillRect(offsetX + x*_cell_size, offsetY + y*_cell_size, _cell_size-1, _cell_size-1, COLORS[_current_piece.color]);
                }
            }
        }
    }
    
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(5, 5);
    _canvas->printf("Score: %d", _score);
    if(_game_over) {
        _canvas->setTextDatum(middle_center);
        _canvas->drawString("GAME OVER", _canvas->width()/2, _canvas->height()/2 - 8);
        _canvas->drawString("ENTER: Restart", _canvas->width()/2, _canvas->height()/2 + 8);
        _canvas->setTextDatum(top_left);
    }
    
    _canvas->pushSprite(0, 0);
    sendMirrorState();
}

void AppTetris::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[111];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x28; p[3] = 0;
    // board 10×20 = 200 cells, 每格 4bit, 共 100 bytes
    for (int y = 0; y < _rows && y < 20; y++) {
        for (int x = 0; x < _cols && x < 10; x++) {
            int idx = y * 10 + x;
            int val = _grid[x][y] & 0xF;
            if (idx & 1) p[4 + idx / 2] |= (val << 4);
            else         p[4 + idx / 2]  = val;
        }
    }
    p[104] = (uint8_t)_current_piece.x;
    p[105] = (uint8_t)_current_piece.y;
    p[106] = (uint8_t)_current_piece.type;
    p[107] = (uint8_t)_current_piece.color;
    p[108] = (uint8_t)(_score & 0xFF);
    p[109] = (uint8_t)((_score >> 8) & 0xFF);
    p[110] = (uint8_t)(_game_over ? 1 : 0);
    glink_send_state(p, sizeof(p));
}

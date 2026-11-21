#include "app_minesweeper.h"
#include <mooncake_log.h>
#include <esp_random.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <string.h>

AppMinesweeper::AppMinesweeper() {
    setAppInfo().name = "Minesweeper";
    setAppInfo().icon = nullptr;
}

AppMinesweeper::~AppMinesweeper() {
    if (_canvas) delete _canvas;
}

void AppMinesweeper::onOpen() {
    // 本应用自己全屏绘制, 先释放共享画布(52KB)降低内存峰值, 退出时 launcher 会重建
    GetHAL().canvas().deleteSprite();
    _canvas = new LGFX_Sprite(&GetHAL().display());
    appCanvasBegin(_canvas, getAppInfo().name);
    
    int cw = (_canvas->width() - 10) / _cols;
    int ch = (_canvas->height() - 14) / _rows;
    _cell_size = (cw < ch) ? cw : ch;
    if (_cell_size < 4) _cell_size = 4;
    
    resetGame();
    
    _keyq.clear();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        // desktop_task：只入队
        _keyq.push(e);
    });

    glink_acquire("Minesweeper", GLINK_GID_MINESWEEPER, GLINK_ICON_MINESWEEPER);

    updateUI();  // 首帧: 不然开进来只有顶栏, 棋盘要等第一次按键才画
}

// app 任务里处理按键（onRunning 消费队列），这里绘制才安全
void AppMinesweeper::handleKey(const Keyboard::KeyEvent_t& e) {
    if(!e.state || e.isModifier) return;

    if(e.keyCode == KEY_ESC || e.keyCode == KEY_BACKSPACE) {
        close();
        return;
    }

    if(_game_over) {
        // Explicit restart so a stray key does not wipe the result screen
        if(e.keyCode == KEY_ENTER || e.keyCode == KEY_SPACE || e.keyCode == KEY_R) {
            resetGame();
            updateUI();
        }
        return;
    }

    if(e.keyCode == KEY_LEFT || e.keyCode == KEY_A || e.keyCode == KEY_COMMA) _cursorX = (_cursorX > 0) ? _cursorX-1 : 0;
    if(e.keyCode == KEY_RIGHT || e.keyCode == KEY_D || e.keyCode == KEY_SEMICOLON) _cursorX = (_cursorX < _cols-1) ? _cursorX+1 : _cols-1;
    if(e.keyCode == KEY_UP || e.keyCode == KEY_W || e.keyCode == KEY_DOT) _cursorY = (_cursorY > 0) ? _cursorY-1 : 0;
    if(e.keyCode == KEY_DOWN || e.keyCode == KEY_S || e.keyCode == KEY_SLASH) _cursorY = (_cursorY < _rows-1) ? _cursorY+1 : _rows-1;

    if(e.keyCode == KEY_ENTER || e.keyCode == KEY_SPACE) reveal(_cursorX, _cursorY);
    if(e.keyCode == KEY_F || e.keyCode == KEY_M) toggleFlag(); // F/M for flag
    updateUI();
}

void AppMinesweeper::resetGame() {
    _cursorX = _cols/2;
    _cursorY = _rows/2;
    _game_over = false;
    _won = false;
    _mines_placed = false;
    
    // Clear grid (mines are placed on the first click)
    for(int x=0; x<_cols; x++) {
        for(int y=0; y<_rows; y++) {
            _grid[x][y] = {false, false, false, 0};
        }
    }
}

void AppMinesweeper::placeMines(int safeX, int safeY) {
    int placed = 0;
    while(placed < _mines_count) {
        int x = esp_random() % _cols;
        int y = esp_random() % _rows;
        // Keep the first click and its 8 neighbors safe
        if(abs(x - safeX) <= 1 && abs(y - safeY) <= 1) continue;
        if(!_grid[x][y].mine) {
            _grid[x][y].mine = true;
            placed++;
        }
    }
    
    // Calculate neighbors
    for(int x=0; x<_cols; x++) {
        for(int y=0; y<_rows; y++) {
            if(_grid[x][y].mine) continue;
            int n = 0;
            for(int dx=-1; dx<=1; dx++) {
                for(int dy=-1; dy<=1; dy++) {
                    int nx = x+dx;
                    int ny = y+dy;
                    if(nx>=0 && nx<_cols && ny>=0 && ny<_rows && _grid[nx][ny].mine) n++;
                }
            }
            _grid[x][y].neighbors = n;
        }
    }
    _mines_placed = true;
}

void AppMinesweeper::reveal(int x, int y) {
    if(_grid[x][y].flagged || _grid[x][y].revealed) return;
    
    if(!_mines_placed) {
        placeMines(x, y);
    }
    
    _grid[x][y].revealed = true;
    
    if(_grid[x][y].mine) {
        _game_over = true;
        // Reveal all mines
        for(int i=0; i<_cols; i++) for(int j=0; j<_rows; j++) if(_grid[i][j].mine) _grid[i][j].revealed = true;
        return;
    }
    
    if(_grid[x][y].neighbors == 0) {
        // Flood fill
        for(int dx=-1; dx<=1; dx++) {
            for(int dy=-1; dy<=1; dy++) {
                int nx = x+dx;
                int ny = y+dy;
                if(nx>=0 && nx<_cols && ny>=0 && ny<_rows && !_grid[nx][ny].revealed) reveal(nx, ny);
            }
        }
    }
    
    // Check win
    bool allRevealed = true;
    for(int i=0; i<_cols; i++) for(int j=0; j<_rows; j++) if(!_grid[i][j].mine && !_grid[i][j].revealed) allRevealed = false;
    if(allRevealed) {
        _game_over = true;
        _won = true;
    }
}

void AppMinesweeper::toggleFlag() {
    if(!_grid[_cursorX][_cursorY].revealed) {
        _grid[_cursorX][_cursorY].flagged = !_grid[_cursorX][_cursorY].flagged;
    }
}

void AppMinesweeper::onRunning() {
    if(GetHAL().goButton().wasClicked()) { close(); return; }
    glink_poll();

    Keyboard::KeyEvent_t e;
    while (_keyq.pop(e)) handleKey(e);
    if (!_canvas) return;  // handleKey 里可能已经 close()

    // 静态游戏: onOpen 末尾和按键时 updateUI 调 sendMirrorState,
    // 但玩家不按键时 C3 只有第一帧 state, 加持续发保持画面稳定
    sendMirrorState();
}

void AppMinesweeper::onClose() {
    glink_release();
    _keyq.clear();
    if(_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    _handle_key_event_slot_id = -1;
    if(_canvas) { _canvas->deleteSprite(); delete _canvas; _canvas = nullptr; }
    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
}

void AppMinesweeper::updateUI() {
    if(!_canvas) return;
    _canvas->fillScreen(TFT_BLACK);
    
    int offsetX = (_canvas->width() - _cols * _cell_size) / 2;
    int offsetY = 12;
    
    int flags = 0;
    for(int x=0; x<_cols; x++)
        for(int y=0; y<_rows; y++)
            if(_grid[x][y].flagged) flags++;
    int remaining = _mines_count - flags;
    if(remaining < 0) remaining = 0;
    
    for(int x=0; x<_cols; x++) {
        for(int y=0; y<_rows; y++) {
            int px = offsetX + x*_cell_size;
            int py = offsetY + y*_cell_size;
            
            if(_grid[x][y].revealed) {
                if(_grid[x][y].mine) {
                    _canvas->fillRect(px, py, _cell_size-1, _cell_size-1, TFT_RED);
                    _canvas->drawChar('*', px+2, py+2);
                } else {
                    _canvas->fillRect(px, py, _cell_size-1, _cell_size-1, 0xCE79); // Light Grey
                    if(_grid[x][y].neighbors > 0) {
                        _canvas->setTextColor(TFT_BLACK);
                        _canvas->setCursor(px+3, py+2);
                        _canvas->print(_grid[x][y].neighbors);
                    }
                }
            } else {
                _canvas->fillRect(px, py, _cell_size-1, _cell_size-1, 0x8410); // Dark Grey
                if(_grid[x][y].flagged) {
                    _canvas->setTextColor(TFT_RED);
                    _canvas->drawChar('F', px+3, py+2);
                }
            }
            
            // Cursor
            if(x == _cursorX && y == _cursorY) {
                _canvas->drawRect(px-1, py-1, _cell_size+1, _cell_size+1, TFT_YELLOW);
            }
        }
    }
    
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(3, 2);
    _canvas->printf("Mines: %d", remaining);
    
    if(_game_over) {
        if(_won) {
            _canvas->setTextColor(TFT_GREEN);
            _canvas->drawString("YOU WON!", 150, 2);
        } else {
            _canvas->setTextColor(TFT_RED);
            _canvas->drawString("GAME OVER", 150, 2);
        }
    } else if(!_mines_placed) {
        _canvas->drawString("First click is safe", 90, 2);
    }
    
    appCanvasPush(_canvas);
    sendMirrorState();
}

// MINESWEEPER: 'G','M',0x2B,seq | 4..223 grid(220B) | 224 cursorX | 225 cursorY | 226 mines | 227 flags
void AppMinesweeper::sendMirrorState() {
    if(!glink_is_linked()) return;
    uint8_t p[228];
    memset(p, 0, sizeof(p));
    p[0]='G'; p[1]='M'; p[2]=0x2B;
    // grid: idx = y*20 + x, 存储 0=hidden, 1-8=revealed neighbor, 9=mine, 10=flagged
    for(int y=0; y<_rows; y++) {
        for(int x=0; x<_cols; x++) {
            uint8_t b;
            if(_grid[x][y].revealed) {
                b = _grid[x][y].mine ? 9 : (uint8_t)_grid[x][y].neighbors;
            } else if(_grid[x][y].flagged) {
                b = 10;
            } else {
                b = 0;
            }
            p[4 + y*20 + x] = b;
        }
    }
    p[224] = (uint8_t)_cursorX;
    p[225] = (uint8_t)_cursorY;
    p[226] = (uint8_t)_mines_count;
    p[227] = (_game_over ? 0x01 : 0) | (_won ? 0x02 : 0);
    glink_send_state(p, sizeof(p));
}

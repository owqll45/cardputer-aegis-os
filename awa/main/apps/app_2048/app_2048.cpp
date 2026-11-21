#include "app_2048.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/icons/default_icons.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>

App2048::App2048() {
    setAppInfo().name = "2048";
    setAppInfo().icon = nullptr;
    // setAppInfo().userData = new AppIcon_t(nullptr, icon_2048_small);
}

App2048::~App2048() {
    // No delete
    _canvas = nullptr;
}

void App2048::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Application opened");
    
    // Ensure physical screen is clean before we start
    GetHAL().display().fillScreen(TFT_BLACK);

    // Use Shared Canvas to avoid OOM and ensure Full Screen（统一走 app_canvas.h）
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);
    
    // Explicitly reset shared canvas state
    _canvas->fillScreen(THEME_COLOR_BG);
    _canvas->setTextSize(1);
    _canvas->setTextDatum(top_left);
    
    // Calculate Dynamic Layout
    // Available Height = Screen Height
    // Board Size needs to be square. Max size = min(W, H) - margin
    int minDim = (_canvas->width() < _canvas->height()) ? _canvas->width() : _canvas->height();
    BOARD_SIZE = minDim - 10; // 5px margin
    TILE_MARGIN = 4;
    TILE_SIZE = (BOARD_SIZE - (GRID_SIZE + 1) * TILE_MARGIN) / GRID_SIZE;
    
    // Re-calculate BOARD_SIZE to match tiles perfectly (avoid partial pixels)
    BOARD_SIZE = GRID_SIZE * TILE_SIZE + (GRID_SIZE + 1) * TILE_MARGIN;
    
    START_X = (_canvas->width() - BOARD_SIZE) / 2;
    START_Y = (_canvas->height() - BOARD_SIZE) / 2;
    
    resetGame();

    // Key handling：回调只入队，处理与绘制都在 onRunning（app 任务）
    _keyq.clear();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& keyEvent) {
        _keyq.push(keyEvent);
    });

    drawBoard();

    // 镜像: C3 显示 2048 游戏画面
    glink_acquire("2048", GLINK_GID_2048, GLINK_ICON_2048);
}

// GMKSTATE 0x27 (len=23B) — 2048 棋盘状态
void App2048::sendMirrorState() {
    if (!glink_is_linked()) return;
    static uint8_t seq = 0;
    uint8_t p[23];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x27; p[3] = seq++;
    // flags: bit0 game_over, bit1 win, bit2 can_move
    p[4] = (uint8_t)((_game_over ? 1 : 0) | (_win ? 2 : 0) | (canMove() ? 0 : 4));
    p[5] = (uint8_t)(_score & 0xFF);
    p[6] = (uint8_t)((_score >> 8) & 0xFF);
    // grid 4x4, 每格 4bit: 0=空, 1-11 对应 2^1..2^11
    int idx = 7;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int v = _grid[r][c];
            int lv = 0;
            if (v > 0) { while (v > 1) { v >>= 1; lv++; } }
            // 每 2 格合一个 byte
            if ((r * 4 + c) & 1) p[idx] = (p[idx] & 0xF0) | (lv & 0x0F);
            else p[idx++] = (uint8_t)((lv & 0x0F) << 4);
        }
    }
    glink_send_state(p, (int)(idx + 1));
}

void App2048::onRunning() {
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }
    glink_poll();

    Keyboard::KeyEvent_t e;
    while (_keyq.pop(e)) handleKey(e);
    if (!_canvas) return;  // handleKey 里可能已经 close()

    // 每帧都发 state 包给 C3 —— 2048 不是实时游戏, drawBoard 只在按键时调,
    // 如果只在 drawBoard 末尾发, C3 打开 2048 后没按键就永远收不到 state 包
    sendMirrorState();
}

void App2048::handleKey(const Keyboard::KeyEvent_t& keyEvent) {
    if (keyEvent.isModifier || !keyEvent.state) return;

    if (keyEvent.keyCode == KEY_ESC || keyEvent.keyCode == KEY_BACKSPACE) {
        close();
        return;
    }

    if (keyEvent.keyCode == KEY_R) {
        resetGame();
        drawBoard();
        return;
    }

    if (!_game_over) {
        handleInput(keyEvent.keyCode);
    }
}

void App2048::onClose() {
    glink_release();
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    }
    _handle_key_event_slot_id = -1;
    _keyq.clear();

    // Restore System Canvas
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

void App2048::resetGame() {
    for(int i=0; i<GRID_SIZE; i++)
        for(int j=0; j<GRID_SIZE; j++)
            _grid[i][j] = 0;
            
    _score = 0;
    _game_over = false;
    _win = false;
    
    addRandomTile();
    addRandomTile();

    // 同步到 C3
    sendMirrorState();
}

void App2048::addRandomTile() {
    std::vector<std::pair<int, int>> empty;
    // Pre-reserve to avoid reallocations
    empty.reserve(GRID_SIZE * GRID_SIZE);
    
    for(int i=0; i<GRID_SIZE; i++) {
        for(int j=0; j<GRID_SIZE; j++) {
            if (_grid[i][j] == 0) empty.push_back({i, j});
        }
    }
    
    if (!empty.empty()) {
        int idx = (int)(esp_random() % empty.size());
        _grid[empty[idx].first][empty[idx].second] = (esp_random() % 10 == 0) ? 4 : 2;
    }
}

void App2048::handleInput(int keyCode) {
    bool moved = false;
    // Standardized Input: E/S/A/D + Arrows
    if (keyCode == KEY_UP || keyCode == KEY_E || keyCode == KEY_W) moved = moveUp();
    else if (keyCode == KEY_DOWN || keyCode == KEY_S) moved = moveDown();
    else if (keyCode == KEY_LEFT || keyCode == KEY_A) moved = moveLeft();
    else if (keyCode == KEY_RIGHT || keyCode == KEY_D) moved = moveRight();
    
    if (moved) {
        addRandomTile();
        if (!canMove()) _game_over = true;
        drawBoard();
        // audio::play_tone(1000, 50); // Disabled to prevent crash
    }
}

// Logic implementations
bool App2048::moveUp() {
    bool moved = false;
    for (int j = 0; j < GRID_SIZE; j++) {
        bool merged[GRID_SIZE] = {false};
        for (int i = 1; i < GRID_SIZE; i++) {
            if (_grid[i][j] != 0) {
                int row = i;
                while (row > 0) {
                    if (_grid[row - 1][j] == 0) {
                        _grid[row - 1][j] = _grid[row][j];
                        _grid[row][j] = 0;
                        row--;
                        moved = true;
                    } else if (_grid[row - 1][j] == _grid[row][j] && !merged[row - 1]) {
                        _grid[row - 1][j] *= 2;
                        _score += _grid[row - 1][j];
                        _grid[row][j] = 0;
                        merged[row - 1] = true;
                        moved = true;
                        if (_grid[row - 1][j] >= 2048) _win = true;
                        break; 
                    } else {
                        break;
                    }
                }
            }
        }
    }
    return moved;
}

bool App2048::moveDown() {
    bool moved = false;
    for (int j = 0; j < GRID_SIZE; j++) {
        bool merged[GRID_SIZE] = {false};
        for (int i = GRID_SIZE - 2; i >= 0; i--) {
            if (_grid[i][j] != 0) {
                int row = i;
                while (row < GRID_SIZE - 1) {
                    if (_grid[row + 1][j] == 0) {
                        _grid[row + 1][j] = _grid[row][j];
                        _grid[row][j] = 0;
                        row++;
                        moved = true;
                    } else if (_grid[row + 1][j] == _grid[row][j] && !merged[row + 1]) {
                        _grid[row + 1][j] *= 2;
                        _score += _grid[row + 1][j];
                        _grid[row][j] = 0;
                        merged[row + 1] = true;
                        moved = true;
                        if (_grid[row + 1][j] >= 2048) _win = true;
                        break;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    return moved;
}

bool App2048::moveLeft() {
    bool moved = false;
    for (int i = 0; i < GRID_SIZE; i++) {
        bool merged[GRID_SIZE] = {false};
        for (int j = 1; j < GRID_SIZE; j++) {
            if (_grid[i][j] != 0) {
                int col = j;
                while (col > 0) {
                    if (_grid[i][col - 1] == 0) {
                        _grid[i][col - 1] = _grid[i][col];
                        _grid[i][col] = 0;
                        col--;
                        moved = true;
                    } else if (_grid[i][col - 1] == _grid[i][col] && !merged[col - 1]) {
                        _grid[i][col - 1] *= 2;
                        _score += _grid[i][col - 1];
                        _grid[i][col] = 0;
                        merged[col - 1] = true;
                        moved = true;
                        if (_grid[i][col - 1] >= 2048) _win = true;
                        break;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    return moved;
}

bool App2048::moveRight() {
    bool moved = false;
    for (int i = 0; i < GRID_SIZE; i++) {
        bool merged[GRID_SIZE] = {false};
        for (int j = GRID_SIZE - 2; j >= 0; j--) {
            if (_grid[i][j] != 0) {
                int col = j;
                while (col < GRID_SIZE - 1) {
                    if (_grid[i][col + 1] == 0) {
                        _grid[i][col + 1] = _grid[i][col];
                        _grid[i][col] = 0;
                        col++;
                        moved = true;
                    } else if (_grid[i][col + 1] == _grid[i][col] && !merged[col + 1]) {
                        _grid[i][col + 1] *= 2;
                        _score += _grid[i][col + 1];
                        _grid[i][col] = 0;
                        merged[col + 1] = true;
                        moved = true;
                        if (_grid[i][col + 1] >= 2048) _win = true;
                        break;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    return moved;
}

bool App2048::canMove() {
    for(int i=0; i<GRID_SIZE; i++)
        for(int j=0; j<GRID_SIZE; j++)
            if (_grid[i][j] == 0) return true;
            
    for(int i=0; i<GRID_SIZE; i++) {
        for(int j=0; j<GRID_SIZE; j++) {
            if (i < GRID_SIZE - 1 && _grid[i][j] == _grid[i+1][j]) return true;
            if (j < GRID_SIZE - 1 && _grid[i][j] == _grid[i][j+1]) return true;
        }
    }
    return false;
}

void App2048::drawBoard() {
    _canvas->fillScreen(THEME_COLOR_BG);
    
    // Draw Background
    _canvas->fillRoundRect(START_X, START_Y, BOARD_SIZE, BOARD_SIZE, 5, 0x8410); // Grey

    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            int val = _grid[i][j];
            int x = START_X + TILE_MARGIN + j * (TILE_SIZE + TILE_MARGIN);
            int y = START_Y + TILE_MARGIN + i * (TILE_SIZE + TILE_MARGIN);
            
            uint16_t color = 0xCDC1; // default empty
            uint16_t textColor = TFT_BLACK;
            
            if (val > 0) {
                switch(val) {
                    case 2: color = 0xEEE4; break;
                    case 4: color = 0xEDE0; break;
                    case 8: color = 0xF2B1; textColor = TFT_WHITE; break;
                    case 16: color = 0xF595; textColor = TFT_WHITE; break;
                    case 32: color = 0xF67C; textColor = TFT_WHITE; break;
                    case 64: color = 0xF65E; textColor = TFT_WHITE; break;
                    case 128: color = 0xEDCF; textColor = TFT_WHITE; break;
                    case 256: color = 0xEDCC; textColor = TFT_WHITE; break;
                    case 512: color = 0xEDC8; textColor = TFT_WHITE; break;
                    case 1024: color = 0xEDC5; textColor = TFT_WHITE; break;
                    case 2048: color = 0xEDC2; textColor = TFT_WHITE; break;
                    default: color = TFT_BLACK; textColor = TFT_WHITE; break;
                }
            }
            
            _canvas->fillRoundRect(x, y, TILE_SIZE, TILE_SIZE, 3, color);
            
            if (val > 0) {
                _canvas->setTextColor(textColor);
                _canvas->setTextSize(1);
                _canvas->setFont(&fonts::efontCN_10);
                
                // Center text
                char str[16];
                sprintf(str, "%d", val);
                int tw = _canvas->textWidth(str);
                int th = _canvas->fontHeight();
                _canvas->drawString(str, x + (TILE_SIZE - tw) / 2, y + (TILE_SIZE - th) / 2);
            }
        }
    }
    
    // UI Info
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setCursor(5, 5);
    _canvas->printf("Score: %d", _score);
    
    if (_game_over) {
        _canvas->setTextColor(TFT_RED);
        _canvas->drawCenterString("GAME OVER", _canvas->width() / 2, 5, &fonts::efontCN_10);
        _canvas->drawCenterString("[R] Restart", _canvas->width() / 2, _canvas->height() - 15, &fonts::efontCN_10);
    } else if (_win) {
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->drawCenterString("YOU WIN", _canvas->width() / 2, 5, &fonts::efontCN_10);
    }
    
    appCanvasPush(_canvas);
}

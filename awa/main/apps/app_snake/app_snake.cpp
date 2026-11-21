#include "app_snake.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <esp_random.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>

AppSnake::AppSnake() {
    setAppInfo().name = "Snake";
    setAppInfo().icon = nullptr;
}

AppSnake::~AppSnake() {
    // No delete
    _canvas = nullptr;
}

void AppSnake::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Application opened");
    
    // Clear Physical Screen
    GetHAL().display().fillScreen(TFT_BLACK);

    // Use Shared Canvas（统一走 app_canvas.h）
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);
    
    // Dynamic grid
    _cols = _canvas->width() / _cell_size;
    _rows = _canvas->height() / _cell_size;
    
    resetGame();

    // Key handling：回调只入队
    _keyq.clear();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& keyEvent) {
        _keyq.push(keyEvent);
    });

    updateUI();
    glink_acquire("Snake", GLINK_GID_SNAKE, GLINK_ICON_SNAKE);
}

void AppSnake::handleKey(const Keyboard::KeyEvent_t& keyEvent) {
    if (keyEvent.isModifier || !keyEvent.state) return;

    if (keyEvent.keyCode == KEY_ESC || keyEvent.keyCode == KEY_BACKSPACE) {
        close();
        return;
    }

    // Restart
    if (_game_over && (keyEvent.keyCode == KEY_ENTER || keyEvent.keyCode == KEY_SPACE)) {
        resetGame();
        updateUI();
        return;
    }

    // Controls: E/S/A/D (this device) + ; . , / + native arrows
    if ((keyEvent.keyCode == KEY_E || keyEvent.keyCode == KEY_UP || keyEvent.keyCode == KEY_DOT) && _dir != 2) _next_dir = 0;
    else if ((keyEvent.keyCode == KEY_RIGHT || keyEvent.keyCode == KEY_D || keyEvent.keyCode == KEY_SEMICOLON) && _dir != 3) _next_dir = 1;
    else if ((keyEvent.keyCode == KEY_DOWN || keyEvent.keyCode == KEY_S || keyEvent.keyCode == KEY_SLASH) && _dir != 0) _next_dir = 2;
    else if ((keyEvent.keyCode == KEY_LEFT || keyEvent.keyCode == KEY_A || keyEvent.keyCode == KEY_COMMA) && _dir != 1) _next_dir = 3;
}

void AppSnake::resetGame() {
    _snake.clear();
    _snake.push_back({5, 5});
    _snake.push_back({4, 5});
    _snake.push_back({3, 5});
    
    _dir = 1;
    _next_dir = 1;
    _score = 0;
    _game_over = false;
    _speed_ms = 200;
    _last_update = GetHAL().millis();
    
    spawnFood();
}

void AppSnake::spawnFood() {
    // Bounded retries: avoid hanging forever when the snake fills the board
    for (int attempt = 0; attempt < 1000; attempt++) {
        _food.x = esp_random() % _cols;
        _food.y = esp_random() % _rows;
        
        bool onSnake = false;
        for (const auto& p : _snake) {
            if (p == _food) {
                onSnake = true;
                break;
            }
        }
        if (!onSnake) return;
    }
}

void AppSnake::updateGame() {
    if (_game_over) return;
    
    _dir = _next_dir;
    Point head = _snake.front();
    
    switch (_dir) {
        case 0: head.y--; break;
        case 1: head.x++; break;
        case 2: head.y++; break;
        case 3: head.x--; break;
    }
    
    // Wrap around the screen edges instead of dying
    head.x = (head.x + _cols) % _cols;
    head.y = (head.y + _rows) % _rows;
    
    // Self Collision (tail will move away this frame unless we eat)
    bool will_eat = (head == _food);
    size_t check_len = _snake.size();
    if (!will_eat && check_len > 0) {
        check_len--;  // last segment vacates
    }
    for (size_t i = 0; i < check_len; i++) {
        if (_snake[i] == head) {
            _game_over = true;
            return;
        }
    }
    
    _snake.push_front(head);
    
    // Eat Food
    if (will_eat) {
        _score += 10;
        if (_speed_ms > 50) _speed_ms -= 5;
        spawnFood();
    } else {
        _snake.pop_back();
    }
}

void AppSnake::onRunning() {
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }
    glink_poll();

    Keyboard::KeyEvent_t e;
    while (_keyq.pop(e)) handleKey(e);
    if (!_canvas) return;  // handleKey 里可能已经 close()

    if (GetHAL().millis() - _last_update > _speed_ms) {
        updateGame();
        _last_update = GetHAL().millis();
        updateUI();
    }
}

void AppSnake::onClose() {
    glink_release();
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    }
    _handle_key_event_slot_id = -1;
    _keyq.clear();

    // Restore System Canvas Size
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

void AppSnake::updateUI() {
    if (!_canvas) return;
    
    _canvas->fillScreen(TFT_BLACK);
    
    // Draw Snake
    for (const auto& p : _snake) {
        _canvas->fillRect(p.x * _cell_size, p.y * _cell_size, _cell_size - 1, _cell_size - 1, TFT_GREEN);
    }
    
    // Draw Food
    _canvas->fillRect(_food.x * _cell_size, _food.y * _cell_size, _cell_size - 1, _cell_size - 1, TFT_RED);
    
    // Draw Score
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(2, 2);
    _canvas->printf("Score: %d", _score);
    
    if (_game_over) {
        _canvas->setTextColor(TFT_RED);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString("GAME OVER", _canvas->width()/2, _canvas->height()/2 - 10);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->drawString("Press Enter", _canvas->width()/2, _canvas->height()/2 + 10);
        _canvas->setTextDatum(top_left);
    }

    appCanvasPush(_canvas);
    sendMirrorState();
}

void AppSnake::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[49];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x29; p[3] = 0;
    p[4] = (uint8_t)_food.x;
    p[5] = (uint8_t)_food.y;
    p[6] = (uint8_t)_dir;
    p[7] = (uint8_t)(_score & 0xFF);
    p[8] = (uint8_t)((_score >> 8) & 0xFF);
    p[9] = (uint8_t)(_game_over ? 1 : 0);
    // grid_mask 24×13=312bit=39B
    for (const auto& s : _snake) {
        if (s.x < 0 || s.x >= 24 || s.y < 0 || s.y >= 13) continue;
        int idx = s.y * 24 + s.x;
        p[10 + idx / 8] |= (1 << (idx % 8));
    }
    glink_send_state(p, sizeof(p));
}

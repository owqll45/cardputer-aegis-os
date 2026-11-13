#include "app_breakout.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <esp_random.h>
#include <apps/utils/glink/glink.h>

AppBreakout::AppBreakout() {
    setAppInfo().name = "Breakout";
    setAppInfo().icon = nullptr;
}

AppBreakout::~AppBreakout() {
    if (_canvas) {
        delete _canvas;
    }
}

void AppBreakout::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Application opened");

    // 本应用自己全屏绘制, 先释放共享画布(52KB)降低内存峰值, 退出时 launcher 会重建
    GetHAL().canvas().deleteSprite();
    _canvas = new LGFX_Sprite(&GetHAL().display());
    _canvas->setColorDepth(8);
    _canvas->setPsram(false);
    if (!_canvas->createSprite(240, 135)) {
        mclog::tagError(getAppInfo().name, "Failed to create sprite, fallback");
        _canvas->createSprite(GetHAL().display().width(), 109);
    }
    
    _lives = 3;
    _score = 0;
    _game_over = false;
    _paddleY = _canvas->height() - 15;
    resetLevel();

    // Key handling
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& keyEvent) {
        if (!keyEvent.isModifier) {
            // Restart / Launch
            // Check for 'Enter' or ' ' (Space)
            // Use keyCode for better reliability
            if (keyEvent.state && (keyEvent.keyCode == KEY_ENTER || keyEvent.keyCode == KEY_SPACE)) {
                if (_game_over) {
                    _lives = 3;
                    _score = 0;
                    _game_over = false;
                    resetLevel();
                } else if (!_running) {
                    _running = true;
                    _ballDX = (esp_random() % 2 == 0 ? 2 : -2);
                    _ballDY = -2;
                }
                return;
            }
            
            // Controls
            if (keyEvent.keyCode == KEY_LEFT || keyEvent.keyCode == KEY_A || keyEvent.keyCode == KEY_COMMA) _key_left = keyEvent.state;
            if (keyEvent.keyCode == KEY_RIGHT || keyEvent.keyCode == KEY_D || keyEvent.keyCode == KEY_SEMICOLON) _key_right = keyEvent.state;
        }
    });

    updateUI();
    glink_acquire("Breakout", GLINK_GID_BREAKOUT, GLINK_ICON_BREAKOUT);
}

void AppBreakout::resetLevel() {
    _bricks.clear();
    int bw = (_canvas->width() - 4) / 8;
    if (bw < 8) bw = 8;
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 8; x++) {
            _bricks.push_back({(float)(x * bw + 2), (float)(y * 10 + 20), (float)(bw - 4), 8, true});
        }
    }
    resetBall();
}

void AppBreakout::resetBall() {
    _running = false;
    _paddleX = _canvas->width() / 2 - _paddleW / 2;
    _ballX = _canvas->width() / 2;
    _ballY = _paddleY - _ballSize;
    _ballDX = 0;
    _ballDY = 0;
}

void AppBreakout::updateGame() {
    if (_game_over) return;
    
    int w = _canvas->width();
    int h = _canvas->height();
    
    // Paddle
    if (_key_left) _paddleX -= 4;
    if (_key_right) _paddleX += 4;
    
    if (_paddleX < 0) _paddleX = 0;
    if (_paddleX > w - _paddleW) _paddleX = w - _paddleW;
    
    if (!_running) {
        _ballX = _paddleX + _paddleW / 2;
        _ballY = _paddleY - _ballSize;
        return;
    }
    
    // Ball Move
    _ballX += _ballDX;
    _ballY += _ballDY;
    
    // Wall Collision
    if (_ballX <= 0) {
        _ballX = 0;
        _ballDX = (_ballDX < 0) ? -_ballDX : _ballDX;
    } else if (_ballX >= w - _ballSize) {
        _ballX = w - _ballSize;
        _ballDX = (_ballDX > 0) ? -_ballDX : _ballDX;
    }
    if (_ballY <= 0) {
        _ballY = 0;
        _ballDY = (_ballDY < 0) ? -_ballDY : _ballDY;
    }
    
    // Paddle Collision
    if (_ballY + _ballSize >= _paddleY && _ballY < _paddleY + _paddleH &&
        _ballX + _ballSize >= _paddleX && _ballX <= _paddleX + _paddleW) {
        _ballY = _paddleY - _ballSize;
        _ballDY = (_ballDY > 0) ? -_ballDY : _ballDY;
        // Add english
        float hitPos = (_ballX - _paddleX) / _paddleW; // 0.0 to 1.0
        _ballDX = (hitPos - 0.5f) * 6.0f;
    }
    
    // Brick Collision: decide the bounce axis from overlap depth
    bool allCleared = true;
    for (auto& b : _bricks) {
        if (!b.active) continue;
        allCleared = false;
        
        if (_ballX + _ballSize > b.x && _ballX < b.x + b.w &&
            _ballY + _ballSize > b.y && _ballY < b.y + b.h) {
            b.active = false;
            _score += 10;
            
            float overlapX_top = _ballX + _ballSize - b.x;
            float overlapX_bot = b.x + b.w - _ballX;
            float overlapY_top = _ballY + _ballSize - b.y;
            float overlapY_bot = b.y + b.h - _ballY;
            float overlapX = (overlapX_top < overlapX_bot) ? overlapX_top : overlapX_bot;
            float overlapY = (overlapY_top < overlapY_bot) ? overlapY_top : overlapY_bot;
            if (overlapX > overlapY) {
                // Hit from top/bottom
                _ballDY = -_ballDY;
                if (_ballDY > 0) _ballY = b.y + b.h;
                else _ballY = b.y - _ballSize;
            } else {
                // Hit from left/right
                _ballDX = -_ballDX;
                if (_ballDX > 0) _ballX = b.x + b.w;
                else _ballX = b.x - _ballSize;
            }
            break; // Only hit one brick per frame usually
        }
    }
    
    if (allCleared) {
        resetLevel();
    }
    
    // Speed cap
    if (_ballDX > 6) _ballDX = 6;
    if (_ballDX < -6) _ballDX = -6;
    if (_ballDY > 6) _ballDY = 6;
    if (_ballDY < -6) _ballDY = -6;
    
    // Death
    if (_ballY > h) {
        _lives--;
        if (_lives <= 0) {
            _game_over = true;
        } else {
            resetBall();
        }
    }
}

void AppBreakout::onRunning() {
    glink_poll();
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }
    
    updateGame();
    updateUI();
}

void AppBreakout::onClose() {
    glink_release();
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    }
    if (_canvas) {
        _canvas->deleteSprite();
        delete _canvas;
        _canvas = nullptr;
    }
}

void AppBreakout::updateUI() {
    if (!_canvas) return;
    
    _canvas->fillScreen(TFT_BLACK);
    
    // Draw Bricks
    for (const auto& b : _bricks) {
        if (b.active) {
            _canvas->fillRect((int)b.x, (int)b.y, (int)b.w, (int)b.h, TFT_YELLOW);
        }
    }
    
    // Draw Paddle
    _canvas->fillRect((int)_paddleX, (int)_paddleY, (int)_paddleW, (int)_paddleH, TFT_CYAN);
    
    // Draw Ball
    _canvas->fillRect((int)_ballX, (int)_ballY, (int)_ballSize, (int)_ballSize, TFT_WHITE);
    
    // HUD
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(2, 2);
    _canvas->printf("Score: %d  Lives: %d", _score, _lives);
    
    if (_game_over) {
        _canvas->setTextDatum(middle_center);
        _canvas->setTextColor(TFT_RED);
        _canvas->drawString("GAME OVER", _canvas->width() / 2, _canvas->height() / 2 - 10);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->drawString("ENTER: Restart", _canvas->width() / 2, _canvas->height() / 2 + 8);
    } else if (!_running) {
        _canvas->setTextDatum(middle_center);
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->drawString("ENTER: Launch", _canvas->width() / 2, _canvas->height() / 2);
    }
    _canvas->setTextDatum(top_left);

    _canvas->pushSprite(0, 0);
    sendMirrorState();
}

void AppBreakout::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[22];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x2A; p[3] = 0;
    p[4] = (uint8_t)(int(_paddleX * 256) & 0xFF);
    p[5] = (uint8_t)((int(_paddleX * 256) >> 8) & 0xFF);
    p[6] = (uint8_t)(int(_ballX * 256) & 0xFF);
    p[7] = (uint8_t)((int(_ballX * 256) >> 8) & 0xFF);
    p[8] = (uint8_t)(int(_ballY * 256) & 0xFF);
    p[9] = (uint8_t)((int(_ballY * 256) >> 8) & 0xFF);
    p[10] = (int8_t)(_ballDX * 4);
    p[11] = (int8_t)(_ballDY * 4);
    p[12] = (uint8_t)(_score & 0xFF);
    p[13] = (uint8_t)((_score >> 8) & 0xFF);
    p[14] = (uint8_t)_lives;
    p[15] = (uint8_t)((_game_over ? 1 : 0) | (_running ? 2 : 0));
    // brick_mask: 5×8=40 bricks, row-major
    for (int i = 0; i < (int)_bricks.size() && i < 48; i++) {
        if (_bricks[i].active) {
            p[16 + i / 8] |= (1 << (i % 8));
        }
    }
    glink_send_state(p, sizeof(p));
}

#include "app_pong.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>

AppPong::AppPong() {
    setAppInfo().name = "Pong";
    setAppInfo().icon = nullptr;
}

AppPong::~AppPong() {
    // No delete
    _canvas = nullptr;
}

void AppPong::onOpen() {
    // 不调用 wifiDeinit(): 联机镜像需要 WiFi(ESP-NOW) 存活

    // Clear Physical Screen
    GetHAL().display().fillScreen(TFT_BLACK);

    // Use Shared Canvas
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    // 镜像服务: 告诉 C3 当前在 Pong(菜单/单人时 C3 显示图标+名字)
    glink_acquire("Pong", GLINK_GID_PONG, GLINK_ICON_PONG);

    _mode = 0;
    _menu_sel = 0;
    _paddle1Y = _canvas->height() / 2 - _paddleH / 2;
    _paddle2Y = _paddle1Y;
    _score1 = 0;
    _score2 = 0;
    _over = false;
    _winner = 0;
    _net_btn_prev = glink_buttons();
    resetBall();
    _last_step_ms = GetHAL().millis();

    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if(!e.isModifier) {
            if(e.keyCode == KEY_W || e.keyCode == KEY_UP || e.keyCode == KEY_DOT) _key_w = e.state;
            if(e.keyCode == KEY_S || e.keyCode == KEY_DOWN || e.keyCode == KEY_SLASH) _key_s = e.state;
            if(e.keyCode == KEY_UP) _key_up = e.state;
            if(e.keyCode == KEY_DOWN) _key_down = e.state;

            if(e.state) {
                // 菜单选择
                if(_mode == 0 && (e.keyCode == KEY_UP || e.keyCode == KEY_DOWN ||
                                  e.keyCode == KEY_W || e.keyCode == KEY_S)) {
                    _menu_sel ^= 1;
                }
                if(_mode == 0 && e.keyCode == KEY_ENTER) {
                    _mode = _menu_sel + 1;
                    restart();
                }
                // 结束后 Enter 重开
                else if(_over && e.keyCode == KEY_ENTER) {
                    restart();
                }
            }
        }
    });
}

void AppPong::resetBall() {
    _ballX = _canvas->width() / 2;
    _ballY = _canvas->height() / 2;
    _ballDX = (esp_random() % 2 ? 2.5f : -2.5f);
    _ballDY = (esp_random() % 2 ? 1.0f : -1.0f);
    _ai_target_offset = (esp_random() % 100 < 20) ? (float)((int)(esp_random() % 25) - 12) : 0.0f;
}

void AppPong::restart() {
    _score1 = 0;
    _score2 = 0;
    _over = false;
    _winner = 0;
    int h = _canvas ? _canvas->height() : 135;
    _paddle1Y = h / 2 - _paddleH / 2;
    _paddle2Y = _paddle1Y;
    resetBall();
}

void AppPong::updateGame() {
    int w = _canvas->width();
    int h = _canvas->height();

    if(_over) return;

    // Paddle 1 (Left) - 键盘
    if(_key_w) _paddle1Y -= 3;
    if(_key_s) _paddle1Y += 3;
    if(_paddle1Y < 0) _paddle1Y = 0;
    if(_paddle1Y > h - _paddleH) _paddle1Y = h - _paddleH;

    // Paddle 2 (Right)
    if(_mode == 2) {
        // 双人联机: C3 手柄方向键(bit0上 bit1下), 稍慢于键盘防手柄占优
        uint16_t btn = glink_buttons();
        if(btn & 0x0001) _paddle2Y -= 2.5f;
        if(btn & 0x0002) _paddle2Y += 2.5f;
    } else {
        // AI with limited speed and aim error
        float targetY = _ballY + _ai_target_offset;
        float diff = targetY - (_paddle2Y + _paddleH/2);
        if(diff < -1.5f) _paddle2Y -= 1.5f;
        if(diff > 1.5f) _paddle2Y += 1.5f;
    }
    if(_paddle2Y < 0) _paddle2Y = 0;
    if(_paddle2Y > h - _paddleH) _paddle2Y = h - _paddleH;

    // Ball
    _ballX += _ballDX;
    _ballY += _ballDY;

    // Top/Bottom collision
    if(_ballY <= 0) {
        _ballY = 0;
        _ballDY = (_ballDY < 0) ? -_ballDY : _ballDY;
    } else if(_ballY >= h - 4) {
        _ballY = h - 4;
        _ballDY = (_ballDY > 0) ? -_ballDY : _ballDY;
    }

    // Paddles collision
    if(_ballX <= 4 + _paddleW && _ballX > 0 && _ballY + 4 >= _paddle1Y && _ballY <= _paddle1Y + _paddleH) {
        _ballX = 5 + _paddleW;
        _ballDX = (_ballDX < 0) ? -_ballDX : _ballDX;
        if(_ballDX < 6) _ballDX += 0.5f; // Speed up (capped)
    }
    if(_ballX + 4 >= w - 4 - _paddleW && _ballX < w && _ballY + 4 >= _paddle2Y && _ballY <= _paddle2Y + _paddleH) {
        _ballX = w - 9 - _paddleW;
        _ballDX = (_ballDX > 0) ? -_ballDX : _ballDX;
    }

    // Score
    if(_ballX < 0) {
        _score2++;
        if(_score2 >= WIN_SCORE) { _over = true; _winner = 2; }
        resetBall();
    }
    if(_ballX > w) {
        _score1++;
        if(_score1 >= WIN_SCORE) { _over = true; _winner = 1; }
        resetBall();
    }
}

void AppPong::onRunning() {
    if(GetHAL().goButton().wasClicked()) { close(); return; }

    glink_poll();

    // 手柄 A: 结束后重开(沿触发)
    uint16_t btn = glink_buttons();
    uint16_t pressed = (uint16_t)(btn & ~_net_btn_prev);
    _net_btn_prev = btn;
    if((pressed & 0x0010) && _over && _mode == 2) restart();

    int steps = game_steps(GetHAL().millis(), _last_step_ms, 16);
    for (int i = 0; i < steps; i++) {
        updateGame();
    }
    updateUI();
}

void AppPong::onClose() {
    if(_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);

    glink_release();

    // Restore System Canvas
    _canvas = &GetHAL().canvas();
    _canvas->deleteSprite();
    _canvas->createSprite(GetHAL().display().width(), 109);

    _canvas = nullptr;
}

void AppPong::updateUI() {
    if(!_canvas) return;
    _canvas->fillScreen(TFT_BLACK);

    int w = _canvas->width();
    int h = _canvas->height();

    // 模式菜单
    if(_mode == 0) {
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setTextColor(TFT_CYAN);
        int tw = _canvas->textWidth("Pong");
        _canvas->setCursor((w - tw) / 2, 18);
        _canvas->print("Pong");

        const char* items[2] = { "单人游戏", "双人联机" };
        for(int i = 0; i < 2; i++) {
            int iw = _canvas->textWidth(items[i]);
            int ix = (w - iw) / 2;
            int iy = 50 + i * 22;
            if(i == _menu_sel) {
                _canvas->fillRect(ix - 10, iy - 2, iw + 20, 16, TFT_DARKGREY);
                _canvas->setTextColor(TFT_YELLOW);
            } else {
                _canvas->setTextColor(TFT_WHITE);
            }
            _canvas->setCursor(ix, iy);
            _canvas->print(items[i]);
        }
        // 手柄在线状态提示
        _canvas->setTextColor((glink_remote_flags() & 0x01) ? TFT_GREEN : TFT_RED);
        const char* hs = (glink_remote_flags() & 0x01) ? "手柄已连接" : "手柄未连接";
        int hw = _canvas->textWidth(hs);
        _canvas->setCursor((w - hw) / 2, h - 20);
        _canvas->print(hs);

        _canvas->pushSprite(0, 0);
        sendMirrorState();
        return;
    }

    // Net
    for(int i=0; i<h; i+=10) _canvas->drawFastVLine(w/2, i, 5, TFT_WHITE);

    // Paddles
    _canvas->fillRect(4, (int)_paddle1Y, _paddleW, _paddleH, TFT_WHITE);
    _canvas->fillRect(w - 4 - _paddleW, (int)_paddle2Y, _paddleW, _paddleH, _mode == 2 ? TFT_CYAN : TFT_WHITE);

    // Ball
    _canvas->fillRect((int)_ballX, (int)_ballY, 4, 4, TFT_WHITE);

    // Score
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(w/2 - 20, 10);
    _canvas->printf("%d", _score1);
    _canvas->setCursor(w/2 + 15, 10);
    _canvas->printf("%d", _score2);

    // 胜负
    if(_over) {
        _canvas->setFont(&fonts::efontCN_12);
        const char* wt;
        if(_mode == 2) wt = (_winner == 1) ? "键盘方获胜!" : "手柄方获胜!";
        else wt = (_winner == 1) ? "你赢了!" : "AI 获胜!";
        int tw = _canvas->textWidth(wt);
        int bx = (w - tw) / 2 - 8;
        _canvas->fillRect(bx, h/2 - 12, tw + 16, 34, TFT_BLACK);
        _canvas->drawRect(bx, h/2 - 12, tw + 16, 34, TFT_YELLOW);
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setCursor(bx + 8, h/2 - 8);
        _canvas->print(wt);
        _canvas->setTextColor(TFT_DARKGREY);
        const char* hint = _mode == 2 ? "Enter/A 重开" : "Enter 重开";
        _canvas->setCursor((w - _canvas->textWidth(hint)) / 2, h/2 + 10);
        _canvas->print(hint);
    }

    _canvas->pushSprite(0, 0);
    sendMirrorState();
}

// PONGSTATE: 'G','M',0x21,seq | 4-5 ballX i16 | 6-7 ballY i16 | 8 p1y | 9 p2y
//            10 score1 | 11 score2 | 12 flags(bit0联机 bit1游戏中 bit2结束) | 13 winner
void AppPong::sendMirrorState() {
    if(!glink_is_linked()) return;
    uint8_t p[20];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x21; // p[3]=seq 链路层填
    int16_t bx = (int16_t)_ballX, by = (int16_t)_ballY;
    p[4] = bx & 0xFF; p[5] = (bx >> 8) & 0xFF;
    p[6] = by & 0xFF; p[7] = (by >> 8) & 0xFF;
    p[8] = (uint8_t)_paddle1Y;
    p[9] = (uint8_t)_paddle2Y;
    p[10] = (uint8_t)_score1;
    p[11] = (uint8_t)_score2;
    p[12] = (_mode == 2 ? 0x01 : 0) | (_mode != 0 ? 0x02 : 0) | (_over ? 0x04 : 0);
    p[13] = (uint8_t)_winner;
    glink_send_state(p, sizeof(p));
}

#include "app_pong.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>
#include <math.h>

/* efontCN 里没有 ★/☆ 字形(会渲染成一根 1px 竖线), 难度星直接画五角星 */
static void draw_stars(LGFX_Sprite* cv, int x, int cy, int diff, int R, uint16_t on, uint16_t off)
{
    for (int s = 0; s < 5; s++) {
        int cx = x + s * (R * 2 + 3);
        int16_t px[10], py[10];
        for (int i = 0; i < 10; i++) {
            float a = -1.5707963f + (float)i * 0.62831853f;
            float rr = (i & 1) ? (float)R * 0.45f : (float)R;
            px[i] = (int16_t)(cx + cosf(a) * rr);
            py[i] = (int16_t)(cy + sinf(a) * rr);
        }
        uint16_t c = (s < diff) ? on : off;
        for (int i = 0; i < 10; i++) {
            int j = (i + 1) % 10;
            if (s < diff) cv->fillTriangle(cx, cy, px[i], py[i], px[j], py[j], c);
            else cv->drawLine(px[i], py[i], px[j], py[j], c);
        }
    }
}

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
                // 左右键调难度(1~5 星)
                if(_mode == 0 && (e.keyCode == KEY_LEFT || e.keyCode == KEY_RIGHT)) {
                    _diff += (e.keyCode == KEY_RIGHT) ? 1 : -1;
                    if(_diff < 1) _diff = 1;
                    if(_diff > 5) _diff = 5;
                }
                // C 键作弊开关(对局中生效): AI 挡板减半 + 球减速
                if(_mode != 0 && e.keyCode == KEY_C) {
                    _cheat = !_cheat;
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
    /* 星级越高误差越小; 5 星零误差。作弊时也清零, 方便验证"能破"。 */
    int err = (6 - _diff) * 6;
    if (!_cheat && _diff < 5) {
        _ai_target_offset = (float)((int)(esp_random() % (err * 2 + 1)) - err);
    } else {
        _ai_target_offset = 0.0f;
    }
}

float AppPong::predictBallY() const
{
    int w = _canvas ? _canvas->width() : 240;
    int h = _canvas ? _canvas->height() : 109;
    if (fabsf(_ballDX) < 0.05f) return _ballY;
    float t = (w - 9 - _paddleW - _ballX) / _ballDX;
    if (t < 0) return _ballY;
    float y = _ballY + _ballDY * t;
    float span = h - 4;
    if (span <= 1) return _ballY;
    float m = fmodf(y, span * 2.0f);
    if (m < 0) m += span * 2.0f;
    if (m > span) m = span * 2.0f - m;
    return m;
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
        /* AI: 星级决定跟随速度/误差/是否预测落点; 5 星完美预测 + 零误差,
         * 挡板更高(24px), 正常球速下打不穿。C 键作弊会把挡板砍半并减速。 */
        float spd = (_diff >= 5) ? 6.0f : (1.2f + _diff * 0.5f);
        if (_cheat) spd *= 0.8f;
        float targetY = (_diff >= 4) ? predictBallY() : _ballY;
        targetY += _ai_target_offset;
        float diff = targetY - (_paddle2Y + aiPaddleH() / 2.0f);
        if (diff < -spd) _paddle2Y -= spd;
        else if (diff > spd) _paddle2Y += spd;
        else _paddle2Y = targetY - aiPaddleH() / 2.0f;
    }
    if(_paddle2Y < 0) _paddle2Y = 0;
    if(_paddle2Y > h - aiPaddleH()) _paddle2Y = h - aiPaddleH();

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
    if(_ballX + 4 >= w - 4 - _paddleW && _ballX < w && _ballY + 4 >= _paddle2Y && _ballY <= _paddle2Y + aiPaddleH()) {
        _ballX = w - 9 - _paddleW;
        _ballDX = (_ballDX > 0) ? -_ballDX : _ballDX;
        if (_cheat) { _ballDX *= 0.8f; _ballDY *= 0.8f; }
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

    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
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
        _canvas->setCursor((w - tw) / 2, 8);
        _canvas->print("Pong");

        const char* items[2] = { "单人游戏", "双人联机" };
        for(int i = 0; i < 2; i++) {
            int iw = _canvas->textWidth(items[i]);
            int ix = (w - iw) / 2;
            int iy = 34 + i * 20;
            if(i == _menu_sel) {
                _canvas->fillRect(ix - 10, iy - 2, iw + 20, 16, TFT_DARKGREY);
                _canvas->setTextColor(TFT_YELLOW);
            } else {
                _canvas->setTextColor(TFT_WHITE);
            }
            _canvas->setCursor(ix, iy);
            _canvas->print(items[i]);
        }

        // 难度星级: ←→ 调整, 5 星 = 无法被打破
        char dbuf[16];
        snprintf(dbuf, sizeof(dbuf), "难度");
        int dw = _canvas->textWidth(dbuf);
        int stars_w = 5 * (5 * 2 + 3) - 3;
        int x0 = (w - (dw + 6 + stars_w)) / 2;
        _canvas->setTextColor(_diff >= 5 ? TFT_RED : TFT_CYAN);
        _canvas->setCursor(x0, 74);
        _canvas->print(dbuf);
        draw_stars(_canvas, x0 + dw + 6 + 5, 80, _diff, 5,
                   _diff >= 5 ? TFT_RED : TFT_CYAN, TFT_DARKGREY);

        // 手柄在线状态提示(顺带告诉作弊键, 对局中按 C 生效)
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor((glink_remote_flags() & 0x01) ? TFT_GREEN : TFT_RED);
        const char* hs = (glink_remote_flags() & 0x01) ? "手柄已连接  C=作弊" : "手柄未连接  C=作弊";
        int hw = _canvas->textWidth(hs);
        _canvas->setCursor((w - hw) / 2, h - 18);
        _canvas->print(hs);

        appCanvasPush(_canvas);
        sendMirrorState();
        return;
    }

    // Net
    for(int i=0; i<h; i+=10) _canvas->drawFastVLine(w/2, i, 5, TFT_WHITE);

    // Paddles
    _canvas->fillRect(4, (int)_paddle1Y, _paddleW, _paddleH, TFT_WHITE);
    _canvas->fillRect(w - 4 - _paddleW, (int)_paddle2Y, _paddleW, aiPaddleH(), _mode == 2 ? TFT_CYAN : TFT_WHITE);

    // 难度 / 作弊提示
    if(_mode == 1) {
        draw_stars(_canvas, 5, h - 6, _diff, 4,
                   _diff >= 5 ? TFT_RED : TFT_DARKGREY, TFT_DARKGREY);
        if(_cheat) {
            _canvas->setTextColor(TFT_MAGENTA);
            _canvas->setCursor(w - 52, h - 10);
            _canvas->print("CHEAT");
        }
    }

    // Ball
    _canvas->fillRect((int)_ballX, (int)_ballY, 4, 4, TFT_WHITE);

    // Score
    _canvas->setFont(&fonts::efontCN_12);
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

    appCanvasPush(_canvas);
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

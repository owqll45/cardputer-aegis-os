#include "app_flappy.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>
#include <string.h>

AppFlappy::AppFlappy() {
    setAppInfo().name = "Flappy";
    setAppInfo().icon = nullptr;
}

AppFlappy::~AppFlappy() {
    if (_canvas) delete _canvas;
}

void AppFlappy::onOpen() {
    // 本应用自己全屏绘制, 先释放共享画布(52KB)降低内存峰值
    GetHAL().canvas().deleteSprite();
    _canvas = new LGFX_Sprite(&GetHAL().display());
    appCanvasBegin(_canvas, getAppInfo().name);
    resetGame();
    
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if(e.state && !e.isModifier) {
            if(_game_over) {
                if(e.keyCode == KEY_ENTER || e.keyCode == KEY_SPACE) {
                    resetGame();
                }
            }
            else jump();
        }
    });

    glink_acquire("Flappy", GLINK_GID_FLAPPY, GLINK_ICON_FLAPPY);
}

void AppFlappy::resetGame() {
    int h = _canvas->height();
    _birdY = h / 2;
    _birdV = 0;
    _score = 0;
    _game_over = false;
    _pipes.clear();
    int max_gap = (h - 60 > 10) ? (h - 60) : 10;
    _pipes.push_back({(float)_canvas->width() + 60, (int)(10 + esp_random() % max_gap), false});
}

void AppFlappy::jump() {
    _birdV = _jump;
}

void AppFlappy::updateGame() {
    if(_game_over) return;
    
    int h = _canvas->height();
    int w = _canvas->width();
    
    _birdV += _gravity;
    _birdY += _birdV;
    
    // Floor/Ceiling
    if(_birdY < 0 || _birdY + 10 > h - 5) _game_over = true;
    
    // Pipes
    for(auto& p : _pipes) {
        p.x -= 2;
        
        // Collision (AABB)
        if(p.x < 30 && p.x + 20 > 20) { // Bird is at x=20, width 10
            if(_birdY < p.gapY || _birdY + 10 > p.gapY + 40) {
                _game_over = true;
            }
        }
        
        // Score
        if(!p.passed && p.x < 20) {
            _score++;
            p.passed = true;
        }
    }
    
    // Remove old pipes
    while(!_pipes.empty() && _pipes[0].x < -30) _pipes.erase(_pipes.begin());
    
    // Add new pipes (guard against empty vector)
    if(_pipes.empty() || _pipes.back().x < w - 100) {
        int max_gap = (h - 60 > 10) ? (h - 60) : 10;
        _pipes.push_back({(float)w, (int)(10 + esp_random() % max_gap), false});
    }
}

void AppFlappy::onRunning() {
    if(GetHAL().goButton().wasClicked()) { close(); return; }
    glink_poll();
    updateGame();
    updateUI();
}

void AppFlappy::onClose() {
    glink_release();
    if(_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    if(_canvas) { _canvas->deleteSprite(); delete _canvas; _canvas = nullptr; }
    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
}

void AppFlappy::updateUI() {
    if(!_canvas) return;
    _canvas->fillScreen(TFT_SKYBLUE);
    
    int w = _canvas->width();
    int h = _canvas->height();
    
    // Pipes
    for(auto& p : _pipes) {
        _canvas->fillRect((int)p.x, 0, 20, p.gapY, TFT_GREEN);
        _canvas->fillRect((int)p.x, p.gapY + 40, 20, h - (p.gapY + 40), TFT_GREEN);
    }
    
    // Bird
    _canvas->fillRect(20, (int)_birdY, 10, 10, TFT_YELLOW);
    
    // Ground
    _canvas->fillRect(0, h - 5, w, 5, TFT_BROWN);
    
    _canvas->setTextDatum(top_left);
    _canvas->setTextColor(TFT_WHITE);
    char score_buf[16];
    snprintf(score_buf, sizeof(score_buf), "%d", _score);
    _canvas->drawCenterString(score_buf, w / 2, 8, &fonts::efontCN_10);
    
    if(_game_over) {
        _canvas->setTextColor(TFT_RED);
        _canvas->drawCenterString("GAME OVER", w / 2, h / 2 - 12, &fonts::efontCN_10);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->drawCenterString("ENTER: Restart", w / 2, h / 2 + 4, &fonts::efontCN_10);
    }
    
    appCanvasPush(_canvas);
    sendMirrorState();
}

// FLAPPY: 'G','M',0x2C,seq | 4-5 birdY Q8.8 | 6 birdV int8 | 7 pipe_count
//   8..23 pipes[4] x {x:2, gapY:1, passed:1} | 24-25 score | 26 flags
void AppFlappy::sendMirrorState() {
    if(!glink_is_linked()) return;
    uint8_t p[27];
    memset(p, 0, sizeof(p));
    p[0]='G'; p[1]='M'; p[2]=0x2C;
    uint16_t by_q = (uint16_t)(_birdY * 256);
    p[4] = by_q & 0xFF; p[5] = (by_q >> 8) & 0xFF;
    int8_t bv = (int8_t)(_birdV * 8);
    p[6] = (uint8_t)bv;
    int pc = (int)_pipes.size();
    if(pc > 4) pc = 4;
    p[7] = (uint8_t)pc;
    for(int i=0; i<pc; i++) {
        // pipe.x: signed int 像素值 (int→uint16_t 隐式保留位模式), C3 侧 (int16_t) 还原
        int px_i = (int)_pipes[i].x;
        uint16_t px = (uint16_t)px_i;
        p[8 + i*4 + 0] = px & 0xFF;
        p[8 + i*4 + 1] = (px >> 8) & 0xFF;
        // gapY: 主机整数像素, C3 侧乘 sy 比例缩放
        p[8 + i*4 + 2] = (uint8_t)_pipes[i].gapY;
        p[8 + i*4 + 3] = _pipes[i].passed ? 1 : 0;
    }
    p[24] = _score & 0xFF; p[25] = (_score >> 8) & 0xFF;
    p[26] = _game_over ? 0x01 : 0;
    glink_send_state(p, sizeof(p));
}

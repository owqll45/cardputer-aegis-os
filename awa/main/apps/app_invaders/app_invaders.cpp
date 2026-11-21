#include "app_invaders.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>
#include <string.h>

AppInvaders::AppInvaders() {
    setAppInfo().name = "Invaders";
    setAppInfo().icon = nullptr;
}

AppInvaders::~AppInvaders() {
    // No delete
    _canvas = nullptr;
}

void AppInvaders::onOpen() {
    // 不再 wifiDeinit: 镜像/联机需要 WiFi 常驻, 反复 init/deinit 会把桌面内存越吃越少
    
    // Clear Physical Screen
    GetHAL().display().fillScreen(TFT_BLACK);

    // Use Shared Canvas
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);
    
    resetGame();
    _last_step_ms = GetHAL().millis();
    _enemy_fire_timer = GetHAL().millis();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if(!e.isModifier) {
            if(e.keyCode == KEY_LEFT || e.keyCode == KEY_A || e.keyCode == KEY_COMMA) _key_left = e.state;
            if(e.keyCode == KEY_RIGHT || e.keyCode == KEY_D || e.keyCode == KEY_SEMICOLON) _key_right = e.state;
            if(e.state && (e.keyCode == KEY_ENTER || e.keyCode == KEY_SPACE)) {
                if(_game_over) resetGame();
                else shoot();
            }
        }
    });

    glink_acquire("Invaders", GLINK_GID_INVADERS, GLINK_ICON_INVADERS);
}

void AppInvaders::resetGame(bool reset_speed) {
    int w = _canvas->width();
    int h = _canvas->height();
    
    _aliens.clear();
    int cols = w / 30; // 25 + 5 spacing
    int rows = 4;
    
    // Center the formation
    int startX = (w - (cols * 25)) / 2;
    if (startX < 5) startX = 5;
    
    for(int y=0; y<rows; y++) {
        for(int x=0; x<cols-2; x++) { // -2 for margin
            _aliens.push_back({(float)(startX + x*25), (float)(20 + y*15), true});
        }
    }
    _bullets.clear();
    _playerX = w / 2;
    if (reset_speed) _alienDX = 0.5f;
    _game_over = false;
    _score = 0;
}

void AppInvaders::shoot() {
    _bullets.push_back({_playerX + 5, (float)(_canvas->height() - 15), true, false});
}

void AppInvaders::updateGame() {
    if(_game_over) return;
    
    int w = _canvas->width();
    int h = _canvas->height();
    
    // Player
    if(_key_left) _playerX -= 2;
    if(_key_right) _playerX += 2;
    if(_playerX < 0) _playerX = 0;
    if(_playerX > w - 10) _playerX = w - 10;
    
    // Aliens
    bool hitEdge = false;
    bool allDead = true;
    float bottomLine = h - 20;
    
    for(auto& a : _aliens) {
        if(!a.alive) continue;
        allDead = false;
        a.x += _alienDX;
        if(a.x <= 0 || a.x >= w - 10) hitEdge = true;
        if(a.y >= bottomLine) _game_over = true;
    }
    
    if(hitEdge) {
        if(!_aliens.empty()) _alienDX = -_alienDX;
        for(auto& a : _aliens) a.y += 5;
    }
    
    if(allDead) {
        resetGame(false); // Next level: keep the accumulated speed
        _alienDX *= 1.2f;
        _enemy_fire_timer = GetHAL().millis();
    }
    
    // Enemy fire: pick a random live alien from the bottom row
    uint32_t now = GetHAL().millis();
    if(now - _enemy_fire_timer > 900) {
        _enemy_fire_timer = now;
        std::vector<Alien*> shooters;
        for(auto& a : _aliens) {
            if(!a.alive) continue;
            bool has_below = false;
            for(auto& o : _aliens) {
                if(o.alive && o.x == a.x && o.y > a.y) { has_below = true; break; }
            }
            if(!has_below) shooters.push_back(&a);
        }
        if(!shooters.empty()) {
            Alien* s = shooters[esp_random() % shooters.size()];
            _bullets.push_back({s->x + 5, s->y + 10, true, true});
        }
    }
    
    // Bullets
    for(auto& b : _bullets) {
        if(!b.active) continue;
        b.y += (b.enemy ? 3.0f : -4.0f);
        if(b.y < 0 || b.y > h) b.active = false;
        
        if(b.enemy) {
            // Enemy bullet vs player
            if(b.x >= _playerX && b.x <= _playerX + 10 && b.y >= h - 10 && b.y <= h - 5) {
                b.active = false;
                _game_over = true;
            }
            continue;
        }
        
        // Player bullet vs aliens
        for(auto& a : _aliens) {
            if(a.alive && b.x >= a.x && b.x <= a.x + 10 && b.y >= a.y && b.y <= a.y + 8) {
                a.alive = false;
                b.active = false;
                _score += 10;
                break;
            }
        }
    }
}

void AppInvaders::onRunning() {
    if(GetHAL().goButton().wasClicked()) { close(); return; }

    glink_poll();

    int steps = game_steps(GetHAL().millis(), _last_step_ms, 16);
    for (int i = 0; i < steps; i++) {
        updateGame();
    }
    updateUI();
}

void AppInvaders::onClose() {
    glink_release();
    if(_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    
    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
    
    _canvas = nullptr;
}

void AppInvaders::updateUI() {
    if(!_canvas) return;
    _canvas->fillScreen(TFT_BLACK);
    
    // Aliens
    for(const auto& a : _aliens) {
        if(a.alive) _canvas->fillRect((int)a.x, (int)a.y, 10, 8, TFT_GREEN);
    }
    
    // Bullets
    for(const auto& b : _bullets) {
        if(b.active) _canvas->fillRect((int)b.x, (int)b.y, 2, 4, b.enemy ? TFT_RED : TFT_WHITE);
    }
    
    // Player
    int py = _canvas->height() - 10;
    _canvas->fillRect((int)_playerX, py, 10, 5, TFT_CYAN);
    _canvas->fillRect((int)_playerX+4, py-3, 2, 3, TFT_CYAN);
    
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(2, 2);
    _canvas->printf("Score: %d", _score);
    
    if(_game_over) {
        _canvas->setTextColor(TFT_RED);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString("GAME OVER", _canvas->width()/2, _canvas->height()/2);
        _canvas->setTextDatum(top_left);
    }
    
    appCanvasPush(_canvas);
    sendMirrorState();
}

// INVADERS 状态包 (32B ≤ 240B):
//  0-3:   'G','M',0x2D,seq
//  4-10:  alive_mask[7] — 行优先 bit i = alive (_aliens[i] in resetGame push_back order)
//  11-12: playerX Q8.8 (int16)
//  13-14: alien_baseX Q8.8 — 第一个(最左上) alive alien 的当前 x
//  15-16: alien_rowY Q8.8  — 第一行 alien 的当前 y (hitEdge 会整体下移)
//  17:    alien_dir int8  — +1 右, -1 左
//  18:    bullet_count uint8 — 玩家子弹数 max 5
//  19-28: bullets[5] × {x:uint8, y:uint8} — 玩家子弹, (0,0) 表示空槽
//  29-30: score uint16
//  31:    flags (bit0=game_over)
void AppInvaders::sendMirrorState() {
    if(!glink_is_linked()) return;
    uint8_t p[32];
    memset(p, 0, sizeof(p));
    p[0]='G'; p[1]='M'; p[2]=0x2D;
    // alive_mask
    int n = (int)_aliens.size();
    if(n > 55) n = 55;
    for(int i=0; i<n; i++) {
        if(_aliens[i].alive) {
            int byte_idx = i >> 3;
            int bit_idx  = i & 7;
            p[4 + byte_idx] |= (1 << bit_idx);
        }
    }
    // playerX Q8.8
    int16_t px88 = (int16_t)(_playerX * 256);
    p[11] = px88 & 0xFF; p[12] = (px88 >> 8) & 0xFF;
    // alien_baseX & alien_rowY: 找第一个 alive alien 作为基准
    float baseX = 0.0f, rowY = 20.0f;
    bool found = false;
    for(const auto& a : _aliens) {
        if(a.alive) { baseX = a.x; rowY = a.y; found = true; break; }
    }
    if(!found) {
        // 没有 alive 的了 — 用原始初始值 (20, 20)
        int w = _canvas ? _canvas->width() : 240;
        int cols = w / 30;
        baseX = (float)((w - cols * 25) / 2);
        rowY = 20.0f;
    }
    int16_t bx88 = (int16_t)(baseX * 256);
    int16_t ry88 = (int16_t)(rowY * 256);
    p[13] = bx88 & 0xFF; p[14] = (bx88 >> 8) & 0xFF;
    p[15] = ry88 & 0xFF; p[16] = (ry88 >> 8) & 0xFF;
    // alien_dir
    p[17] = (_alienDX >= 0) ? 1 : (uint8_t)-1;
    // bullets: 只发玩家子弹 (最多 5 颗)
    int bc = 0;
    for(const auto& b : _bullets) {
        if(bc >= 5) break;
        if(!b.active || b.enemy) continue;
        uint8_t bx = (uint8_t)b.x;
        uint8_t by = (uint8_t)b.y;
        p[19 + bc * 2] = bx;
        p[20 + bc * 2] = by;
        bc++;
    }
    p[18] = (uint8_t)bc;
    // score + flags
    p[29] = _score & 0xFF; p[30] = (_score >> 8) & 0xFF;
    p[31] = _game_over ? 0x01 : 0;
    glink_send_state(p, sizeof(p));
}

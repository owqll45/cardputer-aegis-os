#include "app_tank.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>
#include <cstring>

static const int SPAWN[2][2] = { { 1, 1 }, { 11, 11 } };
static const int MOVE_MS     = 16;   // 移动节拍
static const int BULLET_MS   = 16;   // 子弹推进节拍
static const int FIRE_CD     = 260;  // 开火冷却
static const int RESPAWN_MS  = 1200;

AppTank::AppTank() {
    setAppInfo().name = "Tank";
    setAppInfo().icon = nullptr;
}

AppTank::~AppTank() { _canvas = nullptr; }

// ---------------- 地图 ----------------
// 钢墙布局两端硬编码共享(不传输), 必须与 C3 render_tank 保持一致
bool AppTank::isSteel(int r, int c) {
    if (r <= 0 || r >= MAP_H - 1 || c <= 0 || c >= MAP_W - 1) return true; // 边框
    if (r == 4 && (c == 2 || c == 3 || c == 9 || c == 10)) return true;
    if (r == 8 && (c == 2 || c == 3 || c == 9 || c == 10)) return true;
    if (c == 6 && (r == 2 || r == 3 || r == 9 || r == 10)) return true;
    return false;
}

bool AppTank::solidAtPx(int x, int y) const {
    int c = x / TILE, r = y / TILE;
    if (r < 0 || r >= MAP_H || c < 0 || c >= MAP_W) return true;
    if (isSteel(r, c)) return true;
    return _brick[r][c] != 0;
}

bool AppTank::rectHitsSolid(int x, int y) const {
    // 8x8 坦克, 检查四角+边中点
    int pts[5][2] = { { x, y }, { x + 7, y }, { x, y + 7 }, { x + 7, y + 7 }, { x + 3, y + 3 } };
    for (auto& p : pts)
        if (solidAtPx(p[0], p[1])) return true;
    return false;
}

bool AppTank::tankHitsTank(int who, int x, int y) const {
    int o = 1 - who;
    if (!_t[o].alive) return false;
    return !(x + 7 < _t[o].x || _t[o].x + 7 < x || y + 7 < _t[o].y || _t[o].y + 7 < y);
}

void AppTank::moveTank(int who, int dir) {
    if (!_t[who].alive) return;
    _t[who].dir = dir;
    // 转向时对齐网格, 否则窄道过不去
    if (dir == 0 || dir == 1) _t[who].x = (_t[who].x / TILE) * TILE;
    else _t[who].y = (_t[who].y / TILE) * TILE;

    int nx = _t[who].x, ny = _t[who].y;
    switch (dir) {
    case 0: ny -= 2; break;
    case 1: ny += 2; break;
    case 2: nx -= 2; break;
    case 3: nx += 2; break;
    }
    if (nx < 0 || ny < 0 || nx + 7 >= FIELD || ny + 7 >= FIELD) return;
    if (rectHitsSolid(nx, ny)) return;
    if (tankHitsTank(who, nx, ny)) return;
    _t[who].x = nx;
    _t[who].y = ny;
}

void AppTank::fire(int who) {
    uint32_t now = GetHAL().millis();
    static uint32_t last_fire[2] = { 0, 0 };
    if (now - last_fire[who] < (uint32_t)FIRE_CD) return;
    int cnt = 0;
    for (int i = 0; i < 3; i++) if (_bl[who][i].active) cnt++;
    if (cnt >= 3) return;
    for (int i = 0; i < 3; i++) {
        if (_bl[who][i].active) continue;
        Bullet& b = _bl[who][i];
        b.active = 1;
        b.dir = _t[who].dir;
        b.x = _t[who].x + 3;
        b.y = _t[who].y + 3;
        switch (b.dir) {
        case 0: b.y -= 4; break;
        case 1: b.y += 8; break;
        case 2: b.x -= 4; break;
        case 3: b.x += 8; break;
        }
        last_fire[who] = now;
        audio::play_tone(900, 0.03);
        break;
    }
}

void AppTank::killTank(int who, int by) {
    if (!_t[who].alive) return;
    _t[who].alive = 0;
    _t[who].dead_ms = GetHAL().millis();
    _lives[who]--;
    _kills[by]++;
    audio::play_random_tone();

    // 胜负: 先到 5 杀, 或 3 命耗尽立即判负
    if (_kills[by] >= WIN_KILLS) {
        _over = true;
        _winner = by + 1;
    } else if (_lives[who] <= 0) {
        _over = true;
        _winner = by + 1;
    }
}

void AppTank::respawnCheck(uint32_t now) {
    for (int i = 0; i < 2; i++) {
        if (_t[i].alive || _lives[i] <= 0) continue;
        if (now - _t[i].dead_ms < (uint32_t)RESPAWN_MS) continue;
        _t[i].x = SPAWN[i][0] * TILE;
        _t[i].y = SPAWN[i][1] * TILE;
        _t[i].dir = (i == 0) ? 1 : 0;
        _t[i].alive = 1;
    }
}

void AppTank::stepBullets() {
    if (_over) return;
    for (int who = 0; who < 2; who++) {
        for (int i = 0; i < 3; i++) {
            Bullet& b = _bl[who][i];
            if (!b.active) continue;
            // 分 3 小步推进, 避免穿墙
            for (int s = 0; s < 3 && b.active; s++) {
                switch (b.dir) {
                case 0: b.y -= 1; break;
                case 1: b.y += 1; break;
                case 2: b.x -= 1; break;
                case 3: b.x += 1; break;
                }
                if (b.x < 0 || b.y < 0 || b.x >= FIELD || b.y >= FIELD) { b.active = 0; break; }
                int c = b.x / TILE, r = b.y / TILE;
                if (isSteel(r, c)) { b.active = 0; break; }
                if (_brick[r][c]) { _brick[r][c] = 0; b.active = 0; break; }
                // 命中对方坦克
                int o = 1 - who;
                if (_t[o].alive && b.x >= _t[o].x && b.x < _t[o].x + 8 &&
                    b.y >= _t[o].y && b.y < _t[o].y + 8) {
                    b.active = 0;
                    killTank(o, who);
                    break;
                }
            }
        }
    }
}

void AppTank::genMap() {
    memset(_brick, 0, sizeof(_brick));
    for (int r = 1; r < MAP_H - 1; r++) {
        for (int c = 1; c < MAP_W - 1; c++) {
            if (isSteel(r, c)) continue;
            // 出生点周边留空
            if (r <= 2 && c <= 2) continue;
            if (r >= MAP_H - 3 && c >= MAP_W - 3) continue;
            if ((esp_random() % 100) < 30) _brick[r][c] = 1;
        }
    }
}

void AppTank::restart() {
    genMap();
    memset(_bl, 0, sizeof(_bl));
    for (int i = 0; i < 2; i++) {
        _t[i].x = SPAWN[i][0] * TILE;
        _t[i].y = SPAWN[i][1] * TILE;
        _t[i].dir = (i == 0) ? 1 : 0;
        _t[i].alive = 1;
        _t[i].dead_ms = 0;
        _lives[i] = MAX_LIVES;
        _kills[i] = 0;
    }
    _over = false;
    _winner = 0;
    _ai_dir = 1;
    _ai_ms = 0;
}

void AppTank::aiStep(uint32_t now) {
    if (_over || !_t[1].alive) return;
    if (now - _ai_ms < 100) return;
    _ai_ms = now;

    // 简易 AI: 朝玩家靠拢, 撞墙换向, 随机开火
    int dx = _t[0].x - _t[1].x, dy = _t[0].y - _t[1].y;
    int want;
    if (abs(dx) > abs(dy)) want = (dx > 0) ? 3 : 2;
    else want = (dy > 0) ? 1 : 0;
    if ((esp_random() % 100) < 25) want = (int)(esp_random() % 4);

    int px = _t[1].x, py = _t[1].y;
    int bx = px, by = py;
    switch (want) {
    case 0: by -= 2; break;
    case 1: by += 2; break;
    case 2: bx -= 2; break;
    case 3: bx += 2; break;
    }
    if (bx < 0 || by < 0 || bx + 7 >= FIELD || by + 7 >= FIELD ||
        rectHitsSolid(bx, by) || tankHitsTank(1, bx, by)) {
        want = (int)(esp_random() % 4);
    }
    moveTank(1, want);

    if ((esp_random() % 100) < 40) fire(1);
}

void AppTank::onOpen() {
    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    glink_acquire("Tank", GLINK_GID_TANK, GLINK_ICON_TANK);

    _mode = 0;
    _menu_sel = 0;
    _net_btn_prev = glink_buttons();
    restart();

    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (e.isModifier) return;

        if (_mode == 0) {
            if (!e.state) return;
            if (e.keyCode == KEY_UP || e.keyCode == KEY_DOWN || e.keyCode == KEY_E ||
                e.keyCode == KEY_W || e.keyCode == KEY_S)
                _menu_sel ^= 1;
            else if (e.keyCode == KEY_ENTER) {
                _mode = _menu_sel + 1;
                restart();
            }
            return;
        }

        // 结束: Enter 重开
        if (_over) {
            if (e.state && e.keyCode == KEY_ENTER) restart();
            return;
        }

        // 键盘方(P1) 按住状态: 上=E/W 下=S 左=A 右=D
        bool is_up = (e.keyCode == KEY_E || e.keyCode == KEY_W || e.keyCode == KEY_UP);
        bool is_dn = (e.keyCode == KEY_S || e.keyCode == KEY_DOWN);
        bool is_lf = (e.keyCode == KEY_A || e.keyCode == KEY_LEFT);
        bool is_rt = (e.keyCode == KEY_D || e.keyCode == KEY_RIGHT);
        if (is_up) _k_up = e.state;
        if (is_dn) _k_down = e.state;
        if (is_lf) _k_left = e.state;
        if (is_rt) _k_right = e.state;

        if (e.state && (e.keyCode == KEY_ENTER || e.keyCode == KEY_SPACE)) fire(0);
    });
}

void AppTank::updateGame(uint32_t now) {
    if (_mode == 0 || _over) return;
    if (_mode == 2 && !(glink_remote_flags() & 0x01)) return; // 等手柄

    static uint32_t last_move = 0;
    if (now - last_move >= (uint32_t)MOVE_MS) {
        last_move = now;
        if (_t[0].alive) {
            if (_k_up) moveTank(0, 0);
            else if (_k_down) moveTank(0, 1);
            else if (_k_left) moveTank(0, 2);
            else if (_k_right) moveTank(0, 3);
        }
        if (_mode == 2 && _t[1].alive) {
            uint16_t m = glink_buttons();
            if (m & 0x0001) moveTank(1, 0);
            else if (m & 0x0002) moveTank(1, 1);
            else if (m & 0x0004) moveTank(1, 2);
            else if (m & 0x0008) moveTank(1, 3);

            uint16_t pressed = (uint16_t)(m & ~_net_btn_prev);
            if (pressed & 0x0010) fire(1);
        }
        _net_btn_prev = glink_buttons();
    }

    if (_mode == 1) aiStep(now);

    static uint32_t last_step = 0;
    if (now - last_step >= (uint32_t)BULLET_MS) {
        last_step = now;
        stepBullets();
    }
    respawnCheck(now);
}

void AppTank::onRunning() {
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    glink_poll();
    uint32_t now = GetHAL().millis();

    // 双人模式: 手柄断线 3 秒后重开(连上后计时器复位, 避免下次断线立刻重开)
    static uint32_t lost_ms = 0;
    if (_mode == 2 && !(glink_remote_flags() & 0x01) && !_over) {
        if (!lost_ms) lost_ms = now;
        else if (now - lost_ms > 3000) { lost_ms = 0; restart(); }
    } else {
        lost_ms = 0;
    }

    updateGame(now);
    draw();

    static uint32_t last_mirror = 0;
    if (now - last_mirror >= 50) {  // 20Hz 镜像, 保证 C3 动画流畅
        last_mirror = now;
        sendMirrorState();
    }
}

void AppTank::onClose() {
    if (_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    glink_release();

    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

// ---------------- 绘制 ----------------
void AppTank::draw() {
    if (!_canvas) return;
    int w = _canvas->width(), h = _canvas->height();
    _canvas->fillScreen(TFT_BLACK);
    _canvas->setFont(&fonts::efontCN_12);

    if (_mode == 0) {
        _canvas->setTextColor(TFT_YELLOW);
        int tw = _canvas->textWidth("Tank 坦克大战");
        _canvas->setCursor((w - tw) / 2, 16);
        _canvas->print("Tank 坦克大战");
        const char* items[2] = { "单人 vs AI", "双人联机" };
        for (int i = 0; i < 2; i++) {
            int iw = _canvas->textWidth(items[i]);
            int ix = (w - iw) / 2, iy = 48 + i * 22;
            if (i == _menu_sel) {
                _canvas->fillRect(ix - 10, iy - 2, iw + 20, 16, TFT_DARKGREY);
                _canvas->setTextColor(TFT_YELLOW);
            } else {
                _canvas->setTextColor(TFT_WHITE);
            }
            _canvas->setCursor(ix, iy);
            _canvas->print(items[i]);
        }
        _canvas->setTextColor((glink_remote_flags() & 0x01) ? TFT_GREEN : TFT_RED);
        const char* hs = (glink_remote_flags() & 0x01) ? "手柄已连接" : "手柄未连接";
        _canvas->setCursor((w - _canvas->textWidth(hs)) / 2, h - 20);
        _canvas->print(hs);
        appCanvasPush(_canvas);
        return;
    }

    const int FX = 4, FY = 18;
    // 场区底色
    _canvas->fillRect(FX - 1, FY - 1, FIELD + 2, FIELD + 2, TFT_DARKGREY);

    // 墙
    for (int r = 0; r < MAP_H; r++) {
        for (int c = 0; c < MAP_W; c++) {
            int px = FX + c * TILE, py = FY + r * TILE;
            if (isSteel(r, c)) {
                _canvas->fillRect(px, py, TILE, TILE, TFT_LIGHTGREY);
                _canvas->drawRect(px, py, TILE, TILE, TFT_DARKGREY);
            } else if (_brick[r][c]) {
                _canvas->fillRect(px, py, TILE, TILE, TFT_ORANGE);
                _canvas->drawFastHLine(px, py + 3, TILE, TFT_MAROON);
                _canvas->drawFastHLine(px, py + 6, TILE, TFT_MAROON);
            }
        }
    }

    // 子弹
    for (int who = 0; who < 2; who++)
        for (int i = 0; i < 3; i++)
            if (_bl[who][i].active)
                _canvas->fillRect(FX + _bl[who][i].x, FY + _bl[who][i].y, 2, 2, TFT_YELLOW);

    // 坦克
    for (int who = 0; who < 2; who++) {
        if (!_t[who].alive) continue;
        int px = FX + _t[who].x, py = FY + _t[who].y;
        uint16_t body = (who == 0) ? TFT_CYAN : TFT_MAGENTA;
        _canvas->fillRect(px, py, 8, 8, body);
        _canvas->drawRect(px, py, 8, 8, TFT_BLACK);
        // 炮管
        int cx = px + 3, cy = py + 3;
        switch (_t[who].dir) {
        case 0: _canvas->drawFastVLine(cx + 1, py - 2, 4, TFT_WHITE); break;
        case 1: _canvas->drawFastVLine(cx + 1, py + 7, 4, TFT_WHITE); break;
        case 2: _canvas->drawFastHLine(px - 2, cy + 1, 4, TFT_WHITE); break;
        case 3: _canvas->drawFastHLine(px + 7, cy + 1, 4, TFT_WHITE); break;
        }
    }

    // ---------- 右侧信息 ----------
    int px = FX + FIELD + 8;
    _canvas->setTextColor(TFT_YELLOW);
    _canvas->setCursor(px, 2);
    _canvas->print("坦克大战");

    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(px, 20);
    _canvas->printf("P1 命%d", _lives[0]);
    _canvas->setCursor(px, 34);
    _canvas->printf("   杀%d", _kills[0]);

    _canvas->setTextColor(TFT_MAGENTA);
    _canvas->setCursor(px, 52);
    _canvas->print(_mode == 2 ? "P2 手柄" : "P2 AI");
    _canvas->setCursor(px, 66);
    _canvas->printf("   命%d", _lives[1]);
    _canvas->setCursor(px, 80);
    _canvas->printf("   杀%d", _kills[1]);

    if (_mode == 2 && !(glink_remote_flags() & 0x01)) {
        _canvas->setTextColor(TFT_RED);
        _canvas->setCursor(px, 100);
        _canvas->print("等手柄..");
    } else if (_over) {
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setCursor(px, 100);
        _canvas->print(_winner == 1 ? "P1 胜!" : "P2 胜!");
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(px, 108);
        _canvas->print("Enter重开");
    } else {
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(px, 100);
        _canvas->print("先5杀胜");
        _canvas->setCursor(px, 108);
        _canvas->print("ESAD+Enter");
    }

    appCanvasPush(_canvas);
}

// TANKSTATE 0x23 (59B)
// 0-2 'G','M',0x23 | 3 seq | 4-7 T1 x,y,dir,alive | 8-11 T2
// 12 弹数 | 13-30 6弹x3B(x,y,dir; x=255空) | 31-52 砖墙bitmask 22B
// 53 lives1 | 54 lives2 | 55 kills1 | 56 kills2 | 57 flags | 58 winner
void AppTank::sendMirrorState() {
    if (!glink_is_linked()) return;
    static uint8_t seq = 0;
    uint8_t p[59];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x23; p[3] = seq++;

    p[4] = (uint8_t)_t[0].x; p[5] = (uint8_t)_t[0].y;
    p[6] = (uint8_t)_t[0].dir; p[7] = (uint8_t)_t[0].alive;
    p[8] = (uint8_t)_t[1].x; p[9] = (uint8_t)_t[1].y;
    p[10] = (uint8_t)_t[1].dir; p[11] = (uint8_t)_t[1].alive;

    uint8_t n = 0;
    for (int who = 0; who < 2; who++) {
        for (int i = 0; i < 3; i++) {
            if (!_bl[who][i].active) continue;
            p[13 + n * 3] = (uint8_t)_bl[who][i].x;
            p[14 + n * 3] = (uint8_t)_bl[who][i].y;
            p[15 + n * 3] = (uint8_t)_bl[who][i].dir;
            n++;
        }
    }
    p[12] = n;
    for (int i = n; i < 6; i++) p[13 + i * 3] = 255; // 空槽

    for (int r = 0; r < MAP_H; r++)
        for (int c = 0; c < MAP_W; c++)
            if (_brick[r][c]) {
                int idx = r * MAP_W + c;
                p[31 + idx / 8] |= (uint8_t)(1 << (idx % 8));
            }

    p[53] = (uint8_t)_lives[0];
    p[54] = (uint8_t)_lives[1];
    p[55] = (uint8_t)_kills[0];
    p[56] = (uint8_t)_kills[1];
    p[57] = (_mode == 2 ? 0x01 : 0) | (_mode != 0 ? 0x02 : 0) | (_over ? 0x04 : 0) | (_mode == 1 ? 0x08 : 0);
    p[58] = (uint8_t)_winner;
    glink_send_state(p, sizeof(p));
}

#include "app_reversi.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>

AppReversi::AppReversi() {
    setAppInfo().name = "Reversi";
    setAppInfo().icon = nullptr;
}

AppReversi::~AppReversi() { _canvas = nullptr; }

// ================= 规则核(纯函数, C3 端用同一套重算合法点) =================
static const int8_t kDirs[8][2] = {
    {-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}
};

uint64_t AppReversi::legalMask(int p) const {
    uint64_t m = 0;
    int opp = (p == 1) ? 2 : 1;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (_b[r * 8 + c]) continue;
            for (auto& d : kDirs) {
                int i = r + d[0], j = c + d[1], n = 0;
                while (i >= 0 && i < 8 && j >= 0 && j < 8 && _b[i * 8 + j] == opp) {
                    i += d[0]; j += d[1]; n++;
                }
                if (n > 0 && i >= 0 && i < 8 && j >= 0 && j < 8 && _b[i * 8 + j] == p) {
                    m |= 1ULL << (r * 8 + c);
                    break;
                }
            }
        }
    }
    return m;
}

int AppReversi::applyMove(int idx, int p) {
    int r = idx / 8, c = idx % 8;
    int opp = (p == 1) ? 2 : 1;
    int flips = 0;
    _b[idx] = p;
    for (auto& d : kDirs) {
        int i = r + d[0], j = c + d[1], n = 0;
        while (i >= 0 && i < 8 && j >= 0 && j < 8 && _b[i * 8 + j] == opp) {
            i += d[0]; j += d[1]; n++;
        }
        if (n > 0 && i >= 0 && i < 8 && j >= 0 && j < 8 && _b[i * 8 + j] == p) {
            for (int k = 0; k < n; k++) {
                i -= d[0]; j -= d[1];
                _b[i * 8 + j] = p;
                flips++;
            }
        }
    }
    return flips;
}

void AppReversi::resetGame() {
    memset(_b, 0, sizeof(_b));
    _b[3 * 8 + 3] = 2; _b[3 * 8 + 4] = 1;
    _b[4 * 8 + 3] = 1; _b[4 * 8 + 4] = 2;
    _cur = 1;
    _over = false;
    _winner = 0;
    _last_r = _last_c = -1;
    _must_pass = false;
    _pass_player = 0;
    _cx = 2; _cy = 3;
    _dirty = true;
}

// 落子后: 换方, 处理 pass/终局
void AppReversi::afterMove() {
    _must_pass = false;
    int next = (_cur == 1) ? 2 : 1;
    if (legalMask(next) == 0) {
        if (legalMask(_cur) == 0) {
            // 双方无步: 终局数子
            int nb = 0, nw = 0;
            for (int i = 0; i < 64; i++) {
                if (_b[i] == 1) nb++;
                else if (_b[i] == 2) nw++;
            }
            _over = true;
            _winner = (nb > nw) ? 1 : (nw > nb) ? 2 : 3;
            if (_mode == 2) { _started = false; _ready_local = _ready_remote = false; }
        } else {
            // 对方无步: 自动 pass, 回合回到自己
            _must_pass = true;
            _pass_player = next;
        }
    } else {
        _cur = next;
    }
    if (_mode == 1 && _cur == 2 && !_over) _ai_timer = GetHAL().millis() + 450;
    _dirty = true;
}

void AppReversi::tryPlace() {
    if (_over || !(_mode == 1 || (_mode == 2 && _started))) return;
    if (_mode == 2 && _cur != 1) return; // 键盘方=黑, 只在黑方回合有效
    if (_mode == 1 && _cur != 1) return;
    int idx = _cy * 8 + _cx;
    if (!((legalMask(_cur) >> idx) & 1)) return;
    applyMove(idx, _cur);
    _last_r = _cy; _last_c = _cx;
    audio::play_random_tone();
    afterMove();
}

void AppReversi::aiMove() {
    uint64_t m = legalMask(2);
    if (!m) { afterMove(); return; }
    // 贪心: 翻转最多, 并列随机
    int best = -1, bestf = -1, nbest = 0;
    for (int i = 0; i < 64; i++) {
        if (!((m >> i) & 1)) continue;
        uint8_t save[64];
        memcpy(save, _b, 64);
        int f = applyMove(i, 2);
        memcpy(_b, save, 64);
        if (f > bestf) { bestf = f; best = i; nbest = 1; }
        else if (f == bestf && (esp_random() % ++nbest) == 0) best = i;
    }
    if (best < 0) return;
    applyMove(best, 2);
    _last_r = best / 8; _last_c = best % 8;
    afterMove();
}

// ================= 联机(手柄方=白) =================
void AppReversi::handleNet(uint32_t now) {
    if (_mode != 2) return;
    bool conn = (glink_remote_flags() & 0x01) != 0;
    if (!conn) {
        if (_started || _ready_local || _ready_remote) {
            _started = false;
            _ready_local = _ready_remote = false;
            _dirty = true;
        }
        return;
    }

    uint16_t m = glink_buttons();
    uint16_t pressed = (uint16_t)(m & ~_net_btn_prev);
    _net_btn_prev = m;

    if (!_started || _over) {
        if (pressed & 0x0010) {  // A = 准备/取消准备
            _ready_remote = !_ready_remote;
            _dirty = true;
        }
    } else if (_cur == 2 && !_over) {
        // 方向: 按下走一格, 长按连走
        uint16_t dirs = (uint16_t)(m & 0x000F);
        int dx = 0, dy = 0;
        if (pressed & 0x0001) dy = -1;
        else if (pressed & 0x0002) dy = 1;
        else if (pressed & 0x0004) dx = -1;
        else if (pressed & 0x0008) dx = 1;
        if (dx || dy) {
            _hold_dir = dirs; _hold_ms = now; _rep_ms = now;
        } else if (dirs && dirs == _hold_dir && now - _hold_ms > 420 && now - _rep_ms > 110) {
            _rep_ms = now;
            if (dirs & 0x0001) dy = -1;
            else if (dirs & 0x0002) dy = 1;
            else if (dirs & 0x0004) dx = -1;
            else dx = 1;
        }
        if (!dirs) _hold_dir = 0;
        if (dx || dy) {
            int nx = _cx + dx, ny = _cy + dy;
            if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8) {
                _cx = nx; _cy = ny;
                _dirty = true;
            }
        }
        if (pressed & 0x0010) {  // A = 落子
            int idx = _cy * 8 + _cx;
            if ((legalMask(2) >> idx) & 1) {
                applyMove(idx, 2);
                _last_r = _cy; _last_c = _cx;
                afterMove();
            }
        }
    }

    // 双方都准备 → 开局
    if ((!_started || _over) && _ready_local && _ready_remote) {
        resetGame();
        _started = true;
        _dirty = true;
    }
}

void AppReversi::onOpen() {
    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    glink_acquire("Reversi", GLINK_GID_REVERSI, GLINK_ICON_REVERSI);

    _mode = 0;
    _menu_sel = 0;
    _started = false;
    _ready_local = _ready_remote = false;
    _net_btn_prev = glink_buttons();
    resetGame();

    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (e.isModifier || !e.state) return;

        // 菜单
        if (_mode == 0) {
            if (e.keyCode == KEY_UP || e.keyCode == KEY_DOWN || e.keyCode == KEY_E ||
                e.keyCode == KEY_W || e.keyCode == KEY_S)
                _menu_sel ^= 1;
            else if (e.keyCode == KEY_ENTER) {
                _mode = _menu_sel + 1;
                resetGame();
                if (_mode == 2) { _started = false; _ready_local = _ready_remote = false; }
            }
            _dirty = true;
            return;
        }

        // 联机未开局: Enter = 准备
        if (_mode == 2 && (!_started || _over)) {
            if (e.keyCode == KEY_ENTER) {
                _ready_local = !_ready_local;
                _dirty = true;
            }
            return;
        }

        if (_over) {
            if (e.keyCode == KEY_ENTER && _mode == 1) { resetGame(); }
            return;
        }

        // 棋盘操作(键盘方=黑)
        if (_cur != 1) return;
        int dx = 0, dy = 0;
        if (e.keyCode == KEY_UP || e.keyCode == KEY_E || e.keyCode == KEY_W) dy = -1;
        else if (e.keyCode == KEY_DOWN || e.keyCode == KEY_S) dy = 1;
        else if (e.keyCode == KEY_LEFT || e.keyCode == KEY_A) dx = -1;
        else if (e.keyCode == KEY_RIGHT || e.keyCode == KEY_D) dx = 1;
        if (dx || dy) {
            int nx = _cx + dx, ny = _cy + dy;
            if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8) { _cx = nx; _cy = ny; _dirty = true; }
        } else if (e.keyCode == KEY_ENTER) {
            tryPlace();
        }
    });
}

void AppReversi::onRunning() {
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    glink_poll();
    uint32_t now = GetHAL().millis();
    handleNet(now);

    // AI 回合
    if (_mode == 1 && !_over && _cur == 2 && _ai_timer && now >= _ai_timer) {
        _ai_timer = 0;
        aiMove();
    }

    // 单人模式结束后手柄 A 也能重开(联机时 A 走准备)
    if (_mode == 1 && _over) {
        uint16_t m = glink_buttons();
        uint16_t pressed = (uint16_t)(m & ~_net_btn_prev);
        _net_btn_prev = m;
        if (pressed & 0x0010) resetGame();
    }

    if (_dirty) {
        _dirty = false;
        draw();
        sendMirrorState();
        _last_mirror = now;
    } else if (now - _last_mirror > 500) {
        // 定期补发, 丢包/晚连自愈
        _last_mirror = now;
        sendMirrorState();
    }
}

void AppReversi::onClose() {
    if (_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    glink_release();

    _canvas = &GetHAL().canvas();
    _canvas->deleteSprite();
    _canvas->createSprite(GetHAL().display().width(), 109);
    _canvas = nullptr;
}

// ================= 绘制 =================
void AppReversi::draw() {
    if (!_canvas) return;
    int w = _canvas->width(), h = _canvas->height();
    _canvas->fillScreen(TFT_BLACK);
    _canvas->setFont(&fonts::efontCN_12);

    if (_mode == 0) {
        _canvas->setTextColor(TFT_CYAN);
        int tw = _canvas->textWidth("Reversi 黑白棋");
        _canvas->setCursor((w - tw) / 2, 18);
        _canvas->print("Reversi 黑白棋");
        const char* items[2] = { "单人游戏", "双人联机" };
        for (int i = 0; i < 2; i++) {
            int iw = _canvas->textWidth(items[i]);
            int ix = (w - iw) / 2, iy = 50 + i * 22;
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
        _canvas->pushSprite(0, 0);
        return;
    }

    // 棋盘: 8x13=104, 左侧
    const int CELL = 13, BS = 104;
    const int BX = 4, BY = (h - BS) / 2;
    _canvas->fillRect(BX - 1, BY - 1, BS + 2, BS + 2, 0x0540); // 绿底
    for (int i = 0; i <= 8; i++) {
        _canvas->drawFastHLine(BX, BY + i * CELL - (i == 8 ? 1 : 0), BS, TFT_BLACK);
        _canvas->drawFastVLine(BX + i * CELL - (i == 8 ? 1 : 0), BY, BS, TFT_BLACK);
    }

    uint64_t legal = (!_over && (_mode == 1 || (_mode == 2 && _started))) ? legalMask(_cur) : 0;

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int px = BX + c * CELL + CELL / 2, py = BY + r * CELL + CELL / 2;
            uint8_t v = _b[r * 8 + c];
            if (v == 1) {
                _canvas->fillCircle(px, py, 5, TFT_BLACK);
                _canvas->drawCircle(px, py, 5, TFT_DARKGREY);
            } else if (v == 2) {
                _canvas->fillCircle(px, py, 5, TFT_WHITE);
            } else if ((legal >> (r * 8 + c)) & 1) {
                _canvas->fillCircle(px, py, 1, TFT_YELLOW); // 合法点提示
            }
        }
    }

    // 最后一步
    if (_last_r >= 0)
        _canvas->drawRect(BX + _last_c * CELL, BY + _last_r * CELL, CELL, CELL, TFT_CYAN);
    // 光标(己方回合才画)
    if (!_over && (_cur == 1 || _mode == 2))
        _canvas->drawRect(BX + _cx * CELL + 1, BY + _cy * CELL + 1, CELL - 2, CELL - 2, TFT_RED);

    // ---------- 右侧信息 ----------
    int nb = 0, nw = 0;
    for (int i = 0; i < 64; i++) {
        if (_b[i] == 1) nb++;
        else if (_b[i] == 2) nw++;
    }
    int px = BX + BS + 8;
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(px, 6);
    _canvas->print("黑白棋");
    _canvas->setTextColor(_mode == 2 ? TFT_MAGENTA : TFT_DARKGREY);
    _canvas->setCursor(px, 22);
    _canvas->print(_mode == 2 ? "双人联机" : "单人 vs AI");

    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(px, 44);
    _canvas->printf("黑 %d", nb);
    _canvas->setCursor(px, 60);
    _canvas->printf("白 %d", nw);

    if (_mode == 2 && !_started) {
        // 准备状态
        _canvas->setCursor(px, 82);
        _canvas->setTextColor(_ready_local ? TFT_GREEN : TFT_YELLOW);
        _canvas->print(_ready_local ? "我 OK" : "Enter备");
        _canvas->setCursor(px, 98);
        _canvas->setTextColor(_ready_remote ? TFT_GREEN : TFT_YELLOW);
        if (glink_remote_flags() & 0x01) _canvas->print(_ready_remote ? "对方 OK" : "对方 --");
        else { _canvas->setTextColor(TFT_RED); _canvas->print("等手柄.."); }
    } else if (_over) {
        _canvas->setCursor(px, 82);
        _canvas->setTextColor(TFT_YELLOW);
        const char* wt = (_winner == 3) ? "平局!" : ((_winner == 1) ? "黑方胜!" : "白方胜!");
        _canvas->print(wt);
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(px, 98);
        _canvas->print(_mode == 2 ? "双方备重开" : "Enter重开");
    } else {
        _canvas->setCursor(px, 82);
        _canvas->setTextColor(_cur == 1 ? TFT_GREEN : TFT_LIGHTGREY);
        _canvas->print(_cur == 1 ? "黑方走" : "白方走");
        if (_must_pass) {
            _canvas->setTextColor(TFT_ORANGE);
            _canvas->setCursor(px, 98);
            _canvas->print(_pass_player == 1 ? "黑方pass" : "白方pass");
        }
    }

    _canvas->pushSprite(0, 0);
}

// RVSTATE: 'G','M',0x22,seq | 4-19 棋盘64格x2bit | 20 cursorX | 21 cursorY
//          22 black | 23 white | 24 flags | 25 当前方 | 26 winner | 27 lastR | 28 lastC
// flags: bit0联机 bit1开局 bit2结束 bit3本地备 bit4远端备 bit5须pass bit6单人
void AppReversi::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[29];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x22;
    for (int i = 0; i < 64; i++)
        p[4 + i / 4] |= (uint8_t)(_b[i] & 3) << ((i % 4) * 2);
    int nb = 0, nw = 0;
    for (int i = 0; i < 64; i++) {
        if (_b[i] == 1) nb++;
        else if (_b[i] == 2) nw++;
    }
    p[20] = (uint8_t)_cx;
    p[21] = (uint8_t)_cy;
    p[22] = (uint8_t)nb;
    p[23] = (uint8_t)nw;
    p[24] = (_mode == 2 ? 0x01 : 0)
          | ((_mode != 0 && (_started || _mode == 1)) ? 0x02 : 0)
          | (_over ? 0x04 : 0)
          | (_ready_local ? 0x08 : 0)
          | (_ready_remote ? 0x10 : 0)
          | (_must_pass ? 0x20 : 0)
          | (_mode == 1 ? 0x40 : 0);
    p[25] = (uint8_t)_cur;
    p[26] = (uint8_t)_winner;
    p[27] = (uint8_t)(_last_r < 0 ? 255 : _last_r);
    p[28] = (uint8_t)(_last_c < 0 ? 255 : _last_c);
    glink_send_state(p, sizeof(p));
}

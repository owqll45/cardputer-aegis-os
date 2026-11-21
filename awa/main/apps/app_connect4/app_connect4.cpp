#include "app_connect4.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>
#include <string.h>

AppConnect4::AppConnect4() {
    setAppInfo().name = "Connect4";
    setAppInfo().icon = nullptr;
}

AppConnect4::~AppConnect4() { _canvas = nullptr; }

// ==C4CORE_BEGIN==
// ================= 规则核(纯函数, 宿主可测) =================
#define C4_W 7
#define C4_H 6

// 该列落子后的行号(从底往上找空格), 满列返回 -1
static int c4_drop_row(const uint8_t* b, int col)
{
    for (int r = C4_H - 1; r >= 0; r--)
        if (b[r * C4_W + col] == 0) return r;
    return -1;
}

// 胜利检测: 返回胜方(0无), *widx/*wdir 记录胜利线(起点格 + 方向 0横1竖2撇\3捺/)
static int c4_check_win(const uint8_t* b, int* widx, int* wdir)
{
    static const int8_t D[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
    for (int r = 0; r < C4_H; r++) {
        for (int c = 0; c < C4_W; c++) {
            uint8_t v = b[r * C4_W + c];
            if (!v) continue;
            for (int d = 0; d < 4; d++) {
                int dr = D[d][0], dc = D[d][1];
                int r3 = r + dr * 3, c3 = c + dc * 3;
                if (r3 < 0 || r3 >= C4_H || c3 < 0 || c3 >= C4_W) continue;
                if (b[(r + dr) * C4_W + c + dc] == v &&
                    b[(r + 2 * dr) * C4_W + c + 2 * dc] == v &&
                    b[(r + 3 * dr) * C4_W + c + 3 * dc] == v) {
                    if (widx) *widx = r * C4_W + c;
                    if (wdir) *wdir = d;
                    return v;
                }
            }
        }
    }
    return 0;
}

// ---- AI: 极小化极大 + alpha-beta + 中列优先排序 ----
static uint32_t c4_rng_s = 0x2F6E2B1u;
static uint32_t c4_rand() { c4_rng_s = c4_rng_s * 1664525u + 1013904223u; return c4_rng_s >> 8; }
static void c4_srand(uint32_t s) { c4_rng_s = s ? s : 1u; }

// 局面评估(对 p 方视角)
static int c4_eval(const uint8_t* b, int p)
{
    int opp = (p == 1) ? 2 : 1;
    int score = 0;
    for (int r = 0; r < C4_H; r++)
        if (b[r * C4_W + 3] == p) score += 3; // 中列偏好
    static const int8_t D[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
    for (int r = 0; r < C4_H; r++) {
        for (int c = 0; c < C4_W; c++) {
            for (int d = 0; d < 4; d++) {
                int dr = D[d][0], dc = D[d][1];
                int r3 = r + dr * 3, c3 = c + dc * 3;
                if (r3 < 0 || r3 >= C4_H || c3 < 0 || c3 >= C4_W) continue;
                int nm = 0, no = 0;
                for (int k = 0; k < 4; k++) {
                    uint8_t v = b[(r + dr * k) * C4_W + c + dc * k];
                    if (v == p) nm++;
                    else if (v == opp) no++;
                }
                if (nm && no) continue; // 混杂窗口无价值
                if (nm == 4) score += 100000;
                else if (nm == 3) score += 50;
                else if (nm == 2) score += 10;
                if (no == 3) score -= 80; // 优先堵对方三连
            }
        }
    }
    return score;
}

static int c4_minimax(uint8_t* b, int depth, int alpha, int beta, int cur, int me, int ply)
{
    int w = c4_check_win(b, nullptr, nullptr);
    if (w == me) return 1000000 - ply; // 越快赢越好
    if (w) return ply - 1000000;       // 越慢输越好
    if (depth == 0) return c4_eval(b, me);

    static const int8_t order[7] = {3, 2, 4, 1, 5, 0, 6};
    bool any = false;
    if (cur == me) {
        int best = -2000000000;
        for (int i = 0; i < 7; i++) {
            int r = c4_drop_row(b, order[i]);
            if (r < 0) continue;
            any = true;
            b[r * C4_W + order[i]] = (uint8_t)cur;
            int s = c4_minimax(b, depth - 1, alpha, beta, 3 - cur, me, ply + 1);
            b[r * C4_W + order[i]] = 0;
            if (s > best) best = s;
            if (s > alpha) alpha = s;
            if (beta <= alpha) break;
        }
        return any ? best : 0; // 满=平局
    } else {
        int best = 2000000000;
        for (int i = 0; i < 7; i++) {
            int r = c4_drop_row(b, order[i]);
            if (r < 0) continue;
            any = true;
            b[r * C4_W + order[i]] = (uint8_t)cur;
            int s = c4_minimax(b, depth - 1, alpha, beta, 3 - cur, me, ply + 1);
            b[r * C4_W + order[i]] = 0;
            if (s < best) best = s;
            if (s < beta) beta = s;
            if (beta <= alpha) break;
        }
        return any ? best : 0;
    }
}

// 选最佳列(0-6, 同分随机), 无合法列返回 -1
static int c4_ai_move(const uint8_t* bin, int me, int depth)
{
    uint8_t b[42];
    memcpy(b, bin, 42);
    static const int8_t order[7] = {3, 2, 4, 1, 5, 0, 6};
    int bestc = -1, bests = -2000000000, nbest = 0;
    for (int i = 0; i < 7; i++) {
        int r = c4_drop_row(b, order[i]);
        if (r < 0) continue;
        b[r * C4_W + order[i]] = (uint8_t)me;
        int s = c4_minimax(b, depth - 1, -2000000000, 2000000000, 3 - me, me, 1);
        b[r * C4_W + order[i]] = 0;
        if (s > bests) { bests = s; bestc = order[i]; nbest = 1; }
        else if (s == bests && (c4_rand() % ++nbest) == 0) bestc = order[i];
    }
    return bestc;
}
// ==C4CORE_END==

void AppConnect4::resetGame() {
    memset(_b, 0, sizeof(_b));
    _cur = 1;
    _over = false;
    _winner = 0;
    _win_idx = -1;
    _win_dir = 0;
    _last_c = _last_r = -1;
    _moves = 0;
    _sel = 3;
    _hint_col = -1;
    _dirty = true;
}

void AppConnect4::afterDrop() {
    _moves++;
    int w = c4_check_win(_b, &_win_idx, &_win_dir);
    if (w) {
        _over = true;
        _winner = w;
        _score[w - 1]++;
        if (_mode == 2) { _started = false; _ready_local = _ready_remote = false; }
    } else if (_moves >= 42) {
        _over = true;
        _winner = 3;
        if (_mode == 2) { _started = false; _ready_local = _ready_remote = false; }
    } else {
        _cur = (_cur == 1) ? 2 : 1;
    }
    if (_mode == 1 && _cur == 2 && !_over) _ai_timer = GetHAL().millis() + 450;
    updateHint();
    _dirty = true;
}

void AppConnect4::tryDrop() {
    if (_over || !(_mode == 1 || (_mode == 2 && _started))) return;
    if (_cur != 1) return; // 键盘方=红, 只在红方回合有效
    int r = c4_drop_row(_b, _sel);
    if (r < 0) return; // 满列
    _b[r * C4_W + _sel] = 1;
    _last_c = _sel; _last_r = r;
    audio::play_random_tone();
    afterDrop();
}

void AppConnect4::aiMove() {
    int c = c4_ai_move(_b, 2, 7);
    if (c < 0) return;
    int r = c4_drop_row(_b, c);
    _b[r * C4_W + c] = 2;
    _last_c = c; _last_r = r;
    afterDrop();
}

void AppConnect4::updateHint() {
    if (_over || _mode == 0) { _hint_col = -1; return; }
    if (_mode == 2 && !_started) { _hint_col = -1; return; }
    _hint_col = c4_ai_move(_b, _cur, 6);
}

// ================= 联机(手柄方=黄) =================
void AppConnect4::handleNet(uint32_t now) {
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
        // 左右选列: 按下走一列, 长按连走
        uint16_t dirs = (uint16_t)(m & 0x000C); // 只要左右
        int dx = 0;
        if (pressed & 0x0004) dx = -1;
        else if (pressed & 0x0008) dx = 1;
        if (dx) {
            _hold_dir = dirs; _hold_ms = now; _rep_ms = now;
        } else if (dirs && dirs == _hold_dir && now - _hold_ms > 420 && now - _rep_ms > 120) {
            _rep_ms = now;
            if (dirs & 0x0004) dx = -1;
            else dx = 1;
        }
        if (!dirs) _hold_dir = 0;
        if (dx) {
            int ns = _sel + dx;
            if (ns < 0) ns = 0;
            if (ns > 6) ns = 6;
            if (ns != _sel) { _sel = ns; _dirty = true; }
        }
        if (pressed & 0x0010) {  // A = 落子
            int r = c4_drop_row(_b, _sel);
            if (r >= 0) {
                _b[r * C4_W + _sel] = 2;
                _last_c = _sel; _last_r = r;
                afterDrop();
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

void AppConnect4::onOpen() {
    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    glink_acquire("Connect4", GLINK_GID_CONNECT4, GLINK_ICON_CONNECT4);

    _mode = 0;
    _menu_sel = 0;
    _started = false;
    _ready_local = _ready_remote = false;
    _score[0] = _score[1] = 0;
    _net_btn_prev = glink_buttons();
    c4_srand(esp_random());
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
                _score[0] = _score[1] = 0;
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

        // 棋盘操作(键盘方=红)
        if (_cur != 1) return;
        if (e.keyCode == KEY_LEFT || e.keyCode == KEY_A) {
            if (_sel > 0) { _sel--; _dirty = true; }
        } else if (e.keyCode == KEY_RIGHT || e.keyCode == KEY_D) {
            if (_sel < 6) { _sel++; _dirty = true; }
        } else if (e.keyCode == KEY_ENTER || e.keyCode == KEY_S || e.keyCode == KEY_DOWN ||
                   e.keyCode == KEY_SPACE) {
            tryDrop();
        }
    });
}

void AppConnect4::onRunning() {
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
        _last_mirror = now;
        sendMirrorState();
    }
}

void AppConnect4::onClose() {
    if (_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    glink_release();

    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

// ================= 绘制 =================
void AppConnect4::draw() {
    if (!_canvas) return;
    int w = _canvas->width(), h = _canvas->height();
    _canvas->fillScreen(TFT_BLACK);
    _canvas->setFont(&fonts::efontCN_12);

    if (_mode == 0) {
        _canvas->setTextColor(TFT_CYAN);
        int tw = _canvas->textWidth("Connect4 四子棋");
        _canvas->setCursor((w - tw) / 2, 18);
        _canvas->print("Connect4 四子棋");
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
        appCanvasPush(_canvas);
        return;
    }

    // 棋盘: 7x16=112 宽, 6x16=96 高, 左下
    const int CELL = 16;
    const int BW = 7 * CELL, BH = 6 * CELL;
    const int BX = 6, BY = h - 6 - BH; // BY = 33

    // 选列箭头(轮到自己或联机进行中都画)
    if (!_over && (_cur == 1 || _mode == 2)) {
        int ax = BX + _sel * CELL + CELL / 2;
        _canvas->fillTriangle(ax - 5, BY - 9, ax + 5, BY - 9, ax, BY - 2,
                              _cur == 1 ? TFT_RED : TFT_YELLOW);
    }

    // 蓝色板体(圆孔挖黑)
    _canvas->fillRoundRect(BX - 2, BY - 2, BW + 4, BH + 4, 3, TFT_BLUE);
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 7; c++) {
            int px = BX + c * CELL + CELL / 2, py = BY + r * CELL + CELL / 2;
            uint8_t v = _b[r * 7 + c];
            if (v == 1) {
                _canvas->fillCircle(px, py, 6, TFT_RED);
                _canvas->fillCircle(px - 1, py - 1, 2, 0xFD20); // 高光
            } else if (v == 2) {
                _canvas->fillCircle(px, py, 6, TFT_YELLOW);
                _canvas->fillCircle(px - 1, py - 1, 2, TFT_WHITE);
            } else {
                _canvas->fillCircle(px, py, 6, TFT_BLACK);
            }
        }
    }

    // 胜利线: 白圈
    if (_over && _win_idx >= 0 && _winner != 3) {
        static const int8_t D[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
        int dr = D[_win_dir][0], dc = D[_win_dir][1];
        int r0 = _win_idx / 7, c0 = _win_idx % 7;
        for (int k = 0; k < 4; k++) {
            int px = BX + (c0 + dc * k) * CELL + CELL / 2;
            int py = BY + (r0 + dr * k) * CELL + CELL / 2;
            _canvas->drawCircle(px, py, 6, TFT_WHITE);
            _canvas->drawCircle(px, py, 5, TFT_WHITE);
        }
    }
    // 最后一步: 青圈
    if (_last_r >= 0 && !(_over && _win_idx >= 0)) {
        int px = BX + _last_c * CELL + CELL / 2, py = BY + _last_r * CELL + CELL / 2;
        _canvas->drawCircle(px, py, 7, TFT_CYAN);
    }

    // ---------- 右侧信息 ----------
    int px = BX + BW + 8;
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(px, 4);
    _canvas->print("四子棋");
    _canvas->setTextColor(_mode == 2 ? TFT_MAGENTA : TFT_DARKGREY);
    _canvas->setCursor(px, 20);
    _canvas->print(_mode == 2 ? "双人联机" : "单人 vs AI");

    _canvas->setCursor(px, 40);
    _canvas->setTextColor(TFT_RED);
    _canvas->printf("红 %d", _score[0]);
    _canvas->setCursor(px, 56);
    _canvas->setTextColor(TFT_YELLOW);
    _canvas->printf("黄 %d", _score[1]);

    if (_mode == 2 && !_started) {
        _canvas->setCursor(px, 78);
        _canvas->setTextColor(_ready_local ? TFT_GREEN : TFT_YELLOW);
        _canvas->print(_ready_local ? "我 OK" : "Enter备");
        _canvas->setCursor(px, 94);
        _canvas->setTextColor(_ready_remote ? TFT_GREEN : TFT_YELLOW);
        if (glink_remote_flags() & 0x01) _canvas->print(_ready_remote ? "对方 OK" : "对方 --");
        else { _canvas->setTextColor(TFT_RED); _canvas->print("等手柄.."); }
    } else if (_over) {
        _canvas->setCursor(px, 78);
        _canvas->setTextColor(TFT_YELLOW);
        const char* wt = (_winner == 3) ? "平局!" : ((_winner == 1) ? "红方胜!" : "黄方胜!");
        _canvas->print(wt);
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(px, 94);
        _canvas->print(_mode == 2 ? "双方备重开" : "Enter重开");
    } else {
        _canvas->setCursor(px, 78);
        _canvas->setTextColor(_cur == 1 ? TFT_RED : TFT_YELLOW);
        _canvas->print(_cur == 1 ? "红方落子" : "黄方落子");
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(px, 94);
        _canvas->print("A/D选 Enter落");
    }

    appCanvasPush(_canvas);
}

// CFSTATE: 'G','M',0x25,seq | 4-14 棋盘42格x2bit | 15 sel | 16 flags | 17 cur
//          18 winner | 19 winIdx(255无) | 20 winDir | 21 lastC(255无) | 22 lastR
//          23 score0 | 24 score1 | 25 hintCol(255无) | 26 moves
// flags: bit0联机 bit1开局 bit2结束 bit3本地备 bit4远端备 bit5单人
void AppConnect4::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[27];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x25;
    for (int i = 0; i < 42; i++)
        p[4 + i / 4] |= (uint8_t)(_b[i] & 3) << ((i % 4) * 2);
    p[15] = (uint8_t)_sel;
    p[16] = (_mode == 2 ? 0x01 : 0)
          | ((_mode != 0 && (_started || _mode == 1)) ? 0x02 : 0)
          | (_over ? 0x04 : 0)
          | (_ready_local ? 0x08 : 0)
          | (_ready_remote ? 0x10 : 0)
          | (_mode == 1 ? 0x20 : 0);
    p[17] = (uint8_t)_cur;
    p[18] = (uint8_t)_winner;
    p[19] = (uint8_t)(_win_idx < 0 ? 255 : _win_idx);
    p[20] = (uint8_t)_win_dir;
    p[21] = (uint8_t)(_last_c < 0 ? 255 : _last_c);
    p[22] = (uint8_t)(_last_r < 0 ? 255 : _last_r);
    p[23] = (uint8_t)_score[0];
    p[24] = (uint8_t)_score[1];
    p[25] = (uint8_t)(_hint_col < 0 ? 255 : _hint_col);
    p[26] = (uint8_t)_moves;
    glink_send_state(p, sizeof(p));
}

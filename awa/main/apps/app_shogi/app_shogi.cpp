#include "app_shogi.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <apps/utils/app_canvas.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <apps/utils/glink/glink.h>

// ============================================================
// 棋子编码:
//   1..8   黑方(玩家) 普通
//   9..16  白方(AI)   普通
//   17..24 黑方       成子
//   25..32 白方       成子
// ============================================================

int AppShogi::ownerOf(uint8_t v)
{
    if (v == 0) return 0;
    if (v <= 8) return 1;
    if (v <= 16) return 2;
    if (v <= 24) return 1;
    return 2;
}

int AppShogi::kindOf(uint8_t v)
{
    if (v == 0) return 0;
    if (v <= 8) return v;
    if (v <= 16) return v - 8;
    if (v <= 24) return v - 16;
    return v - 24;
}

bool AppShogi::promotedOf(uint8_t v)
{
    return v > 16;
}

uint8_t AppShogi::makePiece(int owner, int kind, bool promoted)
{
    int base = (owner == 1) ? 0 : 8;
    int p    = promoted ? 16 : 0;
    return (uint8_t)(base + kind + p);
}

static int shogi_abs(int v) { return v < 0 ? -v : v; }

int AppShogi::baseValue(int kind, bool promoted)
{
    if (promoted) {
        switch (kind) {
            case PAWN: return 600;
            case LANCE: return 550;
            case KNIGHT: return 550;
            case SILVER: return 600;
            case BISHOP: return 950;   // 马
            case ROOK: return 1100;    // 龙
            default: return 0;
        }
    }
    switch (kind) {
        case PAWN: return 100;
        case LANCE: return 300;
        case KNIGHT: return 350;
        case SILVER: return 500;
        case GOLD: return 550;
        case BISHOP: return 800;
        case ROOK: return 950;
        case KING: return 100000;
        default: return 0;
    }
}

const char* AppShogi::glyphOf(int kind, int owner)
{
    switch (kind) {
        case PAWN: return "兵";
        case LANCE: return "香";
        case KNIGHT: return "马";
        case SILVER: return "银";
        case GOLD: return "金";
        case BISHOP: return "角";
        case ROOK: return "车";
        case KING: return (owner == 1) ? "王" : "玉";
        default: return "?";
    }
}

AppShogi::AppShogi()
{
    setAppInfo().name = "Shogi";
    setAppInfo().icon = nullptr;
}

AppShogi::~AppShogi()
{
    _canvas = nullptr;
}

void AppShogi::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "Application opened");

    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    // 动态棋盘尺寸: 左右各留一列显示持驹与状态
    int w = _canvas->width();
    int h = _canvas->height();
    int cw = (w - 116) / SIZE;
    int ch = (h - 14) / SIZE;
    _cell = (cw < ch) ? cw : ch;
    if (_cell < 8) _cell = 8;
    if (_cell > 13) _cell = 13;
    _board_w = SIZE * _cell;
    _board_x = (w - _board_w) / 2;
    _board_y = (h - _board_w) / 2;
    if (_board_y < 3) _board_y = 3;

    resetGame();
    drawBoard();

    _keyq.clear();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        // desktop_task：只入队，绘制在 onRunning
        _keyq.push(e);
    });

    glink_acquire("Shogi", GLINK_GID_SHOGI, GLINK_ICON_SHOGI);
}

void AppShogi::onRunning()
{
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }
    glink_poll();

    Keyboard::KeyEvent_t e;
    while (_keyq.pop(e)) {
        handleKey(e);
        if (!_canvas) return;  // handleKey 里可能已经 close()
    }

    // 每帧都发 state 包给 C3 —— onOpen 末尾 drawBoard 发一次,
    // 但玩家回合时 L256 条件成立直接 return, 不会再调 drawBoard,
    // 导致 C3 只有第一帧有 state, 之后永远收不到新的 → 跳回等待画面
    sendMirrorState();

    // Win -> level-up transition animation
    if (_win_anim_active) {
        uint32_t el = GetHAL().millis() - _win_anim_start;
        if (el >= 1800) {
            _win_anim_active = false;
            if (_level_up_pending) {
                _level_up_pending = false;
                if (_level < 5) _level++;
                resetGame();
            }
            drawBoard();
        } else if ((el / 150) % 2 == 0 || el > 700) {
            drawBoard();
        }
        return;
    }

    if (_game_over || _turn != 2 || _prompt || _two_player) {
        _ai_timer = 0;
        return;
    }

    if (_ai_timer == 0) {
        _ai_timer = GetHAL().millis();
        return;
    }

    if (GetHAL().millis() - _ai_timer < 300) return;

    _ai_timer = 0;
    _ai_thinking = true;
    drawBoard();

    aiDecide();

    _ai_thinking = false;
    if (_game_over) {
        drawBoard();
        return;
    }
    commitMove(_best_move, false);
}

void AppShogi::onClose()
{
    glink_release();
    _keyq.clear();
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }

    _canvas = &GetHAL().canvas();
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

void AppShogi::handleKey(const Keyboard::KeyEvent_t& e)
{
    if (e.isModifier || !e.state) return;

    if (e.keyCode == KEY_ESC || e.keyCode == KEY_BACKSPACE) {
        close();
        return;
    }

    // H: 帮助层开关(不改变棋局)
    if (e.keyCode == KEY_H) {
        _help = !_help;
        drawBoard();
        return;
    }
    // M: 单人 / 双人同屏切换
    if (e.keyCode == KEY_M) {
        _two_player = !_two_player;
        _ai_thinking = false;
        _ai_timer = 0;
        drawBoard();
        return;
    }

    if (_help) return;  // 帮助层打开时只响应 H/ESC, 防止误操作棋盘

    // 成子确认（必须显式选择，绝不自动成子）
    if (_prompt) {
        if (e.keyCode == KEY_Y) {
            Move m = _pending;
            m.promote = true;
            _prompt = false;
            if (isLegal(m, 1)) commitMove(m, true);
            else drawBoard();
            return;
        }
        if (e.keyCode == KEY_N) {
            Move m = _pending;
            m.promote = false;
            _prompt = false;
            if (isLegal(m, 1)) commitMove(m, true);
            else drawBoard();
            return;
        }
        return;
    }

    if (_win_anim_active) return;

    switch (e.keyCode) {
        case KEY_R:
            resetGame();
            drawBoard();
            return;
        case KEY_L:
            _level = (_level % 5) + 1;
            drawBoard();
            return;
        case KEY_U:
            handleUndo();
            return;
        case KEY_Z: {
            if (_turn != 1 || _game_over) return;
            int next = 0;
            for (int step = 1; step <= 7; step++) {
                int k = ((_drop_kind + step - 1) % 7) + 1;
                if (_hand[0][k] > 0) {
                    next = k;
                    break;
                }
            }
            if (next == _drop_kind) next = 0;
            _drop_kind = next;
            _sel_r = _sel_c = -1;
            drawBoard();
            return;
        }
        case KEY_X:
            _drop_kind = 0;
            _sel_r = _sel_c = -1;
            drawBoard();
            return;
        case KEY_ENTER:
            handleEnter();
            return;
        case KEY_UP:
        case KEY_E:
        case KEY_DOT:
            if (_cursor_r > 0) _cursor_r--;
            drawBoard();
            return;
        case KEY_DOWN:
        case KEY_S:
        case KEY_SLASH:
            if (_cursor_r < 8) _cursor_r++;
            drawBoard();
            return;
        case KEY_LEFT:
        case KEY_A:
        case KEY_COMMA:
            if (_cursor_c > 0) _cursor_c--;
            drawBoard();
            return;
        case KEY_RIGHT:
        case KEY_D:
        case KEY_SEMICOLON:
            if (_cursor_c < 8) _cursor_c++;
            drawBoard();
            return;
        default:
            return;
    }
}

void AppShogi::setupBoard()
{
    memset(_board, 0, sizeof(_board));
    memset(_hand, 0, sizeof(_hand));

    const int back[9] = {LANCE, KNIGHT, SILVER, GOLD, KING, GOLD, SILVER, KNIGHT, LANCE};
    for (int c = 0; c < 9; c++) {
        _board[0 * 9 + c] = makePiece(2, back[c], false);
        _board[8 * 9 + c] = makePiece(1, back[c], false);
    }

    // 角/飞
    _board[1 * 9 + 1] = makePiece(2, ROOK, false);
    _board[1 * 9 + 7] = makePiece(2, BISHOP, false);
    _board[7 * 9 + 1] = makePiece(1, BISHOP, false);
    _board[7 * 9 + 7] = makePiece(1, ROOK, false);

    // 步兵
    for (int c = 0; c < 9; c++) {
        _board[2 * 9 + c] = makePiece(2, PAWN, false);
        _board[6 * 9 + c] = makePiece(1, PAWN, false);
    }
}

void AppShogi::resetGame()
{
    setupBoard();
    _turn        = 1;
    _cursor_r    = 6;
    _cursor_c    = 4;
    _sel_r       = -1;
    _sel_c       = -1;
    _drop_kind   = 0;
    _prompt      = false;
    _game_over   = false;
    _winner      = 0;
    _checked     = false;
    _move_count  = 0;
    _ai_timer    = 0;
    _ai_thinking = false;
    _last_move   = Move{-1, -1, 0, false};
    _history.clear();
    _win_anim_active  = false;
    _level_up_pending = false;
}

bool AppShogi::inBounds(int r, int c)
{
    return r >= 0 && r < 9 && c >= 0 && c < 9;
}

bool AppShogi::inPromotionZone(int owner, int r)
{
    return (owner == 1) ? (r <= 2) : (r >= 6);
}

bool AppShogi::canPromoteKind(int kind)
{
    return kind != GOLD && kind != KING;
}

bool AppShogi::pawnFileHasOwn(int owner, int c)
{
    for (int r = 0; r < 9; r++) {
        uint8_t v = _board[r * 9 + c];
        if (v && ownerOf(v) == owner && kindOf(v) == PAWN && !promotedOf(v)) return true;
    }
    return false;
}

bool AppShogi::dropLegal(int side, int kind, int r, int c)
{
    int last = (side == 1) ? 0 : 8;
    if (kind == PAWN) {
        if (r == last) return false;                 // 步不能打到底线
        if (pawnFileHasOwn(side, c)) return false;   // 二步
    } else if (kind == LANCE) {
        if (r == last) return false;
    } else if (kind == KNIGHT) {
        if (r == last || r == ((side == 1) ? 1 : 7)) return false;
    }
    return true;
}

void AppShogi::applyMove(const Move& m, Undo& u)
{
    int side = _turn;
    u.side     = (uint8_t)side;
    u.from     = m.from;
    u.to       = m.to;
    u.drop     = m.drop;
    u.moved    = 0;
    u.captured = 0;

    if (m.from < 0) {
        u.moved = makePiece(side, m.drop, false);
        _board[m.to] = u.moved;
        if (_hand[side - 1][m.drop] > 0) _hand[side - 1][m.drop]--;
    } else {
        u.moved    = _board[m.from];
        u.captured = _board[m.to];
        int kind   = kindOf(u.moved);
        bool prom  = promotedOf(u.moved) || m.promote;
        _board[m.from] = 0;
        _board[m.to]   = makePiece(side, kind, prom);
    }

    if (u.captured) {
        int ck = kindOf(u.captured);
        if (ck >= PAWN && ck <= ROOK && _hand[side - 1][ck] < 250) {
            _hand[side - 1][ck]++;
        }
    }

    _turn = (uint8_t)(3 - side);
}

void AppShogi::revertMove(const Move& m, const Undo& u)
{
    int side = u.side;
    _turn = (uint8_t)side;

    if (u.captured) {
        int ck = kindOf(u.captured);
        if (ck >= PAWN && ck <= ROOK && _hand[side - 1][ck] > 0) _hand[side - 1][ck]--;
        _board[m.to] = u.captured;
    } else {
        _board[m.to] = 0;
    }

    if (m.from < 0) {
        if (_hand[side - 1][m.drop] < 250) _hand[side - 1][m.drop]++;
    } else {
        _board[m.from] = u.moved;
    }
}

void AppShogi::genPseudoMoves(int side, Move* out, int& n, int maxN)
{
    n = 0;
    const int f = (side == 1) ? -1 : 1;

    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            int idx = r * 9 + c;
            uint8_t v = _board[idx];
            if (!v || ownerOf(v) != side) continue;

            int kind  = kindOf(v);
            bool prom = promotedOf(v);

            auto add = [&](int tr, int tc) {
                if (n >= maxN) return;
                if (!inBounds(tr, tc)) return;
                uint8_t t = _board[tr * 9 + tc];
                if (t && ownerOf(t) == side) return;

                Move m{(int8_t)idx, (int8_t)(tr * 9 + tc), 0, false};

                bool promotable = canPromoteKind(kind) && !prom;
                bool zone = inPromotionZone(side, r) || inPromotionZone(side, tr);

                if (promotable && zone) {
                    Move p = m;
                    p.promote = true;
                    // 强制成子: 步/香到底线, 桂到最下两线
                    bool forced = false;
                    if (kind == PAWN || kind == LANCE) {
                        forced = (tr == (side == 1 ? 0 : 8));
                    } else if (kind == KNIGHT) {
                        forced = (tr == (side == 1 ? 0 : 8)) || (tr == (side == 1 ? 1 : 7));
                    }
                    if (forced) {
                        out[n++] = p;
                        return;
                    }
                    out[n++] = m;
                    if (n < maxN) out[n++] = p;
                } else {
                    out[n++] = m;
                }
            };

            if (kind == PAWN && !prom) {
                add(r + f, c);
            } else if (kind == LANCE && !prom) {
                for (int rr = r + f; inBounds(rr, c); rr += f) {
                    add(rr, c);
                    if (_board[rr * 9 + c]) break;
                }
            } else if (kind == KNIGHT && !prom) {
                add(r + 2 * f, c - 1);
                add(r + 2 * f, c + 1);
            } else if (kind == SILVER && !prom) {
                add(r + f, c - 1);
                add(r + f, c);
                add(r + f, c + 1);
                add(r - f, c - 1);
                add(r - f, c + 1);
            } else if (kind == GOLD ||
                       (prom && (kind == PAWN || kind == LANCE || kind == KNIGHT || kind == SILVER))) {
                add(r + f, c - 1);
                add(r + f, c);
                add(r + f, c + 1);
                add(r, c - 1);
                add(r, c + 1);
                add(r - f, c);
            } else if (kind == KING) {
                for (int dr = -1; dr <= 1; dr++) {
                    for (int dc = -1; dc <= 1; dc++) {
                        if (dr || dc) add(r + dr, c + dc);
                    }
                }
            } else if (kind == BISHOP) {
                static const int drs[4] = {1, 1, -1, -1};
                static const int dcs[4] = {1, -1, 1, -1};
                for (int d = 0; d < 4; d++) {
                    int rr = r + drs[d];
                    int cc = c + dcs[d];
                    while (inBounds(rr, cc)) {
                        add(rr, cc);
                        if (_board[rr * 9 + cc]) break;
                        rr += drs[d];
                        cc += dcs[d];
                    }
                }
                if (prom) {
                    add(r - 1, c);
                    add(r + 1, c);
                    add(r, c - 1);
                    add(r, c + 1);
                }
            } else if (kind == ROOK) {
                static const int drs[4] = {1, -1, 0, 0};
                static const int dcs[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; d++) {
                    int rr = r + drs[d];
                    int cc = c + dcs[d];
                    while (inBounds(rr, cc)) {
                        add(rr, cc);
                        if (_board[rr * 9 + cc]) break;
                        rr += drs[d];
                        cc += dcs[d];
                    }
                }
                if (prom) {
                    add(r - 1, c - 1);
                    add(r - 1, c + 1);
                    add(r + 1, c - 1);
                    add(r + 1, c + 1);
                }
            }
        }
    }

    // ---- 打入 ----
    for (int kind = PAWN; kind <= ROOK; kind++) {
        if (_hand[side - 1][kind] == 0) continue;
        for (int r = 0; r < 9; r++) {
            for (int c = 0; c < 9; c++) {
                if (_board[r * 9 + c]) continue;
                if (!dropLegal(side, kind, r, c)) continue;
                if (n >= maxN) return;
                out[n++] = Move{-1, (int8_t)(r * 9 + c), (uint8_t)kind, false};
            }
        }
    }
}

bool AppShogi::attackedSquare(int r, int c, int attacker)
{
    for (int pr = 0; pr < 9; pr++) {
        for (int pc = 0; pc < 9; pc++) {
            uint8_t v = _board[pr * 9 + pc];
            if (!v || ownerOf(v) != attacker) continue;

            int kind  = kindOf(v);
            bool prom = promotedOf(v);
            int dr    = r - pr;
            int dc    = c - pc;
            if (dr == 0 && dc == 0) continue;

            int f   = (attacker == 1) ? -1 : 1;
            int fd  = dr * f;
            int adr = shogi_abs(dr);
            int adc = shogi_abs(dc);

            if (kind == PAWN && !prom) {
                if (fd == 1 && dc == 0) return true;
            } else if (kind == LANCE && !prom) {
                if (dc == 0 && fd > 0) {
                    int step = (dr > 0) ? 1 : -1;
                    bool clear = true;
                    for (int rr = pr + step; rr != r; rr += step) {
                        if (_board[rr * 9 + pc]) {
                            clear = false;
                            break;
                        }
                    }
                    if (clear) return true;
                }
            } else if (kind == KNIGHT && !prom) {
                if (fd == 2 && adc == 1) return true;
            } else if (kind == SILVER && !prom) {
                if ((fd == 1 && adc <= 1) || (fd == -1 && adc == 1)) return true;
            } else if (kind == GOLD ||
                       (prom && (kind == PAWN || kind == LANCE || kind == KNIGHT || kind == SILVER))) {
                if ((fd == 1 && adc <= 1) || (fd == 0 && adc == 1) || (fd == -1 && dc == 0)) return true;
            } else if (kind == KING) {
                if (adr <= 1 && adc <= 1) return true;
            } else if (kind == BISHOP) {
                if (adr == adc && adr > 0) {
                    int sr = (dr > 0) ? 1 : -1;
                    int sc = (dc > 0) ? 1 : -1;
                    bool clear = true;
                    for (int k = 1; k < adr; k++) {
                        if (_board[(pr + sr * k) * 9 + (pc + sc * k)]) {
                            clear = false;
                            break;
                        }
                    }
                    if (clear) return true;
                }
                if (prom && ((adr == 1 && adc == 0) || (adr == 0 && adc == 1))) return true;
            } else if (kind == ROOK) {
                if ((dr == 0) != (dc == 0)) {
                    int sr = (dr > 0) - (dr < 0);
                    int sc = (dc > 0) - (dc < 0);
                    int len = (dr != 0) ? adr : adc;
                    bool clear = true;
                    for (int k = 1; k < len; k++) {
                        if (_board[(pr + sr * k) * 9 + (pc + sc * k)]) {
                            clear = false;
                            break;
                        }
                    }
                    if (clear) return true;
                }
                if (prom && adr == 1 && adc == 1) return true;
            }
        }
    }
    return false;
}

bool AppShogi::kingAttacked(int side)
{
    for (int i = 0; i < 81; i++) {
        uint8_t v = _board[i];
        if (v && ownerOf(v) == side && kindOf(v) == KING) {
            return attackedSquare(i / 9, i % 9, 3 - side);
        }
    }
    return true;  // 王已不存在
}

bool AppShogi::hasAnyLegalMove(int side)
{
    Move pseudo[MAX_MOVES];
    int pn = 0;
    genPseudoMoves(side, pseudo, pn, MAX_MOVES);

    uint8_t saved = _turn;
    for (int i = 0; i < pn; i++) {
        _turn = (uint8_t)side;
        Undo u;
        applyMove(pseudo[i], u);
        bool ok = !kingAttacked(side);
        revertMove(pseudo[i], u);
        _turn = saved;
        if (ok) return true;
    }
    return false;
}

void AppShogi::genLegalMoves(Move* out, int& n, int maxN)
{
    Move pseudo[MAX_MOVES];
    int pn = 0;
    int side = _turn;
    genPseudoMoves(side, pseudo, pn, MAX_MOVES);

    n = 0;
    for (int i = 0; i < pn && n < maxN; i++) {
        Undo u;
        applyMove(pseudo[i], u);
        bool ok = !kingAttacked(side);

        // 打步诘: 打入步兵直接形成将死属于犯规
        if (ok && pseudo[i].drop == PAWN && kingAttacked(3 - side)) {
            if (!hasAnyLegalMove(3 - side)) ok = false;
        }

        revertMove(pseudo[i], u);
        if (ok) out[n++] = pseudo[i];
    }
}

bool AppShogi::isLegal(const Move& m, int side)
{
    uint8_t saved = _turn;
    _turn = (uint8_t)side;

    Undo u;
    applyMove(m, u);
    bool ok = !kingAttacked(side);
    if (ok && m.drop == PAWN && kingAttacked(3 - side)) {
        if (!hasAnyLegalMove(3 - side)) ok = false;
    }
    revertMove(m, u);

    _turn = saved;
    return ok;
}

bool AppShogi::mustPromote(const Move& m, int side)
{
    if (m.drop || m.from < 0) return false;
    uint8_t v = _board[m.from];
    if (!v) return false;
    int kind = kindOf(v);
    int tr   = m.to / 9;
    int last = (side == 1) ? 0 : 8;
    if (kind == PAWN || kind == LANCE) return tr == last;
    if (kind == KNIGHT) return tr == last || tr == ((side == 1) ? 1 : 7);
    return false;
}

bool AppShogi::mayPromote(const Move& m, int side)
{
    if (m.drop || m.from < 0) return false;
    uint8_t v = _board[m.from];
    if (!v) return false;
    int kind  = kindOf(v);
    bool prom = promotedOf(v);
    if (prom || !canPromoteKind(kind)) return false;
    int fr = m.from / 9;
    int tr = m.to / 9;
    return inPromotionZone(side, fr) || inPromotionZone(side, tr);
}

void AppShogi::pushSnapshot()
{
    Snapshot s;
    memcpy(s.board, _board, sizeof(_board));
    memcpy(s.hand, _hand, sizeof(_hand));
    s.turn = _turn;
    _history.push_back(s);
    if (_history.size() > 160) _history.erase(_history.begin());
}

void AppShogi::restoreSnapshot(const Snapshot& s)
{
    memcpy(_board, s.board, sizeof(_board));
    memcpy(_hand, s.hand, sizeof(_hand));
    _turn = s.turn;
}

void AppShogi::handleEnter()
{
    /* 以前所有"按了没反应"的分支都是直接 return, 玩家分不清是键坏了、
     * 还是这步不合规。这里统一给一行短提示。 */
    auto tip = [this](const char* s) {
        snprintf(_msg, sizeof(_msg), "%s", s);
        _msg_until = GetHAL().millis() + 1500;
        drawBoard();
    };

    if (_game_over) { tip("本局已结束，按 R 重开"); return; }
    if (_ai_thinking) { tip("AI 正在思考，请稍等"); return; }
    if (!_two_player && _turn != 1) { tip("等 AI 走完这一手"); return; }  // 单人模式仍只由玩家执下方

    const int side = _turn;  // 双人同屏: 轮到哪方就操作哪方

    int r = _cursor_r;
    int c = _cursor_c;
    int idx = r * 9 + c;

    // ---- 打入模式 ----
    if (_drop_kind > 0) {
        Move m{-1, (int8_t)idx, (uint8_t)_drop_kind, false};
        if (isLegal(m, side)) commitMove(m, true);
        else tip("这里不能打子");
        return;
    }

    // ---- 选择自己的棋子 ----
    if (_sel_r < 0) {
        uint8_t v = _board[idx];
        if (v && ownerOf(v) == side) {
            _sel_r = r;
            _sel_c = c;
            drawBoard();
        } else if (v) {
            tip("这不是你的子");
        } else {
            tip("先选自己的棋子");
        }
        return;
    }

    // 再按一次取消选择
    if (_sel_r == r && _sel_c == c) {
        _sel_r = _sel_c = -1;
        drawBoard();
        return;
    }

    // 选择另一枚自己的棋子
    uint8_t target = _board[idx];
    if (target && ownerOf(target) == side) {
        _sel_r = r;
        _sel_c = c;
        drawBoard();
        return;
    }

    Move m{(int8_t)(_sel_r * 9 + _sel_c), (int8_t)idx, 0, false};

    if (mayPromote(m, side)) {
        // 先确认这步棋本身合法，再询问是否成子
        if (!isLegal(m, side)) { tip("这步走不了"); return; }
        _pending = m;
        if (mustPromote(m, side)) {
            m.promote = true;
            if (isLegal(m, side)) commitMove(m, true);
        } else {
            _prompt = true;
            drawBoard();
        }
        return;
    }

    if (isLegal(m, side)) {
        commitMove(m, true);
    } else {
        tip("这步走不了");
    }
}

void AppShogi::handleUndo()
{
    if (_prompt) {
        _prompt = false;
        drawBoard();
        return;
    }
    if (_history.empty()) return;

    for (int i = 0; i < 2 && !_history.empty(); i++) {
        restoreSnapshot(_history.back());
        _history.pop_back();
        if (_turn == 1) break;
    }

    _game_over  = false;
    _winner     = 0;
    _checked    = kingAttacked(_turn);
    _sel_r      = -1;
    _sel_c      = -1;
    _drop_kind  = 0;
    _ai_timer   = 0;
    _last_move  = Move{-1, -1, 0, false};
    drawBoard();
}

void AppShogi::commitMove(const Move& m, bool human)
{
    pushSnapshot();
    Undo u;
    applyMove(m, u);
    _last_move = m;
    _sel_r = _sel_c = -1;
    _drop_kind = 0;
    _move_count++;

    checkGameEnd();

    if (human) audio::play_random_tone();
    drawBoard();
}

void AppShogi::checkGameEnd()
{
    _checked = kingAttacked(_turn);
    if (!hasAnyLegalMove(_turn)) {
        _game_over = true;
        _winner    = 3 - _turn;
        if (_winner == 1) {
            _win_anim_start   = GetHAL().millis();
            _win_anim_active  = true;
            _level_up_pending = true;
        }
    }
}

// ============================================================
// 评估与搜索
// ============================================================

int AppShogi::evaluateBoard()
{
    int score = 0;

    for (int i = 0; i < 81; i++) {
        uint8_t v = _board[i];
        if (!v) continue;
        int owner = ownerOf(v);
        int kind  = kindOf(v);
        bool prom = promotedOf(v);
        int r     = i / 9;
        int c     = i % 9;

        int val = baseValue(kind, prom);
        val += ((owner == 1) ? (8 - r) : r) * 3;
        if (kind == SILVER || kind == GOLD) val += 4 - shogi_abs(c - 4);

        if (owner == 1) score += val;
        else score -= val;
    }

    for (int k = PAWN; k <= ROOK; k++) {
        score += _hand[0][k] * baseValue(k, false);
        score -= _hand[1][k] * baseValue(k, false);
    }

    return score;
}

int AppShogi::evaluateFor(int side)
{
    int e = evaluateBoard();
    return (side == 1) ? e : -e;
}

int AppShogi::moveOrderScore(const Move& m)
{
    if (m.drop) return 50;
    int s = 0;
    uint8_t target = _board[m.to];
    if (target) s += 1000 + baseValue(kindOf(target), promotedOf(target));
    if (m.promote) s += 700;
    return s;
}

int AppShogi::aiSearch(int depth, int alpha, int beta, int ply)
{
    if (_abort) return 0;

    _nodes++;
    if ((_nodes & 63) == 0 && esp_timer_get_time() >= _deadline_us) {
        _abort = true;
        return 0;
    }

    if (depth <= 0) return evaluateFor(_turn);

    Move* moves = _move_buf[ply];
    int n = 0;
    genLegalMoves(moves, n, MAX_MOVES);

    if (n == 0) return -900000 + ply;  // 无棋可走 = 负

    // 简单插入排序: 吃子/成子优先
    for (int i = 1; i < n; i++) {
        Move key = moves[i];
        int ks = moveOrderScore(key);
        int j = i - 1;
        while (j >= 0 && moveOrderScore(moves[j]) < ks) {
            moves[j + 1] = moves[j];
            j--;
        }
        moves[j + 1] = key;
    }

    int best = -1000000;
    for (int i = 0; i < n; i++) {
        Move m = moves[i];
        Undo u;
        applyMove(m, u);
        int v = -aiSearch(depth - 1, -beta, -alpha, ply + 1);
        revertMove(m, u);

        if (_abort) return 0;
        if (v > best) best = v;
        if (v > alpha) alpha = v;
        if (alpha >= beta) break;
    }

    return best;
}

void AppShogi::aiDecide()
{
    Move* root = _move_buf[0];
    int n = 0;
    genLegalMoves(root, n, MAX_MOVES);

    if (n == 0) {
        _game_over = true;
        _winner    = 1;
        return;
    }

    _best_move = root[0];

    if (_level <= 1) {
        _best_move = root[esp_random() % n];
        return;
    }

    if (_level == 2) {
        int bestScore = -1000000;
        for (int i = 0; i < n; i++) {
            int s = moveOrderScore(root[i]) + (int)(esp_random() % 80);
            if (s > bestScore) {
                bestScore = s;
                _best_move = root[i];
            }
        }
        return;
    }

    int maxDepth = (_level == 3) ? 2 : ((_level == 4) ? 3 : 4);
    uint32_t budget = (_level == 3) ? 90 : ((_level == 4) ? 220 : 380);

    _deadline_us = esp_timer_get_time() + (int64_t)budget * 1000;
    _abort       = false;

    Move best = root[0];

    for (int d = 1; d <= maxDepth; d++) {
        int alpha     = -1000000;
        int bestScore = -1000000;
        Move depthBest = best;
        bool completed = true;

        for (int i = 0; i < n; i++) {
            Move m = root[i];
            Undo u;
            applyMove(m, u);
            int v = -aiSearch(d - 1, -1000000, -alpha, 1);
            revertMove(m, u);

            if (_abort) {
                completed = false;
                break;
            }
            if (v > bestScore) {
                bestScore = v;
                depthBest = m;
            }
            if (v > alpha) alpha = v;
        }

        if (completed) best = depthBest;
        if (!completed || bestScore > 800000) break;
    }

    _best_move = best;
}

// ============================================================
// 绘制
// ============================================================

void AppShogi::drawBoard()
{
    if (!_canvas) return;

    _canvas->fillScreen(TFT_BLACK);
    drawPiecesAndGrid();
    drawHandsAndStatus();

    if (_prompt) {
        drawPromotePrompt();
    } else if (_checked && !_game_over) {
        drawCheckMark();
    }

    if (_game_over && !_win_anim_active) {
        int bx = _board_x + 8;
        int by = _board_y + _board_w / 2 - 16;
        int bw = _board_w - 16;
        _canvas->fillRect(bx, by, bw, 32, 0x0000);
        _canvas->drawRect(bx, by, bw, 32, TFT_YELLOW);

        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextDatum(middle_center);
        _canvas->setTextColor(_winner == 1 ? TFT_GREEN : TFT_RED);
        _canvas->drawString(_winner == 1 ? "你赢了" : "AI 获胜", _board_x + _board_w / 2, by + 10);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->drawString("R 重开", _board_x + _board_w / 2, by + 23);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextDatum(top_left);
    }

    // Win -> level-up transition animation
    if (_win_anim_active) {
        uint32_t el = GetHAL().millis() - _win_anim_start;
        int bx = _board_x + 6;
        int by = _board_y + _board_w / 2 - 16;
        int bw = _board_w - 12;
        _canvas->fillRect(bx, by, bw, 32, 0x0000);
        _canvas->drawRect(bx, by, bw, 32, TFT_YELLOW);

        int cxm = bx + bw / 2;
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextDatum(middle_center);

        if (el < 600) {
            _canvas->setTextColor(TFT_GREEN);
            _canvas->drawString("你赢了！", cxm, by + 11);
            _canvas->setFont(&fonts::efontCN_10);
            _canvas->setTextColor(TFT_WHITE);
            _canvas->drawString("WIN", cxm, by + 24);
        } else if (el < 1200) {
            _canvas->setTextColor(TFT_YELLOW);
            _canvas->drawString("难度提升", cxm, by + 11);
            _canvas->setFont(&fonts::efontCN_10);
            _canvas->setTextColor(TFT_YELLOW);
            char stars[16];
            int n = (_level < 5) ? _level + 1 : 5;
            for (int i = 0; i < n && i < 15; i++) stars[i] = '*';
            stars[n < 15 ? n : 15] = '\0';
            _canvas->drawString(stars, cxm, by + 24);
        } else {
            _canvas->setTextColor(TFT_CYAN);
            char buf[48];
            snprintf(buf, sizeof(buf), "第 %d 星 开新局", (_level < 5) ? _level + 1 : 5);
            _canvas->drawString(buf, cxm, by + 13);
        }

        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextDatum(top_left);
    }

    /* 非法操作提示: 棋盘 99 高、画布 121, 棋盘下方正好留一条空白。
     * 画在帮助层之前, 打开帮助时不会残留。 */
    if (_msg[0] && (int32_t)(_msg_until - GetHAL().millis()) > 0) {
        int ty = _board_y + _board_w + 2;
        if (ty > _canvas->height() - 11) ty = _canvas->height() - 11;
        _canvas->setFont(&fonts::efontCN_10);
        int tw = _canvas->textWidth(_msg);
        int tx = _board_x + (_board_w - tw) / 2;
        /* 提示条只占 110..120, 不压最后一行的棋子墨迹(墨高约到 109) */
        _canvas->fillRect(tx - 3, ty, tw + 6, 11, TFT_BLACK);
        _canvas->drawRect(tx - 3, ty, tw + 6, 11, TFT_YELLOW);
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setCursor(tx, ty + 1);
        _canvas->print(_msg);
    }

    if (_help) {
        /* 帮助是模态层: 棋盘从 y≈2 就开画, 只遮 14 以下会漏出顶行残影, 先整屏铺黑 */
        _canvas->fillScreen(TFT_BLACK);
        int bx = 6, by = 14, bw = _canvas->width() - 12, bh = 78;
        _canvas->fillRect(bx, by, bw, bh, 0x0000);
        _canvas->drawRect(bx, by, bw, bh, TFT_CYAN);
        _canvas->drawRect(bx + 1, by + 1, bw - 2, bh - 2, 0x3186);
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setTextDatum(top_left);
        _canvas->setTextColor(TFT_CYAN);
        _canvas->drawString("将棋 · 操作帮助", bx + 6, by + 4);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->drawString("E/S/A/D 移动  Enter 选/落", bx + 6, by + 18);
        _canvas->drawString("Z 打入 U 悔棋 R 重开", bx + 6, by + 32);
        _canvas->drawString("L 难度 M 双人/人机 H 关闭", bx + 6, by + 46);
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->drawString("成子可选; 步香桂到底必成", bx + 6, by + 60);
    }

    appCanvasPush(_canvas);
    sendMirrorState();
}

void AppShogi::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[110];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x30; p[3] = 0;
    memcpy(&p[4], _board, 81);
    memcpy(&p[85], _hand, 16);
    p[101] = _turn;
    p[102] = (uint8_t)_cursor_r;
    p[103] = (uint8_t)_cursor_c;
    p[104] = (uint8_t)_sel_r;
    p[105] = (uint8_t)_sel_c;
    p[106] = (uint8_t)(_game_over ? 1 : 0);
    p[107] = (uint8_t)_winner;
    p[108] = _move_count & 0xFF;
    p[109] = (_move_count >> 8) & 0xFF;
    glink_send_state(p, sizeof(p));
}

void AppShogi::drawPiecesAndGrid()
{
    // 棋盘底色
    _canvas->fillRect(_board_x, _board_y, _board_w, _board_w, 0xE71C);
    _canvas->drawRect(_board_x - 1, _board_y - 1, _board_w + 2, _board_w + 2, 0x0000);

    for (int i = 0; i <= 9; i++) {
        _canvas->drawFastVLine(_board_x + i * _cell, _board_y, _board_w, 0x0000);
        _canvas->drawFastHLine(_board_x, _board_y + i * _cell, _board_w, 0x0000);
    }

    // 星位
    static const int stars[4][2] = {{3, 3}, {3, 6}, {6, 3}, {6, 6}};
    for (int i = 0; i < 4; i++) {
        _canvas->fillCircle(_board_x + stars[i][1] * _cell, _board_y + stars[i][0] * _cell, 1, 0x0000);
    }

    // 上一步高亮
    if (_last_move.to >= 0) {
        int tr = _last_move.to / 9;
        int tc = _last_move.to % 9;
        _canvas->drawRect(_board_x + tc * _cell + 1, _board_y + tr * _cell + 1, _cell - 2, _cell - 2, TFT_YELLOW);
        if (_last_move.from >= 0) {
            int fr = _last_move.from / 9;
            int fc = _last_move.from % 9;
            _canvas->drawRect(_board_x + fc * _cell + 1, _board_y + fr * _cell + 1, _cell - 2, _cell - 2, TFT_YELLOW);
        }
    }

    // 选中棋子的可走点
    if (_sel_r >= 0) {
        Move moves[MAX_MOVES];
        int n = 0;
        genLegalMoves(moves, n, MAX_MOVES);
        int selIdx = _sel_r * 9 + _sel_c;
        for (int i = 0; i < n; i++) {
            if (moves[i].drop || moves[i].promote) continue;
            if (moves[i].from != (int8_t)selIdx) continue;
            int tr = moves[i].to / 9;
            int tc = moves[i].to % 9;
            /* 深绿点落在棕色棋盘上几乎看不见; 加一圈黑边提高对比 */
            _canvas->fillCircle(_board_x + tc * _cell + _cell / 2,
                                _board_y + tr * _cell + _cell / 2, 3, TFT_BLACK);
            _canvas->fillCircle(_board_x + tc * _cell + _cell / 2,
                                _board_y + tr * _cell + _cell / 2, 2, TFT_GREEN);
        }
    }

    // 棋子
    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextDatum(middle_center);

    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            uint8_t v = _board[r * 9 + c];
            if (!v) continue;
            int owner = ownerOf(v);
            int kind  = kindOf(v);
            int cx = _board_x + c * _cell + _cell / 2;
            int cy = _board_y + r * _cell + _cell / 2;

            uint16_t col = (owner == 1) ? TFT_NAVY : TFT_MAROON;
            _canvas->setTextColor(col);
            _canvas->drawString(glyphOf(kind, owner), cx, cy);

            if (promotedOf(v)) {
                _canvas->drawFastHLine(cx - 4, cy + 5, 8, col);
            }
        }
    }

    // 打入幽灵子
    if (_drop_kind > 0) {
        int cx = _board_x + _cursor_c * _cell + _cell / 2;
        int cy = _board_y + _cursor_r * _cell + _cell / 2;
        _canvas->setTextColor(0x7BEF);
        _canvas->drawString(glyphOf(_drop_kind, 1), cx, cy);
    }

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextDatum(top_left);

    // 光标(红 1px) 与选中(青 2px 外框): 原来是红框+蓝框两个 1px 框套在一起,
    // 11px 的格子里几乎分不出"我现在站的"和"我已经选的"。
    // 选中框画在格子外侧, 不再压住棋子字形。
    if (_sel_r >= 0) {
        int sx = _board_x + _sel_c * _cell;
        int sy = _board_y + _sel_r * _cell;
        _canvas->drawRect(sx - 1, sy - 1, _cell + 2, _cell + 2, TFT_CYAN);
        _canvas->drawRect(sx - 2, sy - 2, _cell + 4, _cell + 4, TFT_CYAN);
    }
    _canvas->drawRect(_board_x + _cursor_c * _cell, _board_y + _cursor_r * _cell, _cell, _cell, TFT_RED);
}

void AppShogi::drawHandsAndStatus()
{
    int left_x  = 2;
    int right_x = _board_x + _board_w + 4;
    int h       = _canvas->height();

    _canvas->setTextSize(1);
    _canvas->setTextDatum(top_left);

    // ---- 左侧上方: AI 持驹 ----
    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(_two_player ? TFT_CYAN : TFT_ORANGE);
    _canvas->setCursor(left_x, 2);
    _canvas->print(_two_player ? "2P" : "AI");

    int y = 14;
    _canvas->setFont(&fonts::efontCN_10);
    for (int k = PAWN; k <= ROOK; k++) {
        if (_hand[1][k] == 0) continue;
        char buf[24];
        snprintf(buf, sizeof(buf), "%s%d", glyphOf(k, 2), (int)_hand[1][k]);
        _canvas->setTextColor(TFT_ORANGE);
        _canvas->setCursor(left_x, y);
        _canvas->print(buf);
        y += 11;
        if (y > h / 2) break;
    }

    // ---- 左侧下方: 玩家持驹 ----
    int py = h - 24;
    bool any_hand = false;
    for (int k = ROOK; k >= PAWN; k--) {
        if (_hand[0][k] == 0) continue;
        any_hand = true;
        char buf[24];
        snprintf(buf, sizeof(buf), "%s%d", glyphOf(k, 1), (int)_hand[0][k]);
        _canvas->setTextColor(0x9FFF);
        _canvas->setCursor(left_x, py);
        _canvas->print(buf);
        py -= 11;
        if (py < h / 2) break;
    }

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(0x9FFF);
    _canvas->setCursor(left_x, h - 12);
    _canvas->print(_two_player ? "P2" : "YOU");

    /* 左下角那一列 "兵1 / 香1" 以前没有任何标题, 第一次玩根本不知道是吃到的棋子 */
    if (any_hand) {
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(left_x, py);
        _canvas->print("持驹");
        if (py >= 10) {
            _canvas->setCursor(left_x, py - 10);
            _canvas->print("Z 打入");
        }
    }

    // ---- 右侧状态 ----
    int ry = 2;
    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(right_x, ry);
    _canvas->print("将棋");
    _canvas->setFont(&fonts::efontCN_10);
    ry += 12;

    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(right_x, ry);
    _canvas->printf("Lv.%d", _level);
    ry += 10;

    // Difficulty stars (1 star per level)
    _canvas->setTextColor(TFT_YELLOW);
    _canvas->setCursor(right_x, ry);
    for (int i = 0; i < _level; i++) _canvas->print("*");
    ry += 10;

    if (_game_over) {
        _canvas->setTextColor(_winner == 1 ? TFT_GREEN : TFT_RED);
        _canvas->print(_winner == 1 ? "YOU WIN" : "AI WIN");
        ry += 10;
    } else if (_turn == 2) {
        _canvas->setTextColor(TFT_RED);
        _canvas->print(_two_player ? "P2回合" : (_ai_thinking ? "AI思考" : "AI回合"));
        ry += 10;
    } else {
        _canvas->setTextColor(TFT_GREEN);
        _canvas->print(_two_player ? "P1回合" : "轮到你");
        ry += 10;
    }

    if (_checked && !_game_over) {
        _canvas->setTextColor(TFT_RED);
        _canvas->setCursor(right_x, ry);
        _canvas->print("CHECK");
        ry += 10;
    }

    if (_drop_kind > 0) {
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setCursor(right_x, ry);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->printf("打%s", glyphOf(_drop_kind, 1));
        _canvas->setFont(&fonts::efontCN_10);
        ry += 10;
    }

    /* 主界面必须写清"怎么走子": 原来只有 [Z]Drp/[U]Undo 这类英文缩写,
     * 第一次玩的人既不知道方向键是 E/S/A/D, 也不知道 Enter 是选子/落子。 */
    _canvas->setTextColor(TFT_LIGHTGREY);
    /* 行距 11px: 10px 字实际墨高 9~10 行, 原来 9px 行距上下两行会粘在一起 */
    _canvas->setCursor(right_x, ry); _canvas->print("E/S/A/D 移"); ry += 11;
    _canvas->setCursor(right_x, ry); _canvas->print("Enter 选/落"); ry += 11;
    /* 10px 字体里 '[' 会渲染得像 'C', 实机显示成 "CZ]打"; 去掉方括号 */
    _canvas->setCursor(right_x, ry); _canvas->print("Z 打 U 悔"); ry += 11;
    _canvas->setCursor(right_x, ry); _canvas->print("R 新 M 双"); ry += 11;
    _canvas->setCursor(right_x, ry); _canvas->print("H 帮助");
}

void AppShogi::drawPromotePrompt()
{
    int bw = _board_w - 12;
    int bx = _board_x + 6;
    int by = _board_y + _board_w / 2 - 15;

    _canvas->fillRect(bx, by, bw, 30, 0x0000);
    _canvas->drawRect(bx, by, bw, 30, TFT_YELLOW);

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextDatum(middle_center);
    _canvas->setTextColor(TFT_YELLOW);
    /* 只说"是否成子"玩家不知道是哪颗子、变成什么; 补上 兵 成 金? 这类说明 */
    int kind = 0;
    if (_pending.from >= 0 && _pending.from < 81) kind = kindOf(_board[(int)_pending.from]);
    const char* to_name = "金";
    if (kind == BISHOP) to_name = "马";
    else if (kind == ROOK) to_name = "龙";
    char pbuf[24];
    if (kind > 0) snprintf(pbuf, sizeof(pbuf), "%s 成 %s?", glyphOf(kind, 1), to_name);
    else snprintf(pbuf, sizeof(pbuf), "是否成子?");
    _canvas->drawString(pbuf, _board_x + _board_w / 2, by + 10);
    _canvas->setTextColor(TFT_WHITE);
    _canvas->drawString("Y 成子 / N 不变", _board_x + _board_w / 2, by + 22);
    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextDatum(top_left);
}

void AppShogi::drawCheckMark()
{
    _canvas->drawRect(_board_x - 2, _board_y - 2, _board_w + 4, _board_w + 4, TFT_RED);
    _canvas->drawRect(_board_x - 3, _board_y - 3, _board_w + 6, _board_w + 6, TFT_RED);
}

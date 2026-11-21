#include "app_checkers.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>
#include <string.h>
#include <stdlib.h>

AppCheckers::AppCheckers() {
    setAppInfo().name = "Checkers";
    setAppInfo().icon = nullptr;
}

AppCheckers::~AppCheckers() { _canvas = nullptr; }

// ==CKCORE_BEGIN==
// ================= 规则核(纯函数, 宿主可测) =================
// 棋盘: 8x8, 仅 (r+c) 为奇数的黑格可下; 红方(1)在下方 r=5..7, 往 r=0 走;
//       白方(2)在上方 r=0..2, 往 r=7 走. 兵只向前, 王四向; 吃子强制, 可连跳,
//       兵跳到底线立即升王且本回合结束(美式规则).
#define CK_EMPTY 0
#define CK_RM 1
#define CK_RK 2
#define CK_WM 3
#define CK_WK 4

static int ck_side_of(uint8_t v) { return (v == CK_RM || v == CK_RK) ? 1 : ((v == CK_WM || v == CK_WK) ? 2 : 0); }
static int ck_is_king(uint8_t v) { return v == CK_RK || v == CK_WK; }
static int ck_playable(int r, int c) { return (unsigned)r < 8 && (unsigned)c < 8 && ((r + c) & 1); }

// 该棋子的可走方向(兵仅向前)
static int ck_dirs(uint8_t v, int8_t d[4][2])
{
    if (v == CK_RM) { d[0][0] = -1; d[0][1] = -1; d[1][0] = -1; d[1][1] = 1; return 2; }
    if (v == CK_WM) { d[0][0] = 1; d[0][1] = -1; d[1][0] = 1; d[1][1] = 1; return 2; }
    if (v == CK_RK || v == CK_WK) {
        d[0][0] = -1; d[0][1] = -1; d[1][0] = -1; d[1][1] = 1;
        d[2][0] = 1; d[2][1] = -1; d[3][0] = 1; d[3][1] = 1;
        return 4;
    }
    return 0;
}

// (r,c) 的子当前是否有吃
static int ck_can_capture_at(const uint8_t* b, int r, int c)
{
    uint8_t v = b[r * 8 + c];
    if (!v) return 0;
    int me = ck_side_of(v);
    int8_t d[4][2];
    int nd = ck_dirs(v, d);
    for (int i = 0; i < nd; i++) {
        int r1 = r + d[i][0], c1 = c + d[i][1];
        int r2 = r + 2 * d[i][0], c2 = c + 2 * d[i][1];
        if (!ck_playable(r2, c2)) continue;
        uint8_t mv = b[r1 * 8 + c1];
        if (mv && ck_side_of(mv) != me && b[r2 * 8 + c2] == 0) return 1;
    }
    return 0;
}

static int ck_side_capture_exists(const uint8_t* b, int side)
{
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (b[r * 8 + c] && ck_side_of(b[r * 8 + c]) == side && ck_can_capture_at(b, r, c)) return 1;
    return 0;
}

// 到底升王
static uint8_t ck_maybe_king(uint8_t v, int r)
{
    if (v == CK_RM && r == 0) return CK_RK;
    if (v == CK_WM && r == 7) return CK_WK;
    return v;
}

// from 格的合法落点(已含强制吃子/连跳锁), 返回个数, outs 最多 4 个
static int ck_legal_dests(const uint8_t* b, int side, int from, int locked_sq, uint8_t* outs)
{
    int n = 0;
    int r = from / 8, c = from % 8;
    uint8_t v = b[from];
    if (!v || ck_side_of(v) != side) return 0;
    if (locked_sq >= 0 && from != locked_sq) return 0;
    int must_cap = (locked_sq >= 0) ? 1 : ck_side_capture_exists(b, side);
    int8_t d[4][2];
    int nd = ck_dirs(v, d);
    for (int i = 0; i < nd; i++) {
        int r1 = r + d[i][0], c1 = c + d[i][1];
        int r2 = r + 2 * d[i][0], c2 = c + 2 * d[i][1];
        if (ck_playable(r2, c2) && b[r2 * 8 + c2] == 0) {
            uint8_t mv = b[r1 * 8 + c1];
            if (mv && ck_side_of(mv) != side) outs[n++] = (uint8_t)(r2 * 8 + c2);
        }
        if (!must_cap && ck_playable(r1, c1) && b[r1 * 8 + c1] == 0)
            outs[n++] = (uint8_t)(r1 * 8 + c1);
    }
    return n;
}

// 执行一步(普通1格或吃子2格). 返回: bit0=吃了子 bit1=升王 bit2=该子还能继续吃
static int ck_apply_step(uint8_t* b, int from, int to)
{
    int r = from / 8, c = from % 8, r2 = to / 8, c2 = to % 8;
    uint8_t v = b[from];
    int ret = 0;
    b[from] = 0;
    if (abs(r2 - r) == 2) {
        int mr = (r + r2) / 2, mc = (c + c2) / 2;
        b[mr * 8 + mc] = 0;
        ret |= 1;
    }
    uint8_t nv = ck_maybe_king(v, r2);
    if (nv != v) ret |= 2;
    b[to] = nv;
    if ((ret & 1) && !(ret & 2) && ck_can_capture_at(b, r2, c2)) ret |= 4;
    return ret;
}

// side 方是否还有任何合法着法(无着=输)
static int ck_has_any_move(const uint8_t* b, int side)
{
    int must = ck_side_capture_exists(b, side);
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t v = b[r * 8 + c];
            if (!v || ck_side_of(v) != side) continue;
            int8_t d[4][2];
            int nd = ck_dirs(v, d);
            for (int i = 0; i < nd; i++) {
                int r1 = r + d[i][0], c1 = c + d[i][1];
                int r2 = r + 2 * d[i][0], c2 = c + 2 * d[i][1];
                if (ck_playable(r2, c2) && b[r2 * 8 + c2] == 0) {
                    uint8_t mv = b[r1 * 8 + c1];
                    if (mv && ck_side_of(mv) != side) return 1;
                }
                if (!must && ck_playable(r1, c1) && b[r1 * 8 + c1] == 0) return 1;
            }
        }
    }
    return 0;
}

static void ck_count(const uint8_t* b, int* n1, int* n2)
{
    int a = 0, c = 0;
    for (int i = 0; i < 64; i++) {
        int s = ck_side_of(b[i]);
        if (s == 1) a++;
        else if (s == 2) c++;
    }
    *n1 = a; *n2 = c;
}

static void ck_init(uint8_t* b)
{
    memset(b, 0, 64);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 8; c++)
            if ((r + c) & 1) b[r * 8 + c] = CK_WM;
    for (int r = 5; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if ((r + c) & 1) b[r * 8 + c] = CK_RM;
}

// ---- AI: 完整着法生成(含多跳链) + 极小化极大 ----
typedef struct { uint8_t from; uint8_t n; uint8_t caps; uint8_t path[9]; } ck_move_t;

// 递归生成吃子链; 命中升王即收尾该链
static void ck_gen_caps_rec(uint8_t* b, int r, int c, uint8_t v, ck_move_t* mv, ck_move_t* out, int* nout, int max)
{
    int me = ck_side_of(v);
    int8_t d[4][2];
    int nd = ck_dirs(v, d);
    int extended = 0;
    for (int i = 0; i < nd && mv->n < 9; i++) {
        int r1 = r + d[i][0], c1 = c + d[i][1];
        int r2 = r + 2 * d[i][0], c2 = c + 2 * d[i][1];
        if (!ck_playable(r2, c2) || b[r2 * 8 + c2] != 0) continue;
        uint8_t mid = b[r1 * 8 + c1];
        if (!mid || ck_side_of(mid) == me) continue;
        uint8_t nv = ck_maybe_king(v, r2);
        b[r * 8 + c] = 0; b[r1 * 8 + c1] = 0; b[r2 * 8 + c2] = nv;
        mv->path[mv->n++] = (uint8_t)(r2 * 8 + c2);
        mv->caps++;
        extended = 1;
        if (nv != v) {
            if (*nout < max) { out[*nout] = *mv; (*nout)++; }  // 升王即停
        } else {
            ck_gen_caps_rec(b, r2, c2, nv, mv, out, nout, max);
        }
        mv->n--; mv->caps--;
        b[r * 8 + c] = v; b[r1 * 8 + c1] = mid; b[r2 * 8 + c2] = 0;
    }
    if (!extended && mv->caps > 0 && *nout < max) { out[*nout] = *mv; (*nout)++; }
}

// 生成 side 全部合法着法(有吃只吃; 否则全部普通步), 返回着法数
static int ck_gen_moves(const uint8_t* bin, int side, ck_move_t* out, int max)
{
    int n = 0;
    uint8_t b[64];
    memcpy(b, bin, 64);
    if (ck_side_capture_exists(b, side)) {
        for (int r = 0; r < 8 && n < max; r++) {
            for (int c = 0; c < 8 && n < max; c++) {
                uint8_t v = b[r * 8 + c];
                if (!v || ck_side_of(v) != side) continue;
                if (!ck_can_capture_at(b, r, c)) continue;
                ck_move_t mv;
                memset(&mv, 0, sizeof(mv));
                mv.from = (uint8_t)(r * 8 + c);
                ck_gen_caps_rec(b, r, c, v, &mv, out, &n, max);
            }
        }
        return n;
    }
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t v = b[r * 8 + c];
            if (!v || ck_side_of(v) != side) continue;
            int8_t d[4][2];
            int nd = ck_dirs(v, d);
            for (int i = 0; i < nd && n < max; i++) {
                int r1 = r + d[i][0], c1 = c + d[i][1];
                if (ck_playable(r1, c1) && b[r1 * 8 + c1] == 0) {
                    out[n].from = (uint8_t)(r * 8 + c);
                    out[n].n = 1;
                    out[n].caps = 0;
                    out[n].path[0] = (uint8_t)(r1 * 8 + c1);
                    n++;
                }
            }
        }
    }
    return n;
}

// 把整步(含多跳链)应用到局面
static void ck_apply_move(uint8_t* b, const ck_move_t* mv)
{
    int cur = mv->from;
    for (int i = 0; i < mv->n; i++) {
        ck_apply_step(b, cur, mv->path[i]);
        cur = mv->path[i];
    }
}

// 局面评估(me 方视角)
static int ck_eval(const uint8_t* b, int me)
{
    int s = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t v = b[r * 8 + c];
            if (!v) continue;
            int side = ck_side_of(v);
            int val;
            if (v == CK_RM || v == CK_WM) {
                val = 100;
                int adv = (v == CK_RM) ? (7 - r) : r;  // 推进奖励
                val += adv * 6;
                if ((v == CK_RM && r == 7) || (v == CK_WM && r == 0)) val += 8;  // 守底线
                if (adv >= 5) val += 8;                                          // 接近升王
            } else {
                val = 185;
            }
            if (r >= 2 && r <= 5 && c >= 2 && c <= 5) val += 6;  // 中央控制
            s += (side == me) ? val : -val;
        }
    }
    return s;
}

#define CK_INF 2000000000
#define CK_MAX_PLY 10
// 静态池: 避免每层递归在栈上放 64B 棋盘 + 大着法表(ESP32 任务栈有限)
static ck_move_t s_ck_mvpool[CK_MAX_PLY][48];
static uint8_t s_ck_bpool[CK_MAX_PLY][64];

static int ck_minimax(const uint8_t* b, int depth, int alpha, int beta, int cur, int me, int ply)
{
    ck_move_t* mvs = s_ck_mvpool[ply];
    int n = ck_gen_moves(b, cur, mvs, 48);
    if (n == 0) return (cur == me) ? (-1000000 + ply) : (1000000 - ply);
    if (depth == 0) return ck_eval(b, me);
    if (cur == me) {
        int best = -CK_INF;
        for (int i = 0; i < n; i++) {
            uint8_t* nb = s_ck_bpool[ply];
            memcpy(nb, b, 64);
            ck_apply_move(nb, &mvs[i]);
            int s = ck_minimax(nb, depth - 1, alpha, beta, 3 - cur, me, ply + 1);
            if (s > best) best = s;
            if (s > alpha) alpha = s;
            if (beta <= alpha) break;
        }
        return best;
    } else {
        int best = CK_INF;
        for (int i = 0; i < n; i++) {
            uint8_t* nb = s_ck_bpool[ply];
            memcpy(nb, b, 64);
            ck_apply_move(nb, &mvs[i]);
            int s = ck_minimax(nb, depth - 1, alpha, beta, 3 - cur, me, ply + 1);
            if (s < best) best = s;
            if (s < beta) beta = s;
            if (beta <= alpha) break;
        }
        return best;
    }
}

static uint32_t ck_rng_s = 0x1A2B3C4Du;
static uint32_t ck_rand(void) { ck_rng_s = ck_rng_s * 1664525u + 1013904223u; return ck_rng_s >> 8; }
static void ck_srand(uint32_t s) { ck_rng_s = s ? s : 1u; }

// 选最佳着法, 同分随机; 无着返回 -1, 否则写入 best 返回 0
static int ck_ai_move(const uint8_t* b, int me, int depth, ck_move_t* best)
{
    ck_move_t mvs[48];
    int n = ck_gen_moves(b, me, mvs, 48);
    if (n == 0) return -1;
    int bs = -CK_INF, nb = 0, bi = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b2[64];
        memcpy(b2, b, 64);
        ck_apply_move(b2, &mvs[i]);
        int s = ck_minimax(b2, depth - 1, -CK_INF, CK_INF, 3 - me, me, 1);
        if (s > bs) { bs = s; bi = i; nb = 1; }
        else if (s == bs && (ck_rand() % ++nb) == 0) bi = i;
    }
    *best = mvs[bi];
    return 0;
}
// ==CKCORE_END==

void AppCheckers::resetGame() {
    ck_init(_b);
    _cur = 1;
    _cursor = 51;
    _sel = -1;
    _locked = -1;
    _ndests = 0;
    _over = false;
    _winner = 0;
    _quiet = 0;
    _moves = 0;
    _last_from = _last_to = -1;
    _hint_from = _hint_to = -1;
    _dirty = true;
}

void AppCheckers::refreshDests() {
    _ndests = 0;
    if (_sel < 0) return;
    _ndests = ck_legal_dests(_b, _cur, _sel, _locked, _dests);
}

void AppCheckers::pressConfirm() {
    if (_over || !(_mode == 1 || (_mode == 2 && _started))) return;
    // 只在该方回合操作(键盘=红, 手柄=白; 调用方已保证轮到本端)
    if (_sel < 0) {
        // 尝试选中: 必须有合法落点
        int n = ck_legal_dests(_b, _cur, _cursor, _locked, _dests);
        if (n > 0) {
            _sel = _cursor;
            _ndests = n;
            audio::play_random_tone();
        }
        _dirty = true;
        return;
    }
    if (_cursor == _sel) {
        if (_locked < 0) { _sel = -1; _ndests = 0; _dirty = true; }  // 非连跳锁可取消
        return;
    }
    // 光标在合法落点上 → 走一步
    bool ok = false;
    for (int i = 0; i < _ndests; i++)
        if (_dests[i] == _cursor) { ok = true; break; }
    if (!ok) return;

    int from = _sel;
    int ret = ck_apply_step(_b, from, _cursor);
    _last_from = from;
    _last_to = _cursor;
    audio::play_random_tone();
    if (ret & 1) _quiet = 0;  // 有吃子, 清零和棋计数(连跳时不会走到 else)
    else _quiet++;
    if (ret & 4) {
        // 连跳: 锁定该子, 只能继续吃
        _locked = _cursor;
        _sel = _cursor;
        refreshDests();
    } else {
        _sel = -1;
        _locked = -1;
        _ndests = 0;
        endTurn();
    }
    _dirty = true;
}

void AppCheckers::endTurn() {
    _moves++;
    _cur = 3 - _cur;
    if (!ck_has_any_move(_b, _cur)) {
        _over = true;
        _winner = 3 - _cur;  // 无着方输
        _score[_winner - 1]++;
        if (_mode == 2) { _started = false; _ready_local = _ready_remote = false; }
    } else if (_quiet >= 100) {
        _over = true;
        _winner = 3;
        if (_mode == 2) { _started = false; _ready_local = _ready_remote = false; }
    }
    if (_mode == 1 && _cur == 2 && !_over) _ai_timer = GetHAL().millis() + 450;
    updateHint();
    _dirty = true;
}

void AppCheckers::aiMove() {
    ck_move_t mv;
    if (ck_ai_move(_b, 2, 7, &mv) != 0) return;
    _last_from = mv.from;
    _last_to = mv.path[mv.n - 1];
    if (mv.caps > 0) _quiet = 0;
    else _quiet++;
    ck_apply_move(_b, &mv);
    endTurn();
}

void AppCheckers::updateHint() {
    _hint_from = _hint_to = -1;
    if (_over || _mode == 0) return;
    if (_mode == 2 && !_started) return;
    ck_move_t mv;
    if (ck_ai_move(_b, _cur, 6, &mv) == 0) {
        _hint_from = mv.from;
        _hint_to = mv.path[mv.n - 1];
    }
}

// ================= 联机(手柄方=白) =================
void AppCheckers::handleNet(uint32_t now) {
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
        // 四方向光标: 按下走一格, 长按连走
        uint16_t dirs = (uint16_t)(m & 0x000F);
        int dx = 0, dy = 0;
        if (pressed & 0x0001) dy = -1;
        else if (pressed & 0x0002) dy = 1;
        else if (pressed & 0x0004) dx = -1;
        else if (pressed & 0x0008) dx = 1;
        if (dx || dy) {
            _hold_dir = dirs; _hold_ms = now; _rep_ms = now;
        } else if (dirs && dirs == _hold_dir && now - _hold_ms > 420 && now - _rep_ms > 120) {
            _rep_ms = now;
            if (dirs & 0x0001) dy = -1;
            else if (dirs & 0x0002) dy = 1;
            else if (dirs & 0x0004) dx = -1;
            else dx = 1;
        }
        if (!dirs) _hold_dir = 0;
        if (dx || dy) {
            int r = _cursor / 8 + dy, c = _cursor % 8 + dx;
            if (r < 0) r = 0;
            if (r > 7) r = 7;
            if (c < 0) c = 0;
            if (c > 7) c = 7;
            int ns = r * 8 + c;
            if (ns != _cursor) { _cursor = ns; _dirty = true; }
        }
        if (pressed & 0x0010) pressConfirm();          // A = 选子/落子
        if ((pressed & 0x0020) && _locked < 0) {       // B = 取消选择
            _sel = -1; _ndests = 0; _dirty = true;
        }
    }

    // 双方都准备 → 开局
    if ((!_started || _over) && _ready_local && _ready_remote) {
        resetGame();
        _started = true;
        _dirty = true;
    }
}

void AppCheckers::onOpen() {
    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    glink_acquire("Checkers", GLINK_GID_CHECKERS, GLINK_ICON_CHECKERS);

    _mode = 0;
    _menu_sel = 0;
    _started = false;
    _ready_local = _ready_remote = false;
    _score[0] = _score[1] = 0;
    _net_btn_prev = glink_buttons();
    ck_srand(esp_random());
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
        int r = _cursor / 8, c = _cursor % 8;
        if (e.keyCode == KEY_LEFT || e.keyCode == KEY_A) {
            if (c > 0) { _cursor--; _dirty = true; }
        } else if (e.keyCode == KEY_RIGHT || e.keyCode == KEY_D) {
            if (c < 7) { _cursor++; _dirty = true; }
        } else if (e.keyCode == KEY_UP || e.keyCode == KEY_E || e.keyCode == KEY_W) {
            if (r > 0) { _cursor -= 8; _dirty = true; }
        } else if (e.keyCode == KEY_DOWN || e.keyCode == KEY_S) {
            if (r < 7) { _cursor += 8; _dirty = true; }
        } else if (e.keyCode == KEY_ENTER || e.keyCode == KEY_SPACE) {
            pressConfirm();
        } else if (e.keyCode == KEY_BACKSPACE) {
            if (_locked < 0) { _sel = -1; _ndests = 0; _dirty = true; }
        }
    });
}

void AppCheckers::onRunning() {
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

void AppCheckers::onClose() {
    if (_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    glink_release();

    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

// ================= 绘制 =================
void AppCheckers::draw() {
    if (!_canvas) return;
    int w = _canvas->width(), h = _canvas->height();
    _canvas->fillScreen(TFT_BLACK);
    _canvas->setFont(&fonts::efontCN_12);

    if (_mode == 0) {
        _canvas->setTextColor(TFT_CYAN);
        int tw = _canvas->textWidth("Checkers 西洋跳棋");
        _canvas->setCursor((w - tw) / 2, 18);
        _canvas->print("Checkers 西洋跳棋");
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

    // 棋盘: 8x14=112, 左上 (2, 10)
    const int CELL = 14;
    const int BW = 8 * CELL;
    const int BX = 2, BY = 10;

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int px = BX + c * CELL, py = BY + r * CELL;
            bool dark = ((r + c) & 1) != 0;
            _canvas->fillRect(px, py, CELL, CELL, dark ? 0x8A22 : 0xE79C);  // 深棕/米白
        }
    }

    // 最后一步: 橙框
    if (_last_from >= 0) {
        _canvas->drawRect(BX + (_last_from % 8) * CELL, BY + (_last_from / 8) * CELL, CELL, CELL, TFT_ORANGE);
        _canvas->drawRect(BX + (_last_to % 8) * CELL, BY + (_last_to / 8) * CELL, CELL, CELL, TFT_ORANGE);
    }

    // 合法落点: 绿点
    if (_sel >= 0) {
        for (int i = 0; i < _ndests; i++) {
            int dr = _dests[i] / 8, dc = _dests[i] % 8;
            _canvas->fillCircle(BX + dc * CELL + CELL / 2, BY + dr * CELL + CELL / 2, 2, TFT_GREEN);
        }
    }

    // 棋子
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t v = _b[r * 8 + c];
            if (!v) continue;
            int px = BX + c * CELL + CELL / 2, py = BY + r * CELL + CELL / 2;
            uint16_t body = (v == CK_RM || v == CK_RK) ? TFT_RED : TFT_WHITE;
            _canvas->fillCircle(px, py, 5, body);
            _canvas->drawCircle(px, py, 5, TFT_BLACK);
            if (ck_is_king(v)) {
                _canvas->drawCircle(px, py, 3, (v == CK_RK) ? TFT_YELLOW : TFT_DARKGREY);
                _canvas->drawCircle(px, py, 2, (v == CK_RK) ? TFT_YELLOW : TFT_DARKGREY);
            }
        }
    }

    // 选中框: 黄(连跳锁=品红)
    if (_sel >= 0) {
        uint16_t sc = (_locked >= 0) ? TFT_MAGENTA : TFT_YELLOW;
        _canvas->drawRect(BX + (_sel % 8) * CELL, BY + (_sel / 8) * CELL, CELL, CELL, sc);
        _canvas->drawRect(BX + (_sel % 8) * CELL + 1, BY + (_sel / 8) * CELL + 1, CELL - 2, CELL - 2, sc);
    }
    // 光标: 青框
    if (!_over) {
        _canvas->drawRect(BX + (_cursor % 8) * CELL, BY + (_cursor / 8) * CELL, CELL, CELL, TFT_CYAN);
    }

    // ---------- 右侧信息 ----------
    int px = BX + BW + 6;
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(px, 2);
    _canvas->print("西洋跳棋");
    _canvas->setTextColor(_mode == 2 ? TFT_MAGENTA : TFT_DARKGREY);
    _canvas->setCursor(px, 16);
    _canvas->print(_mode == 2 ? "双人联机" : "单人 vs AI");

    int n1, n2;
    ck_count(_b, &n1, &n2);
    _canvas->setCursor(px, 32);
    _canvas->setTextColor(TFT_RED);
    _canvas->printf("红 %d子 %d胜", n1, _score[0]);
    _canvas->setCursor(px, 46);
    _canvas->setTextColor(TFT_WHITE);
    _canvas->printf("白 %d子 %d胜", n2, _score[1]);

    // 强制吃子/连跳提示
    if (!_over && (_mode == 1 || (_mode == 2 && _started))) {
        _canvas->setCursor(px, 62);
        if (_locked >= 0) {
            _canvas->setTextColor(TFT_MAGENTA);
            _canvas->print("连跳!");
        } else if (ck_side_capture_exists(_b, _cur)) {
            _canvas->setTextColor(TFT_YELLOW);
            _canvas->print("必须吃子");
        }
    }

    if (_mode == 2 && !_started) {
        _canvas->setCursor(px, 80);
        _canvas->setTextColor(_ready_local ? TFT_GREEN : TFT_YELLOW);
        _canvas->print(_ready_local ? "我 OK" : "Enter备");
        _canvas->setCursor(px, 94);
        _canvas->setTextColor(_ready_remote ? TFT_GREEN : TFT_YELLOW);
        if (glink_remote_flags() & 0x01) _canvas->print(_ready_remote ? "对方 OK" : "对方 --");
        else { _canvas->setTextColor(TFT_RED); _canvas->print("等手柄.."); }
    } else if (_over) {
        _canvas->setCursor(px, 80);
        _canvas->setTextColor(TFT_YELLOW);
        const char* wt = (_winner == 3) ? "和棋!" : ((_winner == 1) ? "红方胜!" : "白方胜!");
        _canvas->print(wt);
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(px, 94);
        _canvas->print(_mode == 2 ? "双方备重开" : "Enter重开");
    } else {
        _canvas->setCursor(px, 80);
        _canvas->setTextColor(_cur == 1 ? TFT_RED : TFT_WHITE);
        _canvas->print(_cur == 1 ? "红方走" : "白方走");
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(px, 94);
        _canvas->print("ESAD移 Ent选");
    }

    appCanvasPush(_canvas);
}

// CKSTATE: 'G','M',0x26,seq | 4-15 棋盘32黑格x3bit | 16 cursor | 17 sel(255无)
//          18 locked(255无) | 19 flags | 20 cur | 21 winner | 22 lastFrom(255无) | 23 lastTo
//          24 score0 | 25 score1 | 26 hintFrom(255无) | 27 hintTo | 28 quiet | 29 moves
//          30-37 合法落点掩码64bit(选中时)
// flags: bit0联机 bit1开局 bit2结束 bit3本地备 bit4远端备 bit5单人 bit6有选中 bit7连跳锁
void AppCheckers::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[38];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x26;
    // 32 个黑格 x 3bit, 逐位写入
    int pi = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (!((r + c) & 1)) continue;
            int v = _b[r * 8 + c];
            int off = pi * 3;
            for (int k = 0; k < 3; k++)
                if ((v >> k) & 1) p[4 + (off + k) / 8] |= (uint8_t)(1 << ((off + k) % 8));
            pi++;
        }
    }
    p[16] = (uint8_t)_cursor;
    p[17] = (uint8_t)(_sel < 0 ? 255 : _sel);
    p[18] = (uint8_t)(_locked < 0 ? 255 : _locked);
    p[19] = (_mode == 2 ? 0x01 : 0)
          | ((_mode != 0 && (_started || _mode == 1)) ? 0x02 : 0)
          | (_over ? 0x04 : 0)
          | (_ready_local ? 0x08 : 0)
          | (_ready_remote ? 0x10 : 0)
          | (_mode == 1 ? 0x20 : 0)
          | (_sel >= 0 ? 0x40 : 0)
          | (_locked >= 0 ? 0x80 : 0);
    p[20] = (uint8_t)_cur;
    p[21] = (uint8_t)_winner;
    p[22] = (uint8_t)(_last_from < 0 ? 255 : _last_from);
    p[23] = (uint8_t)(_last_to < 0 ? 255 : _last_to);
    p[24] = (uint8_t)_score[0];
    p[25] = (uint8_t)_score[1];
    p[26] = (uint8_t)(_hint_from < 0 ? 255 : _hint_from);
    p[27] = (uint8_t)(_hint_to < 0 ? 255 : _hint_to);
    p[28] = (uint8_t)(_quiet > 255 ? 255 : _quiet);
    p[29] = (uint8_t)_moves;
    // 合法落点掩码
    if (_sel >= 0) {
        for (int i = 0; i < _ndests; i++)
            p[30 + _dests[i] / 8] |= (uint8_t)(1 << (_dests[i] % 8));
    }
    glink_send_state(p, sizeof(p));
}

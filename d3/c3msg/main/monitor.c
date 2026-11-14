/*
 * monitor.c — 五子棋监视器（GMK 镜像）界面
 *
 * 端口自 zd/gomoku_c3/main/main.c：
 *   GMKBEAC(13B) / GMKHI1(6B) / GMKWELC(7B) / GMKAPP(0x10,24B) / GMKSTATE(0x20,240B)
 * 画面完全由本机按状态包重绘，不依赖主机视频流；布局与 gomoku_c3 实机一致
 * （棋盘 CELL=12、BSX=70、BSY=30；320x240 横屏）。
 *
 * 注意：本模块只做解析与绘制，ESP-NOW 的初始化/peer 注册仍在 main.c，
 * 收包由 main.c 的 on_recv 转进来，发送通过 monitor_init 注入的回调。
 */
#include "monitor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#define MW 320
#define MH 240

#define CELL 12
#define BPD  (CELL * 15)      /* 180 */
#define BSX  70
#define BSY  30
#define LX   2
#define RX   252
#define RW   66

/* flags0 */
#define F0_OVER   1
#define F0_TAUNT  4
#define F0_THREAT 16
#define F0_REPLAY 32
/* flags1 */
#define F1_PVP          1
#define F1_ASSIST       2
#define F1_BLITZ        4
#define F1_TAUNT_REMOTE 128

#define GID_GOMOKU 1
#define EV_N 5

typedef struct {
    uint8_t flags0, flags1, winner, cur;
    uint8_t difficulty;
    uint8_t cursor_x, cursor_y, move_count, last_r, last_c;
    uint8_t pw, aw, streak, best;
    uint8_t blunders, min_danger;
    uint8_t clk1, clk2;
    uint16_t game_sec;
    uint8_t taunt_kind;
    char taunt[3][18];
    uint8_t board[15][15];
    uint8_t threat[15][15];
} gmk_t;

static gmk_t s_g;
static volatile int      s_have;
static volatile uint32_t s_last_pkt;
static volatile int      s_rssi = -120;

static uint8_t s_host[6];
static int     s_host_valid;
static uint8_t s_gid = GID_GOMOKU;
static char    s_app[17] = "Gomoku";
static uint32_t s_last_hello;
static mon_send_fn s_send;

static char s_ev[EV_N][24];
static int  s_ev_cnt, s_ev_head;

static volatile int s_n_beac, s_n_state, s_n_app;

static uint32_t now_ms_u(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void push_ev(const char *s)
{
    snprintf(s_ev[s_ev_head], sizeof(s_ev[0]), "%s", s);
    s_ev_head = (s_ev_head + 1) % EV_N;
    if (s_ev_cnt < EV_N) s_ev_cnt++;
}

/* i = 0 最旧 */
static const char *ev_at(int i)
{
    int start = (s_ev_head - s_ev_cnt + EV_N) % EV_N;
    return s_ev[(start + i) % EV_N];
}

/* ==================== GMK 帧解析 ==================== */
static void parse_state(const uint8_t *d)
{
    uint8_t prev_mc = s_g.move_count;

    s_g.flags0 = d[4];  s_g.flags1 = d[5];
    s_g.winner = d[6];  s_g.cur = d[7];
    s_g.difficulty = d[8];
    s_g.cursor_x = d[11]; s_g.cursor_y = d[12];
    s_g.move_count = d[13];
    s_g.last_r = d[14];  s_g.last_c = d[15];
    s_g.pw = d[18]; s_g.aw = d[19];
    s_g.streak = d[20]; s_g.best = d[21];
    s_g.blunders = d[24];
    s_g.min_danger = d[25];
    s_g.clk1 = d[31]; s_g.clk2 = d[32];
    s_g.game_sec = (uint16_t)(d[33] | (d[34] << 8));
    s_g.taunt_kind = d[69];
    memcpy(s_g.taunt, d + 70, 54);
    s_g.taunt[0][17] = s_g.taunt[1][17] = s_g.taunt[2][17] = 0;

    for (int i = 0; i < 57; i++) {              /* 每字节 4 格 x 2bit */
        uint8_t v = d[124 + i];
        for (int k = 0; k < 4; k++) {
            int idx = i * 4 + k;
            if (idx >= 225) break;
            s_g.threat[idx / 15][idx % 15] = (uint8_t)((v >> (k * 2)) & 3);
        }
    }
    for (int i = 0; i < 57; i++) {
        uint8_t v = d[181 + i];
        for (int k = 0; k < 4; k++) {
            int idx = i * 4 + k;
            if (idx >= 225) break;
            s_g.board[idx / 15][idx % 15] = (uint8_t)((v >> (k * 2)) & 3);
        }
    }

    if (s_g.move_count != prev_mc && s_g.move_count > 0 &&
        s_g.last_r < 15 && s_g.last_c < 15) {
        int who = s_g.board[s_g.last_r][s_g.last_c];
        char b[24];
        snprintf(b, sizeof(b), "%s %c%d", who == 2 ? "白方" : "黑方",
                 'A' + s_g.last_c, s_g.last_r + 1);
        push_ev(b);
    }

    s_have = 1;
}

void monitor_init(mon_send_fn send)
{
    s_send = send;
    memset(&s_g, 0, sizeof(s_g));
    s_g.cursor_x = s_g.cursor_y = 0;
    s_have = 0;
    s_last_pkt = 0;
    s_ev_cnt = s_ev_head = 0;
}

void monitor_on_frame(const uint8_t src_mac[6], const uint8_t *d, int len, int rssi)
{
    if (!d || len < 6 || d[0] != 'G' || d[1] != 'M') return;

    uint32_t now = now_ms_u();
    uint32_t prev_pkt = s_last_pkt;
    s_last_pkt = now;
    s_rssi = rssi;

    if (len == 13 && memcmp(d, "GMKBEAC", 7) == 0) {
        /* 主机每次开机/心跳都会广播：回 GMKHI1 让它认为监视器在线。
         * 注意主机 MAC 在负载 d+7（与 gomoku_c3 一致），不是 ESP-NOW 源地址。 */
        s_n_beac++;
        memcpy(s_host, d + 7, 6);
        s_host_valid = 1;
        if (!prev_pkt || now - prev_pkt > 1500) s_have = 0;  /* 主机重启：旧画面作废 */
        if (s_last_hello == 0 || now - s_last_hello > 800) {
            s_last_hello = now;
            static const uint8_t hi[6] = {'G', 'M', 'K', 'H', 'I', '1'};
            if (s_send) s_send(s_host, hi, 6);
        }
        return;
    }
    if (len == 7 && memcmp(d, "GMKWELC", 7) == 0) {
        memcpy(s_host, src_mac, 6);
        s_host_valid = 1;
        return;
    }
    if (len == 6 && memcmp(d, "GMKBYE", 6) == 0) {
        s_have = 0;
        return;
    }
    if (len == 24 && d[2] == 0x10) {               /* GMKAPP：前台 app 切换 */
        uint8_t gid = d[4];
        s_n_app++;
        if (gid != s_gid) {
            s_gid = gid;
            s_have = 0;
            memset(&s_g, 0, sizeof(s_g));
            s_ev_cnt = s_ev_head = 0;
        }
        memcpy(s_app, d + 6, 16);
        s_app[16] = 0;
        return;
    }
    if (len == 240 && d[2] == 0x20) {              /* GMKSTATE 全量状态 */
        s_n_state++;
        parse_state(d);
        return;
    }
}

/* ==================== 绘图原语 ==================== */
static void fill(int x, int y, int w, int h, color_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > MW) w = MW - x;
    if (y + h > MH) h = MH - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) display_put(x + i, y + j, c);
}

static void hl(int x, int y, int n, color_t c) { fill(x, y, n, 1, c); }
static void vl(int x, int y, int n, color_t c) { fill(x, y, 1, n, c); }

static void rect(int x, int y, int w, int h, color_t c)
{
    hl(x, y, w, c);
    hl(x, y + h - 1, w, c);
    vl(x, y, h, c);
    vl(x + w - 1, y, h, c);
}

static void disc(int cx, int cy, int r, color_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int half = (int)sqrtf((float)(r * r - dy * dy));
        hl(cx - half, cy + dy, 2 * half + 1, c);
    }
}

static void ring(int cx, int cy, int r, color_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        display_put(cx + x, cy + y, c); display_put(cx - x, cy + y, c);
        display_put(cx + x, cy - y, c); display_put(cx - x, cy - y, c);
        display_put(cx + y, cy + x, c); display_put(cx - y, cy + x, c);
        display_put(cx + y, cy - x, c); display_put(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void text_cx(int x0, int w, int y, const char *s, color_t c)
{
    int tw = ui_text_w(s);
    int x = x0 + (w - tw) / 2;
    if (x < x0) x = x0;
    ui_text(x, y, s, c, w);
}

/* ==================== 棋盘 ==================== */
static void draw_board(void)
{
    fill(BSX, BSY, BPD, BPD, C_BOARD);
    for (int i = 0; i < 15; i++) {
        hl(BSX + CELL / 2, BSY + i * CELL + CELL / 2, BPD - CELL, C_STONE_B);
        vl(BSX + i * CELL + CELL / 2, BSY + CELL / 2, BPD - CELL, C_STONE_B);
    }

    for (int r = 0; r < 15; r++) {
        for (int c = 0; c < 15; c++) {
            int x = BSX + c * CELL + CELL / 2;
            int y = BSY + r * CELL + CELL / 2;
            if (s_g.board[r][c] == 1) {
                disc(x, y, CELL / 2 - 1, C_STONE_B);
            } else if (s_g.board[r][c] == 2) {
                disc(x, y, CELL / 2 - 1, C_STONE_W);
                ring(x, y, CELL / 2 - 1, C_STONE_B);
            }
        }
    }

    /* 威胁标记：红=成五 橙=活四 绿框=可成五 */
    if ((s_g.flags0 & F0_THREAT) && !(s_g.flags0 & F0_REPLAY)) {
        for (int r = 0; r < 15; r++) {
            for (int c = 0; c < 15; c++) {
                if (s_g.board[r][c] || s_g.threat[r][c] == 0) continue;
                int x = BSX + c * CELL + CELL / 2;
                int y = BSY + r * CELL + CELL / 2;
                if (s_g.threat[r][c] == 3) fill(x - 3, y - 3, 7, 7, C_ERR);
                else if (s_g.threat[r][c] == 2) fill(x - 3, y - 3, 6, 6, C_ORANGE);
                else rect(x - 3, y - 3, 7, 7, C_IN);
            }
        }
    }

    /* 最后一手 */
    if (s_g.last_r < 15 && s_g.last_c < 15)
        rect(BSX + s_g.last_c * CELL, BSY + s_g.last_r * CELL, CELL, CELL, C_CYAN);

    /* 光标（回放时不画） */
    if (!(s_g.flags0 & F0_REPLAY) && s_g.cursor_x < 15 && s_g.cursor_y < 15)
        rect(BSX + s_g.cursor_x * CELL, BSY + s_g.cursor_y * CELL, CELL, CELL, C_ERR);
}

/* ==================== 左栏：对局状态 ==================== */
static void draw_left(void)
{
    char b[40];
    ui_text(LX, 2, "五子棋", C_CYAN, 64);

    int pvp = (s_g.flags1 & F1_PVP) != 0;

    if (pvp) {
        ui_text(LX, 20, "PVP", C_MAGENTA, 64);
        if (s_g.flags0 & F0_OVER) {
            ui_text(LX, 36, s_g.winner == 0 ? "平局" : (s_g.winner == 1 ? "黑胜" : "白胜"),
                    C_FG, 64);
        } else {
            ui_text(LX, 36, s_g.cur == 1 ? "黑方" : "白方", C_FG, 64);
        }
        ui_text(LX, 56, s_g.cur == 1 ? "轮黑下" : "轮白下", C_DIM, 64);
    } else {
        snprintf(b, sizeof(b), "Lv.%d", s_g.difficulty);
        ui_text(LX, 20, b, C_FG, 64);

        for (int row = 0; row < 2; row++) {
            char st[6];
            for (int i = 0; i < 5; i++) st[i] = (row * 5 + i < s_g.difficulty) ? '*' : ' ';
            st[5] = 0;
            ui_text(LX, 34 + row * 13, st, C_WARN, 64);
        }

        snprintf(b, sizeof(b), "YOU%2d", s_g.pw);
        ui_text(LX, 62, b, C_IN, 64);
        snprintf(b, sizeof(b), "AI %2d", s_g.aw);
        ui_text(LX, 76, b, C_ORANGE, 64);
        {
            int total = s_g.pw + s_g.aw;
            int rate = total > 0 ? s_g.aw * 100 / total : 0;
            snprintf(b, sizeof(b), "WIN%2d%%", rate);
            ui_text(LX, 90, b, C_ERR, 64);
        }
    }

    const char *st;
    if (s_g.flags0 & F0_REPLAY) st = "REPLAY";
    else if (s_g.flags0 & F0_OVER) {
        if (s_g.winner == 0) st = "DRAW";
        else if (s_g.flags1 & F1_ASSIST) st = s_g.winner == 2 ? "AI WIN" : "OPP WIN";
        else st = s_g.winner == 1 ? "YOU WIN" : "AI WIN";
    } else {
        if (s_g.flags1 & F1_ASSIST) st = s_g.cur == 1 ? "INPUT P1" : "THINK..";
        else st = s_g.cur == 1 ? "YOURTURN" : "THINK..";
    }
    ui_text(LX, 106, st, C_FG, 64);
    ui_text(LX, 120, (s_g.flags1 & F1_ASSIST) ? "Assist" : "Normal", C_DIM, 64);

    snprintf(b, sizeof(b), "MV %d", s_g.move_count);
    ui_text(LX, 138, b, C_DIM, 64);

    if (s_g.flags1 & F1_BLITZ) {
        int cs = (s_g.flags0 & F0_OVER) ? 0 : (s_g.cur == 1 ? s_g.clk1 : s_g.clk2);
        snprintf(b, sizeof(b), "CLK %d", cs);
        ui_text(LX, 152, b, cs < 10 ? C_ERR : C_CYAN, 64);
    } else {
        snprintf(b, sizeof(b), "T %d:%02d", s_g.game_sec / 60, s_g.game_sec % 60);
        ui_text(LX, 152, b, C_CYAN, 64);
    }

    if (!pvp) {
        snprintf(b, sizeof(b), "误%d 险%d", s_g.blunders,
                 s_g.min_danger == 255 ? 0 : s_g.min_danger);
        ui_text(LX, 170, b, C_DIM, 64);
        snprintf(b, sizeof(b), "胜%d 最%d", s_g.streak, s_g.best);
        ui_text(LX, 184, b, s_g.streak > 0 ? C_IN : C_DIM, 64);
    }
}

/* ==================== 右栏：链路 + 记录 ==================== */
static void draw_right(uint32_t now)
{
    char b[40];
    int alive = s_last_pkt && (now - s_last_pkt) < 3000;

    ui_text(RX, 2, s_app, C_DIM, RW);
    ui_text(RX, 18, alive ? "LINK" : "WAIT", alive ? C_IN : C_ERR, RW);
    snprintf(b, sizeof(b), "RSSI%d", s_rssi);
    ui_text(RX, 32, b, C_DIM, RW);

    if (s_g.last_r < 15 && s_g.last_c < 15) {
        snprintf(b, sizeof(b), "LAST %c%d", 'A' + s_g.last_c, s_g.last_r + 1);
        ui_text(RX, 48, b, C_CYAN, RW);
    }

    ui_text(RX, 66, "记录", C_DIM, RW);
    for (int i = 0; i < 4 && i < s_ev_cnt; i++)
        ui_text(RX, 80 + i * 14, ev_at(s_ev_cnt - 1 - i), C_FG, RW);

    if ((s_g.flags0 & F0_TAUNT) && !(s_g.flags1 & F1_TAUNT_REMOTE)) {
        int nl = s_g.taunt_kind == 3 ? 3 : 2;
        for (int i = 0; i < nl; i++) {
            if (!s_g.taunt[i][0]) break;
            text_cx(RX, RW, 150 + i * 15, s_g.taunt[i], C_WARN);
        }
    }
}

static void draw_bottom(void)
{
    const char *hint = "长按 BOOT 返回";
    fill(0, MH - 18, MW, 18, C_BAR);
    ui_text(4, MH - 16, "GMK 500ms", C_DIM, -1);
    ui_text(MW - 4 - ui_text_w(hint), MH - 16, hint, C_DIM, -1);
}

/* ==================== 无数据页 ==================== */
static void draw_wait(void)
{
    char b[48];
    uint32_t now = now_ms_u();
    int alive = s_last_pkt && (now - s_last_pkt) < 3000;

    fill(0, 0, MW, MH, C_BG);
    ui_text(8, 8, "五子棋 MIRROR", C_CYAN, -1);
    ui_text(8, 34, s_app, C_FG, -1);
    snprintf(b, sizeof(b), "gid %u", (unsigned)s_gid);
    ui_text(8, 52, b, C_DIM, -1);

    ui_text(8, 92, alive ? "已连接，等待画面数据..." : "等待主机 GMKBEAC...",
            alive ? C_IN : C_ERR, -1);
    ui_text(8, 116, "主机需运行 Cardputer 游戏端", C_DIM, -1);
    ui_text(8, 134, "收到 GMKSTATE 后自动镜像", C_DIM, -1);
    if (s_host_valid) {
        snprintf(b, sizeof(b), "host %02X:%02X:%02X:%02X:%02X:%02X",
                 s_host[0], s_host[1], s_host[2], s_host[3], s_host[4], s_host[5]);
        ui_text(8, 160, b, C_DIM, -1);
    }
    ui_text(8, MH - 30, "长按 BOOT 返回消息页", C_DIM, -1);
}

static void draw_app_page(void)
{
    char b[48];
    fill(0, 0, MW, MH, C_BG);
    ui_text(8, 8, "MIRROR", C_CYAN, -1);
    ui_text(8, 40, s_app, C_FG, -1);
    snprintf(b, sizeof(b), "gid %u", (unsigned)s_gid);
    ui_text(8, 58, b, C_DIM, -1);
    ui_text(8, 100, "主机当前不在五子棋", C_FG, -1);
    ui_text(8, 120, "本机只镜像五子棋画面", C_DIM, -1);
    ui_text(8, MH - 30, "长按 BOOT 返回消息页", C_DIM, -1);
}

/* ==================== 入口 ==================== */
int monitor_supported(void)
{
    return SCR_W >= 300 && SCR_H >= 200;
}

void monitor_get_stats(int *beac, int *state, int *app, int *gid, char *name, int cap)
{
    if (beac) *beac = s_n_beac;
    if (state) *state = s_n_state;
    if (app) *app = s_n_app;
    if (gid) *gid = s_gid;
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", s_app);
}

void monitor_draw(void)
{
    if (!monitor_supported()) {
        fill(0, 0, SCR_W, SCR_H, C_BG);
        ui_text(4, 4, "MON 需大屏", C_DIM, -1);
        return;
    }

    fill(0, 0, MW, MH, C_BG);

    if (s_gid != GID_GOMOKU) { draw_app_page(); return; }
    if (!s_have) { draw_wait(); return; }

    draw_board();
    draw_left();
    draw_right(now_ms_u());
    draw_bottom();
}

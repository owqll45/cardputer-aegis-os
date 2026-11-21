#include "app_maze.h"
#include <mooncake_log.h>
#include <apps/utils/app_canvas.h>
#include <apps/utils/common.h>
#include <esp_random.h>
#include <string.h>

// 墙位: bit0 上, bit1 右, bit2 下, bit3 左
static const int DX[4] = {0, 1, 0, -1};
static const int DY[4] = {-1, 0, 1, 0};
static const uint8_t OPP[4] = {2, 3, 0, 1};

AppMaze::AppMaze()
{
    setAppInfo().name = "Maze";
    setAppInfo().icon = nullptr;
}

AppMaze::~AppMaze()
{
    _canvas = nullptr;
}

void AppMaze::generate()
{
    memset(_walls, 0x0F, sizeof(_walls));  // 先给所有格子立满四面墙
    memset(_seen, 0, sizeof(_seen));

    // 迭代 DFS 挖通道(避免递归爆栈)
    uint16_t stack[MW * MH];
    int sp = 0;
    bool visited[MW * MH];
    memset(visited, 0, sizeof(visited));

    int start = 0;
    visited[start] = true;
    stack[sp++] = (uint16_t)start;

    while (sp > 0) {
        int cur = stack[sp - 1];
        int cx = cur % MW, cy = cur / MW;

        int cand[4], nc = 0;
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || nx >= MW || ny < 0 || ny >= MH) continue;
            int ni = ny * MW + nx;
            if (visited[ni]) continue;
            cand[nc++] = d;
        }
        if (nc == 0) { sp--; continue; }

        int d = cand[esp_random() % nc];
        int nx = cx + DX[d], ny = cy + DY[d];
        int ni = ny * MW + nx;
        // 打通两边的墙
        _walls[cur] &= (uint8_t)~(1 << d);
        _walls[ni] &= (uint8_t)~(1 << OPP[d]);
        visited[ni] = true;
        stack[sp++] = (uint16_t)ni;
    }

    _px = 0;
    _py = 0;
    _steps = 0;
    _won = false;
    _start_ms = GetHAL().millis();
    _seen[0] = 1;
    mclog::tagInfo(getAppInfo().name, "generate level=%d", _level);
}

void AppMaze::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");

    GetHAL().display().fillScreen(TFT_BLACK);
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    _level = 1;
    generate();

    _keyq.clear();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        _keyq.push(e);
    });

    draw();
    appCanvasPush(_canvas);
}

void AppMaze::move(int dx, int dy)
{
    if (_won) return;
    int d = -1;
    if (dx == 1) d = 1;
    else if (dx == -1) d = 3;
    else if (dy == 1) d = 2;
    else if (dy == -1) d = 0;
    if (d < 0) return;

    int cur = _py * MW + _px;
    if (_walls[cur] & (1 << d)) return;  // 撞墙

    int nx = _px + dx, ny = _py + dy;
    if (nx < 0 || nx >= MW || ny < 0 || ny >= MH) return;
    _px = nx;
    _py = ny;
    _steps++;
    _seen[_py * MW + _px] = 1;

    if (_px == MW - 1 && _py == MH - 1) {
        _won = true;
        _win_at = GetHAL().millis();
        mclog::tagInfo(getAppInfo().name, "level=%d clear steps=%d", _level, _steps);
    }
}

void AppMaze::handleKey(const Keyboard::KeyEvent_t& e)
{
    if (e.isModifier || !e.state) return;
    if (e.keyCode == KEY_ESC) { close(); return; }

    if (e.keyCode == KEY_R) {
        generate();
        return;
    }
    if (_won) {
        if (e.keyCode == KEY_ENTER || e.keyCode == KEY_N) {
            _level++;
            generate();
        }
        return;
    }

    switch (e.keyCode) {
        case KEY_LEFT: case KEY_A: move(-1, 0); break;
        case KEY_RIGHT: case KEY_D: move(1, 0); break;
        case KEY_UP: case KEY_E: move(0, -1); break;
        case KEY_DOWN: case KEY_S: move(0, 1); break;
        default: break;
    }
}

void AppMaze::draw()
{
    if (!_canvas) return;
    auto* c = _canvas;
    int w = c->width();
    int h = c->height();
    c->fillScreen(TFT_BLACK);
    c->setTextDatum(top_left);

    uint32_t secs = (GetHAL().millis() - _start_ms) / 1000;

    c->setFont(&fonts::efontCN_10);
    c->setTextColor(TFT_CYAN);
    c->setCursor(4, 1);
    c->printf("迷宫 第%d关", _level);
    c->setTextColor(TFT_YELLOW);
    c->setCursor(96, 1);
    c->printf("步数%d", _steps);
    c->setTextColor(TFT_DARKGREY);
    c->setCursor(w - 52, 1);
    c->printf("%lus", (unsigned long)secs);

    // 迷宫网格: 19x8, 单元格 10x10 (给底部提示留出空间)
    const int cs = 10;
    int ox = (w - MW * cs) / 2;
    int oy = 13;
    uint16_t wall_col = 0x8C71;

    // 先画所有墙
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            int idx = y * MW + x;
            int px = ox + x * cs, py = oy + y * cs;
            if (_walls[idx] & 1) c->drawFastHLine(px, py, cs + 1, wall_col);
            if (_walls[idx] & 2) c->drawFastVLine(px + cs, py, cs + 1, wall_col);
            if (_walls[idx] & 4) c->drawFastHLine(px, py + cs, cs + 1, wall_col);
            if (_walls[idx] & 8) c->drawFastVLine(px, py, cs + 1, wall_col);
        }
    }

    // 走过的路(淡青点)
    for (int y = 0; y < MH; y++) {
        for (int x = 0; x < MW; x++) {
            if (_seen[y * MW + x] && !(x == _px && y == _py))
                c->fillRect(ox + x * cs + 4, oy + y * cs + 4, 4, 4, 0x0861);
        }
    }

    // 终点
    c->fillRect(ox + (MW - 1) * cs + 2, oy + (MH - 1) * cs + 2, cs - 4, cs - 4, TFT_RED);

    // 玩家
    c->fillRect(ox + _px * cs + 2, oy + _py * cs + 2, cs - 4, cs - 4, TFT_GREEN);

    c->setFont(&fonts::efontCN_10);
    c->setTextColor(TFT_DARKGREY);
    c->setCursor(4, h - 10);
    if (_won) c->print("到达终点! Enter=下一关  R=重开  Esc=退出");
    else c->print("方向键/ESAD 移动  R=重开  Esc=退出");

    if (_won) {
        c->fillRect(50, 42, w - 100, 30, TFT_BLACK);
        c->drawRect(50, 42, w - 100, 30, TFT_GREEN);
        c->setFont(&fonts::efontCN_12);
        c->setTextColor(TFT_GREEN);
        c->setTextDatum(middle_center);
        char buf[40];
        snprintf(buf, sizeof(buf), "过关 %d 步", _steps);
        c->drawString(buf, w / 2, 57);
        c->setTextDatum(top_left);
    }
}

void AppMaze::onRunning()
{
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    Keyboard::KeyEvent_t e;
    while (_keyq.pop(e)) {
        handleKey(e);
        if (!_canvas) return;
    }

    // 通关 1.2s 后自动进下一关, 也可以按 Enter 立即进
    if (_won && GetHAL().millis() - _win_at > 1200) {
        _level++;
        generate();
    }

    draw();
    appCanvasPush(_canvas);
}

void AppMaze::onClose()
{
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
    _keyq.clear();
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

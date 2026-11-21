#include "app_whack.h"
#include <mooncake_log.h>
#include <apps/utils/app_canvas.h>
#include <apps/utils/common.h>
#include <esp_random.h>

AppWhack::AppWhack()
{
    setAppInfo().name = "Whack";
    setAppInfo().icon = nullptr;
}

AppWhack::~AppWhack()
{
    _canvas = nullptr;
}

void AppWhack::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");

    GetHAL().display().fillScreen(TFT_BLACK);
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    _score = 0;
    _hit = 0;
    _miss = 0;
    _combo = 0;
    _best_combo = 0;
    _over = false;
    _mole = -1;
    _cursor = 4;
    _start_ms = GetHAL().millis();

    _keyq.clear();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        _keyq.push(e);
    });

    draw();
    appCanvasPush(_canvas);
}

void AppWhack::handleKey(const Keyboard::KeyEvent_t& e)
{
    if (e.isModifier || !e.state) return;
    if (e.keyCode == KEY_ESC) { close(); return; }

    if (_over) {
        if (e.keyCode == KEY_ENTER || e.keyCode == KEY_R) {
            _over = false;
            _score = _hit = _miss = _combo = _best_combo = 0;
            _mole = -1;
            _start_ms = GetHAL().millis();
        }
        return;
    }

    // 方向: 方向键 / ESA D 双别名 (ADV 无独立方向键, Ctrl+;.,/ 合成箭头仍兼容)
    if (e.keyCode == KEY_LEFT || e.keyCode == KEY_A) { if (_cursor % 3 > 0) _cursor--; return; }
    if (e.keyCode == KEY_RIGHT || e.keyCode == KEY_D) { if (_cursor % 3 < 2) _cursor++; return; }
    if (e.keyCode == KEY_UP || e.keyCode == KEY_E) { if (_cursor >= 3) _cursor -= 3; return; }
    if (e.keyCode == KEY_DOWN || e.keyCode == KEY_S) { if (_cursor < 6) _cursor += 3; return; }
    if (e.keyCode == KEY_ENTER) { hitCell(_cursor, GetHAL().millis()); return; }

    // 1..9 直接打对应地洞
    if (e.keyName && e.keyName[0] >= '1' && e.keyName[0] <= '9' && !e.keyName[1]) {
        hitCell(e.keyName[0] - '1', GetHAL().millis());
    }
}

void AppWhack::newMole(uint32_t now)
{
    int idx = _mole;
    while (idx == _mole) idx = (int)(esp_random() % N);
    _mole = idx;
    // 分数越高缩得越快, 但保底 420ms
    int life = 1000 - _score * 12;
    if (life < 420) life = 420;
    _mole_until = now + life;
}

void AppWhack::hitCell(int idx, uint32_t now)
{
    if (_mole < 0) return;
    if (idx != _mole) {
        // 打空: 断连击并记一次失误
        _combo = 0;
        _miss++;
        return;
    }
    _hit++;
    _combo++;
    if (_combo > _best_combo) _best_combo = _combo;
    _score += 1 + (_combo >= 5 ? 1 : 0);  // 5 连以上双倍
    _mole = -1;
    newMole(now);
}

void AppWhack::draw()
{
    if (!_canvas) return;
    auto* c = _canvas;
    int w = c->width();
    int h = c->height();
    c->fillScreen(TFT_BLACK);
    c->setTextDatum(top_left);

    uint32_t now = GetHAL().millis();
    int left = _over ? 0 : (int)((GAME_MS - (now - _start_ms)) / 1000);
    if (left < 0) left = 0;

    // 顶部: 时间 / 分数 / 连击
    c->setFont(&fonts::efontCN_10);
    c->setTextColor(left <= 5 ? TFT_RED : TFT_CYAN);
    c->setCursor(4, 2);
    c->printf("时间 %2ds", left);
    c->setTextColor(TFT_YELLOW);
    c->setCursor(80, 2);
    c->printf("得分 %d", _score);
    c->setTextColor(TFT_GREEN);
    c->setCursor(160, 2);
    c->printf("连 %d", _combo);

    // 时间条
    int bw = (int)((uint32_t)(w - 8) * (left * 1000 + (1000 - (now - _start_ms) % 1000)) / GAME_MS);
    if (_over) bw = 0;
    if (bw > w - 8) bw = w - 8;
    c->drawRect(4, 14, w - 8, 3, 0x4208);
    if (bw > 0) c->fillRect(4, 14, bw, 3, left <= 5 ? TFT_RED : TFT_CYAN);

    // 3x3 地洞
    const int cw = 42, chh = 24, gapx = 4, gapy = 2;
    int gw = 3 * cw + 2 * gapx;
    int x0 = (w - gw) / 2;
    int y0 = 20;
    for (int i = 0; i < N; i++) {
        int r = i / 3, cc = i % 3;
        int x = x0 + cc * (cw + gapx);
        int y = y0 + r * (chh + gapy);
        // 洞
        c->fillRect(x, y, cw, chh, 0x18E3);
        c->drawRect(x, y, cw, chh, 0x4208);
        if (i == _cursor && !_over) c->drawRect(x - 1, y - 1, cw + 2, chh + 2, TFT_DARKGREY);
        if (i == _mole && !_over) {
            // 地鼠
            uint16_t body = TFT_ORANGE;
            c->fillRect(x + 10, y + 5, cw - 20, chh - 10, body);
            c->drawRect(x + 10, y + 5, cw - 20, chh - 10, TFT_WHITE);
            c->fillRect(x + 15, y + 10, 3, 3, TFT_BLACK);
            c->fillRect(x + cw - 18, y + 10, 3, 3, TFT_BLACK);
        }
    }

    // 底部提示
    c->setFont(&fonts::efontCN_10);
    c->setTextColor(TFT_DARKGREY);
    c->setCursor(4, h - 11);
    c->printf("1-9/ESAD+Enter 打  失误%d", _miss);

    if (_over) {
        c->fillRect(40, 40, w - 80, 38, TFT_BLACK);
        c->drawRect(40, 40, w - 80, 38, TFT_YELLOW);
        c->setFont(&fonts::efontCN_12);
        c->setTextColor(TFT_YELLOW);
        c->setTextDatum(middle_center);
        c->drawString("时间到", w / 2, 52);
        c->setFont(&fonts::efontCN_10);
        c->setTextColor(TFT_WHITE);
        char buf[48];
        snprintf(buf, sizeof(buf), "得分 %d  最高连击 %d", _score, _best_combo);
        c->drawString(buf, w / 2, 68);
        c->setTextDatum(top_left);
    }
}

void AppWhack::onRunning()
{
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    Keyboard::KeyEvent_t e;
    while (_keyq.pop(e)) {
        handleKey(e);
        if (!_canvas) return;
    }

    uint32_t now = GetHAL().millis();

    if (!_over) {
        if (now - _start_ms >= GAME_MS) {
            _over = true;
            _mole = -1;
            mclog::tagInfo(getAppInfo().name, "game over score=%d hit=%d best_combo=%d",
                           _score, _hit, _best_combo);
        } else if (_mole < 0) {
            newMole(now);
        } else if ((int32_t)(now - _mole_until) >= 0) {
            _mole = -1;
            _miss++;
            _combo = 0;
        }
    }

    draw();
    appCanvasPush(_canvas);
}

void AppWhack::onClose()
{
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
    _keyq.clear();
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

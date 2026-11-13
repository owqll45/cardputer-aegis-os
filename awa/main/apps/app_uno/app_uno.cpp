#include "app_uno.h"
#include <mooncake_log.h>
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <esp_random.h>
#include <cstring>

// 牌面值: 0-9 数字, 10=跳过 11=反转 12=罚2, 13=变色, 14=罚4变色
// 牌 id: 0..99 = 4色 x 25张; 100..103=变色; 104..107=罚4变色

AppUno::AppUno() {
    setAppInfo().name = "UNO";
    setAppInfo().icon = nullptr;
}
AppUno::~AppUno() { _canvas = nullptr; }

bool AppUno::canPlay(int id) const {
    if (_pending > 0) {
        // 叠加: 只能用同类型罚牌顶回去
        return cardValue(id) == _pkind;
    }
    if (cardColor(id) == 0) return true;
    return cardColor(id) == _cur_color || cardValue(id) == cardValue(_top);
}

bool AppUno::anyPlayable(int p) const {
    for (int i = 0; i < _hn[p]; i++)
        if (canPlay(_hand[p][i])) return true;
    return false;
}

void AppUno::buildDeck() {
    _deck_n = 0;
    for (int i = 0; i < 108; i++) _deck[i] = (uint8_t)i;
    for (int i = 107; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = _deck[i]; _deck[i] = _deck[j]; _deck[j] = t;
    }
    _deck_n = 108;
}

void AppUno::drawCards(int p, int n) {
    for (int k = 0; k < n; k++) {
        if (_deck_n <= 0) buildDeck();   // 牌堆抽空: 简化处理, 重洗一副
        if (_deck_n <= 0) break;
        if (_hn[p] >= HAND_MAX) break;
        _hand[p][_hn[p]++] = _deck[--_deck_n];
    }
    if (_hn[p] > 1) _uno_ok[p] = false;
    _dirty = true;
}

void AppUno::startRound() {
    buildDeck();
    for (int i = 0; i < NP_MAX; i++) { _hn[i] = 0; _uno_ok[i] = false; _uno_pending[i] = false; }
    for (int i = 0; i < _np; i++) drawCards(i, 7);

    // 翻首牌: 避开变色牌, 保证开局有颜色
    do {
        _top = _deck[--_deck_n];
    } while (_deck_n > 0 && cardColor(_top) == 0);
    _cur_color = cardColor(_top) ? cardColor(_top) : 1;
    _cur = 0;
    _dir = 1;
    _pending = 0;
    _pkind = 0;
    _choosing = -1;
    _sel = 0;
    _over = false;
    _winner = -1;
    _dirty = true;
}

void AppUno::advance(int steps) {
    _cur = ((_cur + _dir * steps) % _np + _np) % _np;
    _sel = 0;
    if (_cur != 0) _dirty = true;
}

void AppUno::applyEffect(int p, int id) {
    int v = cardValue(id);
    int steps = 1;
    if (v == 12) { _pending += 2; _pkind = 12; }
    if (v == 14) { _pending += 4; _pkind = 14; }
    if (v == 10) steps = 2;                       // skip
    if (v == 11) { if (_np == 2) steps = 2; else _dir = -_dir; }

    if (_hn[p] == 0) { endRound(p); return; }
    advance(steps);

    // 新回合方若被罚牌压着, 只能选择顶回去或吃罚(交给输入处理)
    if (_pending > 0 && !anyPlayable(_cur)) doDrawTurn(_cur);
    if (_cur == 1 && _mode == 1) _ai_ms = GetHAL().millis() + 600;
}

void AppUno::confirmColor(int color) {
    if (_choosing < 0) return;
    _cur_color = color;
    int p = _choosing;
    _choosing = -1;
    int id = _top;
    applyEffect(p, id);
    _dirty = true;
}

void AppUno::doPlay(int p, int idx) {
    if (idx < 0 || idx >= _hn[p]) return;
    int id = _hand[p][idx];
    if (!canPlay(id)) { audio::play_tone(200, 0.05); return; }

    for (int i = idx; i < _hn[p] - 1; i++) _hand[p][i] = _hand[p][i + 1];
    _hn[p]--;
    _top = id;
    if (_hn[p] > 1) _uno_ok[p] = false;
    // 出到剩 1 张 → 需要喊 UNO(3 秒宽限)
    if (_hn[p] == 1 && !_uno_ok[p]) {
        if (p == 1 && _mode == 1) {
            _uno_ok[1] = true;   // AI 自动喊
        } else {
            _uno_pending[p] = true;
            _uno_deadline[p] = GetHAL().millis() + 3000;
        }
    }
    audio::play_tone(880, 0.04);

    if (cardColor(id) == 0) {
        // 变色牌: 先选色再结算
        _choosing = p;
        _color_sel = 0;
        if (p == 1 && _mode == 1) {
            // AI: 选手里最多的颜色
            int cnt[5] = { 0 };
            for (int i = 0; i < _hn[1]; i++) cnt[cardColor(_hand[1][i])]++;
            int best = 1;
            for (int c = 2; c <= 4; c++) if (cnt[c] > cnt[best]) best = c;
            confirmColor(best);
        }
        _dirty = true;
        return;
    }
    applyEffect(p, id);
}

void AppUno::doDrawTurn(int p) {
    if (_pending > 0) {
        drawCards(p, _pending);
        _pending = 0;
        _pkind = 0;
        audio::play_tone(300, 0.08);
        advance(1);
    } else {
        drawCards(p, 1);
        audio::play_tone(300, 0.05);
        advance(1);
    }
    if (_cur == 1 && _mode == 1) _ai_ms = GetHAL().millis() + 600;
    _dirty = true;
}

void AppUno::endRound(int p) {
    int pts = 0;
    for (int i = 0; i < _np; i++) {
        if (i == p) continue;
        for (int k = 0; k < _hn[i]; k++) {
            int v = cardValue(_hand[i][k]);
            if (cardColor(_hand[i][k]) == 0) pts += 50;
            else if (v >= 10) pts += 20;
            else pts += v;
        }
    }
    _score[p] += pts;
    _winner = p;
    _over = true;
    if (_score[p] >= TARGET_SCORE) _match_end = true;
    _dirty = true;
}

void AppUno::aiTurn(uint32_t now) {
    if (_mode != 1 || _over || _choosing >= 0) return;
    if (_cur != 1) return;
    if (now < _ai_ms) return;
    _ai_ms = now + 700;

    if (_pending > 0 && !anyPlayable(1)) { doDrawTurn(1); return; }
    // 策略: 优先数字牌跟色 > 功能牌 > 变色牌
    int best = -1, bestScore = -1;
    for (int i = 0; i < _hn[1]; i++) {
        int id = _hand[1][i];
        if (!canPlay(id)) continue;
        int v = cardValue(id);
        int s = (cardColor(id) == 0) ? 1 : (v >= 10 ? 2 : 3);
        if (s > bestScore) { bestScore = s; best = i; }
    }
    if (best >= 0) doPlay(1, best);
    else doDrawTurn(1);
}

void AppUno::updateUno(uint32_t now) {
    if (_over) return;
    for (int p = 0; p < _np; p++) {
        if (!_uno_pending[p]) continue;
        if (_uno_ok[p]) { _uno_pending[p] = false; continue; }
        if ((int32_t)(now - _uno_deadline[p]) >= 0) {
            _uno_pending[p] = false;
            drawCards(p, 2);
            audio::play_tone(200, 0.12);
            _dirty = true;
        }
    }
}

void AppUno::handleButtons(uint32_t now) {
    if (_mode != 2) return;
    bool conn = (glink_remote_flags() & 0x01) != 0;
    _peek_remote = conn && (glink_remote_flags() & 0x02);
    if (!conn) { _btn_prev = 0; return; }

    uint16_t m = glink_buttons();
    uint16_t pressed = (uint16_t)(m & ~_btn_prev);
    _btn_prev = m;

    // 选色阶段: P2 用方向+A 选
    if (_choosing == 1) {
        if (pressed & 0x0004) { _color_sel = (_color_sel + 3) % 4; _dirty = true; }
        if (pressed & 0x0008) { _color_sel = (_color_sel + 1) % 4; _dirty = true; }
        if (pressed & 0x0001) { _color_sel = (_color_sel + 3) % 4; _dirty = true; }
        if (pressed & 0x0002) { _color_sel = (_color_sel + 1) % 4; _dirty = true; }
        if (pressed & 0x0010) confirmColor(_color_sel + 1);
        return;
    }
    if (_cur != 1 || _over) return;

    if (pressed & 0x0004) { if (_sel > 0) { _sel--; _dirty = true; } }
    if (pressed & 0x0008) { if (_sel < _hn[1] - 1) { _sel++; _dirty = true; } }
    if (pressed & 0x0010) {
        if (_sel < _hn[1] && canPlay(_hand[1][_sel])) doPlay(1, _sel);
        else if (!anyPlayable(1)) doDrawTurn(1);
        else audio::play_tone(200, 0.05);
    }
    if (pressed & 0x0020) {  // B = 喊 UNO(手牌≤2 时才有效)
        if (_hn[1] <= 2) _uno_ok[1] = true;
        else audio::play_tone(200, 0.05);
        _dirty = true;
    }
    (void)now;
}

void AppUno::onOpen() {
    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    glink_acquire("UNO", GLINK_GID_UNO, GLINK_ICON_UNO);

    _mode = 0;
    _menu_sel = 0;
    _np = 2;
    _help = false;
    _peek_local = false;
    _peek_remote = false;
    memset(_score, 0, sizeof(_score));
    _round = 1;
    _match_end = false;
    _btn_prev = glink_buttons();
    buildDeck();
    startRound();

    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (e.isModifier) return;

        // H: 帮助开关(打开时屏蔽其它按键)
        if (e.keyCode == KEY_H) {
            if (e.state) {
                _help = !_help;
                if (_help) _peek_local = false;
                _dirty = true;
            }
            return;
        }
        if (_help) return;

        // 偷窥: 按住 P
        if (e.keyCode == KEY_P) {
            _peek_local = e.state;
            _dirty = true;
            return;
        }

        if (_mode == 0) {
            if (!e.state) return;
            if (e.keyCode == KEY_UP || e.keyCode == KEY_DOWN || e.keyCode == KEY_E ||
                e.keyCode == KEY_W || e.keyCode == KEY_S)
                _menu_sel ^= 1;
            else if (e.keyCode == KEY_ENTER) {
                _mode = _menu_sel + 1;
                memset(_score, 0, sizeof(_score));
                _round = 1;
                _match_end = false;
                startRound();
            }
            _dirty = true;
            return;
        }

        if (!e.state) return;

        // 选色阶段: E/S/A/D 或左右键选, Enter 确认
        if (_choosing == 0) {
            if (e.keyCode == KEY_LEFT || e.keyCode == KEY_A) { _color_sel = (_color_sel + 3) % 4; _dirty = true; }
            else if (e.keyCode == KEY_RIGHT || e.keyCode == KEY_D) { _color_sel = (_color_sel + 1) % 4; _dirty = true; }
            else if (e.keyCode == KEY_ENTER) confirmColor(_color_sel + 1);
            return;
        }

        // 局间: Enter 下一局 / 比赛结束 Enter 重开新赛
        if (_over) {
            if (e.keyCode == KEY_ENTER) {
                if (_match_end) {
                    memset(_score, 0, sizeof(_score));
                    _round = 1;
                    _match_end = false;
                } else {
                    _round++;
                }
                startRound();
            }
            return;
        }
        if (_cur != 0) return;   // 键盘方 = P1

        if (e.keyCode == KEY_LEFT || e.keyCode == KEY_A) { if (_sel > 0) { _sel--; _dirty = true; } }
        else if (e.keyCode == KEY_RIGHT || e.keyCode == KEY_D) { if (_sel < _hn[0] - 1) { _sel++; _dirty = true; } }
        else if (e.keyCode == KEY_ENTER) {
            if (_sel < _hn[0] && canPlay(_hand[0][_sel])) doPlay(0, _sel);
            else if (!anyPlayable(0)) doDrawTurn(0);
            else audio::play_tone(200, 0.05);
        } else if (e.keyCode == KEY_U) {
            if (_hn[0] <= 2) _uno_ok[0] = true;
            else audio::play_tone(200, 0.05);
            _dirty = true;
        }
    });
}

void AppUno::onRunning() {
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    glink_poll();
    uint32_t now = GetHAL().millis();

    if (_mode == 2) handleButtons(now);  // 断线只提示, 不中断对局(牌局无实时性)

    updateUno(now);
    if (_mode == 1) aiTurn(now);

    if (_dirty) _dirty = false;
    draw();

    static uint32_t last_mirror = 0;
    if (now - last_mirror >= 100) {
        last_mirror = now;
        sendMirrorState();
    }
}

void AppUno::onClose() {
    if (_handle_key_event_slot_id >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    glink_release();

    _canvas = &GetHAL().canvas();
    _canvas->deleteSprite();
    _canvas->createSprite(GetHAL().display().width(), 109);
    _canvas = nullptr;
}

// ---------------- 绘制 ----------------
void AppUno::drawColorSwatch(int x, int y, int color) {
    uint16_t c = TFT_WHITE;
    switch (color) {
    case 1: c = TFT_RED; break;
    case 2: c = TFT_YELLOW; break;
    case 3: c = TFT_GREEN; break;
    case 4: c = TFT_BLUE; break;
    }
    _canvas->fillRect(x, y, 12, 12, c);
    _canvas->drawRect(x, y, 12, 12, TFT_WHITE);
}

void AppUno::drawCard(int x, int y, int w, int h, int id, bool hidden) {
    if (hidden) {
        _canvas->fillRect(x, y, w, h, TFT_DARKGREY);
        _canvas->drawRect(x, y, w, h, TFT_WHITE);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(x + w / 2 - 3, y + h / 2 - 5);
        _canvas->print("?");
        return;
    }
    int col = cardColor(id), v = cardValue(id);
    uint16_t bg = TFT_WHITE;
    switch (col) {
    case 1: bg = TFT_RED; break;
    case 2: bg = TFT_YELLOW; break;
    case 3: bg = TFT_GREEN; break;
    case 4: bg = TFT_BLUE; break;
    }
    if (col == 0) {
        // 变色牌: 四宫格
        int hw = w / 2, hh = h / 2;
        _canvas->fillRect(x, y, hw, hh, TFT_RED);
        _canvas->fillRect(x + hw, y, w - hw, hh, TFT_YELLOW);
        _canvas->fillRect(x, y + hh, hw, h - hh, TFT_GREEN);
        _canvas->fillRect(x + hw, y + hh, w - hw, h - hh, TFT_BLUE);
    } else {
        _canvas->fillRect(x, y, w, h, bg);
    }
    _canvas->drawRect(x, y, w, h, TFT_BLACK);
    // 中央白底 + 内容
    _canvas->fillRect(x + 2, y + 2, w - 4, h - 4, TFT_WHITE);
    _canvas->setTextColor(bg == TFT_YELLOW ? TFT_BLACK : bg);
    _canvas->setFont(&fonts::efontCN_12);
    char t[4];
    if (v <= 9) snprintf(t, sizeof(t), "%d", v);
    else if (v == 10) snprintf(t, sizeof(t), "S");
    else if (v == 11) snprintf(t, sizeof(t), "R");
    else if (v == 12) snprintf(t, sizeof(t), "+2");
    else if (v == 13) snprintf(t, sizeof(t), "W");
    else snprintf(t, sizeof(t), "+4");
    int tw = _canvas->textWidth(t);
    _canvas->setCursor(x + (w - tw) / 2, y + h / 2 - 6);
    _canvas->print(t);
    if (v == 13 || v == 14) {
        // 变色牌角标: 四个小色点
        _canvas->fillRect(x + 3, y + h - 6, 3, 3, TFT_RED);
        _canvas->fillRect(x + 8, y + h - 6, 3, 3, TFT_YELLOW);
        _canvas->fillRect(x + 13, y + h - 6, 3, 3, TFT_GREEN);
        _canvas->fillRect(x + 18, y + h - 6, 3, 3, TFT_BLUE);
    }
}

void AppUno::draw() {
    if (!_canvas) return;
    int w = _canvas->width(), h = _canvas->height();
    _canvas->fillScreen(TFT_BLACK);
    _canvas->setFont(&fonts::efontCN_12);

    if (_help) {
        drawHelp(w, h);
        _canvas->pushSprite(0, 0);
        return;
    }

    if (_mode == 0) {
        _canvas->setTextColor(TFT_YELLOW);
        int tw = _canvas->textWidth("UNO 纸牌");
        _canvas->setCursor((w - tw) / 2, 14);
        _canvas->print("UNO 纸牌");
        const char* items[2] = { "单人 vs AI", "双人联机" };
        for (int i = 0; i < 2; i++) {
            int iw = _canvas->textWidth(items[i]);
            int ix = (w - iw) / 2, iy = 46 + i * 22;
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
        _canvas->setCursor((w - _canvas->textWidth(hs)) / 2, h - 16);
        _canvas->print(hs);
        // 操作提示: 统一收进 H 帮助, 入口放右下角
        _canvas->setFont(&fonts::efontCN_10_b);
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(190, 119);
        _canvas->print("H=帮助");
        _canvas->pushSprite(0, 0);
        return;
    }

    // ---- 顶栏: 颜色/罚牌/牌堆/回合 ----
    drawColorSwatch(4, 3, _cur_color);
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(22, 3);
    _canvas->printf("堆%d", _deck_n);
    if (_pending > 0) {
        _canvas->setTextColor(TFT_ORANGE);
        _canvas->setCursor(66, 3);
        _canvas->printf("罚%d", _pending);
    }
    _canvas->setTextColor(_cur == 0 ? TFT_GREEN : TFT_MAGENTA);
    _canvas->setCursor(w - 64, 3);
    _canvas->print(_cur == 0 ? "你的回合" : (_mode == 1 ? "AI回合" : "对方回合"));

    // ---- 中部: 弃牌堆顶 ----
    if (_top >= 0) drawCard(30, 26, 34, 46, _top, false);

    // ---- 右侧: 双方剩牌/比分 ----
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(78, 26);
    _canvas->printf("P1 %d张", _hn[0]);
    _canvas->setCursor(78, 42);
    _canvas->printf("分%d", _score[0]);
    _canvas->setTextColor(TFT_MAGENTA);
    _canvas->setCursor(78, 60);
    _canvas->printf("P2 %d张", _hn[1]);
    _canvas->setCursor(78, 76);
    _canvas->printf("分%d", _score[1]);
    _canvas->setTextColor(TFT_DARKGREY);
    _canvas->setCursor(78, 94);
    _canvas->printf("第%d局", _round);

    // 操作提示: 统一收进 H 帮助, 入口放右下角
    _canvas->setFont(&fonts::efontCN_10_b);
    _canvas->setTextColor(TFT_DARKGREY);
    _canvas->setCursor(190, 84);
    _canvas->print("H=帮助");
    _canvas->setFont(&fonts::efontCN_12);

    // ---- 偷窥: 我(键盘方)按住 P 看 P2 的牌 ----
    if (_peek_local) {
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setCursor(124, 3);
        _canvas->print("偷窥P2");
        int n = _hn[1];
        int show = n > 6 ? 6 : n;
        for (int i = 0; i < show; i++)
            drawCard(124 + i * 18, 18, 18, 26, _hand[1][i], false);
        if (n > 6) {
            _canvas->setTextColor(TFT_DARKGREY);
            _canvas->setCursor(232, 24);
            _canvas->printf("+%d", n - 6);
        }
    }
    // ---- 对方(手柄方)正在偷窥我的手牌: 系统警告 ----
    if (_mode == 2 && _peek_remote) {
        _canvas->fillRect(0, 1, w, 16, TFT_RED);
        _canvas->setTextColor(TFT_WHITE);
        const char* wt = "对方正在偷窥你的手牌!";
        int tw = _canvas->textWidth(wt);
        _canvas->setCursor((w - tw) / 2, 3);
        _canvas->print(wt);
    }

    // ---- 底部: 我的手牌 ----
    int n = _hn[0];
    int cw = 22, ch = 30, gap = 2;
    int total = n * (cw + gap);
    int maxw = w - 8;
    if (total > maxw) { cw = maxw / n - gap; if (cw < 12) cw = 12; total = n * (cw + gap); }
    int x0 = 4;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * (cw + gap);
        int y = (i == _sel && _cur == 0) ? 96 : 100;
        drawCard(x, y, cw, ch, _hand[0][i], false);
        if (i == _sel && _cur == 0) _canvas->drawRect(x - 1, y - 1, cw + 2, ch + 2, TFT_YELLOW);
    }

    // ---- 变色选色 ----
    if (_choosing == 0) {
        _canvas->fillRect(60, 40, 120, 50, TFT_BLACK);
        _canvas->drawRect(60, 40, 120, 50, TFT_WHITE);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(66, 44);
        _canvas->print("选颜色");
        for (int c = 0; c < 4; c++) {
            int x = 74 + c * 24;
            drawColorSwatch(x, 62, c + 1);
            if (c == _color_sel) _canvas->drawRect(x - 2, 60, 16, 16, TFT_YELLOW);
        }
    }
    if (_choosing == 1) {
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setCursor(66, 44);
        _canvas->print("对方选颜色..");
    }

    // ---- 局末 ----
    if (_over) {
        _canvas->fillRect(34, 40, 172, 52, TFT_BLACK);
        _canvas->drawRect(34, 40, 172, 52, TFT_YELLOW);
        _canvas->setTextColor(TFT_YELLOW);
        const char* wt = _match_end
            ? (_winner == 0 ? "你赢得比赛!" : "对方赢得比赛!")
            : (_winner == 0 ? "本局你赢!" : "本局对方赢!");
        int tw = _canvas->textWidth(wt);
        _canvas->setCursor((w - tw) / 2, 48);
        _canvas->print(wt);
        _canvas->setTextColor(TFT_DARKGREY);
        const char* hint = _match_end ? "Enter 重开新赛" : "Enter 下一局";
        tw = _canvas->textWidth(hint);
        _canvas->setCursor((w - tw) / 2, 70);
        _canvas->print(hint);
    }

    _canvas->pushSprite(0, 0);
}

// ---------------- 帮助 ----------------
void AppUno::drawHelp(int w, int h) {
    (void)h;
    _canvas->fillScreen(TFT_BLACK);
    _canvas->setFont(&fonts::efontCN_10_b);

    _canvas->setTextColor(TFT_YELLOW);
    _canvas->setCursor(6, 4);
    _canvas->print("UNO 操作说明");
    _canvas->setTextColor(TFT_DARKGREY);
    _canvas->setCursor(w - 52, 4);
    _canvas->print("H 关闭");

    static const char* kLines[] = {
        "A / D    选牌",
        "Enter    出牌 (没牌可出时自动摸牌)",
        "U        喊 UNO (剩 1 张时)",
        "按住 P   偷窥对手手牌",
        "H        打开 / 关闭帮助",
    };
    _canvas->setTextColor(TFT_WHITE);
    int y = 22;
    for (int i = 0; i < 5; i++) {
        _canvas->setCursor(6, y);
        _canvas->print(kLines[i]);
        y += 16;
    }
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(6, y + 2);
    _canvas->print("手柄: 左右选牌 A出牌 B喊UNO 按住Y偷窥");
}

// UNOSTATE 0x24 (64B)
// 0-2 'G','M',0x24 | 3 seq | 4 flags | 5 cur | 6 dir | 7 color | 8 top | 9 deck | 10 pending
// 11 hn0 | 12 hn1 | 13-28 hand0[16] | 29-44 hand1[16]
// 45 sel | 46 choosing(0xFF=无) | 47 color_sel | 48 round | 49 match_end
// 50 score0 | 51 score1 | 52 winner(255=无) | 53-63 保留
// flags: bit0联机 bit1单人 bit2开局 bit3结束 bit4本地UNO bit5远端UNO bit6 P1偷窥 bit7 P2偷窥
void AppUno::sendMirrorState() {
    if (!glink_is_linked()) return;
    // 菜单阶段也发 state: C3 收到后能显示 UNO 菜单画面, 不会卡在"等待画面数据..."
    static uint8_t seq = 0;
    uint8_t p[64];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x24; p[3] = seq++;
    p[4] = (uint8_t)((_mode == 2 ? 0x01 : 0) | (_mode == 1 ? 0x02 : 0) | 0x04
                     | (_over ? 0x08 : 0) | (_uno_ok[0] ? 0x10 : 0) | (_uno_ok[1] ? 0x20 : 0)
                     | (_peek_local ? 0x40 : 0) | (_peek_remote ? 0x80 : 0));
    p[5] = (uint8_t)_cur;
    p[6] = (uint8_t)(_dir > 0 ? 1 : 0);
    p[7] = (uint8_t)_cur_color;
    p[8] = (uint8_t)_top;
    p[9] = (uint8_t)_deck_n;
    p[10] = (uint8_t)_pending;
    p[11] = (uint8_t)_hn[0];
    p[12] = (uint8_t)_hn[1];
    for (int i = 0; i < _hn[0] && i < 16; i++) p[13 + i] = _hand[0][i];
    for (int i = 0; i < _hn[1] && i < 16; i++) p[29 + i] = _hand[1][i];
    p[45] = (uint8_t)_sel;
    p[46] = (uint8_t)(_choosing < 0 ? 0xFF : _choosing);
    p[47] = (uint8_t)_color_sel;
    p[48] = (uint8_t)_round;
    p[49] = (uint8_t)(_match_end ? 1 : 0);
    p[50] = (uint8_t)(_score[0] > 255 ? 255 : _score[0]);
    p[51] = (uint8_t)(_score[1] > 255 ? 255 : _score[1]);
    p[52] = (uint8_t)(_winner < 0 ? 255 : _winner);
    glink_send_state(p, sizeof(p));
}

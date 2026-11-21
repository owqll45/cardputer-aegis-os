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

/* 牌堆空了: 只把弃牌堆(不含桌面顶牌)重洗回牌堆。
 * 原来直接 buildDeck() 重发一整副新牌, 手牌只增不减, 一局永远打不完。 */
void AppUno::recycleDiscard() {
    if (_deck_n > 0) return;
    if (_discard_n <= 1) return;
    int n = _discard_n - 1;                     // 最后一张是桌面顶牌, 留在桌上
    for (int i = 0; i < n; i++) _deck[i] = _discard[i];
    _deck_n = n;
    _discard[0] = _discard[n];
    _discard_n = 1;
    for (int i = _deck_n - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = _deck[i]; _deck[i] = _deck[j]; _deck[j] = t;
    }
}

void AppUno::drawCards(int p, int n) {
    for (int k = 0; k < n; k++) {
        if (_deck_n <= 0) recycleDiscard();
        if (_deck_n <= 0) break;
        if (_hn[p] >= HAND_MAX) break;
        _hand[p][_hn[p]++] = _deck[--_deck_n];
    }
    if (_hn[p] > 1) _uno_ok[p] = false;
    _dirty = true;
}

void AppUno::startRound() {
    buildDeck();
    _discard_n = 0;
    for (int i = 0; i < NP_MAX; i++) { _hn[i] = 0; _uno_ok[i] = false; _uno_pending[i] = false; }
    for (int i = 0; i < _np; i++) drawCards(i, 7);

    // 翻首牌: 避开变色牌, 保证开局有颜色
    do {
        _top = _deck[--_deck_n];
    } while (_deck_n > 0 && cardColor(_top) == 0);
    _cur_color = cardColor(_top) ? cardColor(_top) : 1;
    _discard[0] = (uint8_t)_top;
    _discard_n = 1;
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
    /* 跟"同点数"出的牌(颜色不同)必须把当前色切到新牌的颜色。
     * 以前只在打变色牌时改 _cur_color, 顶牌早就变成黄/绿了, 当前色还停在旧色:
     * 下一家按旧色判可出牌, 结果 AI 一直摸不到"旧色"的牌, 整局无限摸牌打不完。 */
    if (cardColor(id) != 0) _cur_color = cardColor(id);
    if (_discard_n < 108) _discard[_discard_n++] = (uint8_t)id;   // 记弃牌, 牌堆空了重洗它
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
        if (p == 0) {
            snprintf(_hint, sizeof(_hint), "吃罚 %d 张", _pending);
            _hint_until = GetHAL().millis() + 1500;
        }
        _pending = 0;
        _pkind = 0;
        audio::play_tone(300, 0.08);
        advance(1);
    } else {
        int before = _hn[p];
        drawCards(p, 1);
        if (_hn[p] == before) {          // 牌堆+弃牌堆都空了, 只能过
            audio::play_tone(200, 0.05);
            advance(1);
        } else {
            /* 摸到能出的牌: 留在当前回合, 让玩家马上能打出去。
             * 以前一律"摸一张就过", 手牌越摸越多, 一局打不完。 */
            int drawn = _hand[p][_hn[p] - 1];
            if (canPlay(drawn)) {
                if (p == 0) {
                    _sel = _hn[p] - 1;
                    snprintf(_hint, sizeof(_hint), "摸到能出的牌，按 ENTER 出");
                    _hint_until = GetHAL().millis() + 2000;
                } else if (_mode == 1) {
                    _ai_ms = GetHAL().millis() + 500;
                }
                audio::play_tone(660, 0.05);
                _dirty = true;
                return;
            }
            if (p == 0) {
                snprintf(_hint, sizeof(_hint), "没牌可出，摸1张后过");
                _hint_until = GetHAL().millis() + 1500;
            }
            audio::play_tone(300, 0.05);
            advance(1);
        }
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
            if (p == 0) {
                snprintf(_hint, sizeof(_hint), "没喊 UNO，罚摸 2 张");
                _hint_until = now + 1800;
            }
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
    _want_close = false;
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

        // ESC/退格: 帮助层打开时先关帮助(帮助页写的是"H/Esc 关"), 否则返回桌面
        if (e.keyCode == KEY_ESC || e.keyCode == KEY_BACKSPACE) {
            if (e.state) {
                if (_help) {
                    _help = false;
                    _dirty = true;
                } else {
                    _want_close = true;
                }
            }
            return;
        }

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
        if (_hn[0] <= 0) return;

        if (e.keyCode == KEY_LEFT || e.keyCode == KEY_A) { _sel = (_sel + _hn[0] - 1) % _hn[0]; _dirty = true; }
        else if (e.keyCode == KEY_RIGHT || e.keyCode == KEY_D) { _sel = (_sel + 1) % _hn[0]; _dirty = true; }
        else if (e.keyCode == KEY_ENTER) {
            if (_sel < _hn[0] && canPlay(_hand[0][_sel])) doPlay(0, _sel);
            else if (!anyPlayable(0)) doDrawTurn(0);
            else {
                /* 以前只响一声 200Hz 静音, 玩家不知道"为什么这张出不了" */
                snprintf(_hint, sizeof(_hint), "这张出不了，A/D 换一张");
                _hint_until = GetHAL().millis() + 1600;
                audio::play_tone(200, 0.05);
                _dirty = true;
            }
        } else if (e.keyCode == KEY_U) {
            if (_hn[0] <= 2) _uno_ok[0] = true;
            else {
                snprintf(_hint, sizeof(_hint), "要剩 1 张才能喊 UNO");
                _hint_until = GetHAL().millis() + 1600;
                audio::play_tone(200, 0.05);
            }
            _dirty = true;
        }
    });
}

void AppUno::onRunning() {
    if (_want_close) { close(); return; }
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

    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
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
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString("?", x + w / 2, y + h / 2);
        _canvas->setTextDatum(top_left);
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

    char t[4];
    if (v <= 9) snprintf(t, sizeof(t), "%d", v);
    else if (v == 10) snprintf(t, sizeof(t), "S");
    else if (v == 11) snprintf(t, sizeof(t), "R");
    else if (v == 12) snprintf(t, sizeof(t), "2");
    else if (v == 13) snprintf(t, sizeof(t), "W");
    else snprintf(t, sizeof(t), "4");

    if (w >= 22 && h >= 26) {
        // 大牌: 白底 + 12px 内容
        _canvas->fillRect(x + 2, y + 2, w - 4, h - 4, TFT_WHITE);
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setTextColor(bg == TFT_YELLOW ? TFT_BLACK : bg);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString(t, x + w / 2, y + h / 2);
        _canvas->setTextDatum(top_left);
        if (v == 12 || v == 14) {
            _canvas->drawFastHLine(x + 3, y + h - 5, w - 6, TFT_BLACK);
        }
    } else {
        // 小牌(监视行): 直接压色底写 10px 白字, 不用白底, 省空间
        _canvas->setFont(&fonts::efontCN_10);
        /* 黄底和白字几乎同亮, 变色牌(四色格)也有浅底, 这两种改黑字 */
        _canvas->setTextColor((col == 2 || col == 0) ? TFT_BLACK : TFT_WHITE);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString(t, x + w / 2, y + h / 2);
        _canvas->setTextDatum(top_left);
    }
    if (v == 13 || v == 14) {
        // 变色牌角标: 四个小色点(只在大牌上画, 小牌画了会糊)
        if (w >= 22) {
            _canvas->fillRect(x + 3, y + h - 6, 3, 3, TFT_RED);
            _canvas->fillRect(x + 8, y + h - 6, 3, 3, TFT_YELLOW);
            _canvas->fillRect(x + 13, y + h - 6, 3, 3, TFT_GREEN);
            _canvas->fillRect(x + 18, y + h - 6, 3, 3, TFT_BLUE);
        }
    }
}

void AppUno::draw() {
    if (!_canvas) return;
    int w = _canvas->width(), h = _canvas->height();
    _canvas->fillScreen(TFT_BLACK);
    _canvas->setFont(&fonts::efontCN_12);

    if (_help) {
        drawHelp(w, h);
        appCanvasPush(_canvas);
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
        /* 10px 下"选择"两个复杂字会糊成一团(实测), 这里用 12px。
         * 12px 全行约 110px, 240 宽放得下。 */
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setTextColor(TFT_LIGHTGREY);
        _canvas->setCursor(6, 90);
        _canvas->print("E/S 选择  ENTER 开始");
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(w - 54, 90);
        _canvas->print("H=帮助");
        appCanvasPush(_canvas);
        return;
    }

    // ---- 顶栏: 当前色 / 牌堆 / 罚牌 / 回合 ----
    drawColorSwatch(4, 3, _cur_color);
    _canvas->setFont(&fonts::efontCN_10);
    /* 只画一个 12px 色块时, 变色牌打出去以后玩家不知道现在该跟什么色;
     * 补一个汉字颜色名, 色块再小也读得出来。 */
    {
        static const char* kColorName[5] = { "无", "红", "黄", "绿", "蓝" };
        int ci = (_cur_color >= 1 && _cur_color <= 4) ? _cur_color : 0;
        _canvas->setTextColor(_cur_color == 1 ? TFT_RED
                              : _cur_color == 2 ? TFT_YELLOW
                              : _cur_color == 3 ? TFT_GREEN
                              : _cur_color == 4 ? TFT_CYAN : TFT_WHITE);
        _canvas->setCursor(18, 3);
        _canvas->print(kColorName[ci]);
    }
    /* 牌堆数只在左栏(y=46)显示。顶栏原来又打了一遍"堆N",
     * 同一个数字出现两次, 去掉顶栏这份, 罚牌提示往前挪。 */
    if (_pending > 0 && !_over) {
        _canvas->setTextColor(TFT_ORANGE);
        _canvas->setCursor(34, 3);
        _canvas->printf("罚%d", _pending);
    }
    const char* turnTx = _cur == 0 ? "你的回合" : (_mode == 1 ? "AI回合" : "对方回合");
    _canvas->setTextColor(_cur == 0 ? TFT_GREEN : TFT_MAGENTA);
    _canvas->setCursor(w - _canvas->textWidth(turnTx) - 4, 3);
    _canvas->print(turnTx);
    /* H 帮助入口从右栏 y=74 挪到顶栏: 手牌会画到 y=78, 原来的位置被牌盖住。 */
    _canvas->setTextColor(TFT_DARKGREY);
    _canvas->setCursor(126, 3);
    _canvas->print("H=帮助");

    // ---- 对手手牌行(监视) ----
    // 单人: 明牌(AI 手牌), 联机: 牌背; 按住 P 偷窥时也翻成明牌
    bool showOpp = (_mode == 1) || _peek_local;
    const char* oppLabel = _peek_local ? "偷窥对手" : (_mode == 1 ? "AI明牌" : "对手");
    _canvas->setTextColor(_peek_local ? TFT_YELLOW : TFT_MAGENTA);
    _canvas->setCursor(4, 18);
    _canvas->print(oppLabel);
    /* 张数以前画在下一行 y=30, 正好被对手手牌行(17~37)盖住, 而且是暗灰。
     * 现在固定在标签右侧 x=58, 手牌行固定从 x=96 起:
     * 以前按 textWidth 推算起点, 实测第一张牌会压住"张"字。 */
    char cntBuf[12];
    snprintf(cntBuf, sizeof(cntBuf), "%d张", _hn[1]);
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(58, 18);
    _canvas->print(cntBuf);

    {
        int ox = 96, oy = 17, ow = 14, oh = 20, og = 2;
        int nn = _hn[1];
        int maxShow = (w - ox - 6) / (ow + og);
        int show = nn > maxShow ? maxShow : nn;
        for (int i = 0; i < show; i++)
            drawCard(ox + i * (ow + og), oy, ow, oh, _hand[1][i], !showOpp);
        if (nn > show) {
            _canvas->setTextColor(TFT_DARKGREY);
            _canvas->setCursor(ox + show * (ow + og) + 1, oy + 5);
            _canvas->printf("+%d", nn - show);
        }
    }

    // ---- 中部: 弃牌堆顶 ----
    if (_top >= 0) drawCard(104, 44, 30, 36, _top, false);

    // ---- 左栏: 牌堆/方向/局数 ----
    /* 手牌从 y=78 起(选中抬高到 78), 原来"第N局"画在 y=74 会被手牌盖住,
     * 这里把方向与局数并到一行 y=60, 两行都落在 78 以上。 */
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(4, 46);
    _canvas->printf("牌堆%d", _deck_n);
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(4, 60);
    _canvas->printf("%s 第%d局", _dir > 0 ? "方向→" : "方向←", _round);

    // ---- 右栏: 双方剩牌/比分 ----
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(146, 46);
    _canvas->printf("P1 %d张 分%d", _hn[0], _score[0]);
    _canvas->setTextColor(TFT_MAGENTA);
    _canvas->setCursor(146, 60);
    /* 单人时 AI 张数已在上方"AI明牌 N张"里, 这里只留比分, 避免同一个数字出现两遍 */
    if (_mode == 1) _canvas->printf("AI 分%d", _score[1]);
    else _canvas->printf("P2 %d张 分%d", _hn[1], _score[1]);

    // ---- 对方(手柄方)正在偷窥我的手牌: 系统警告 ----
    if (_mode == 2 && _peek_remote) {
        _canvas->fillRect(0, 0, w, 15, TFT_RED);
        _canvas->setTextColor(TFT_WHITE);
        const char* wt = "对方正在偷窥你的手牌!";
        int tw = _canvas->textWidth(wt);
        _canvas->setCursor((w - tw) / 2, 2);
        _canvas->print(wt);
    }

    // ---- 底部: 我的手牌 + 常驻按键提示 ----
    int n = _hn[0];
    int cw = 22, ch = 26, gap = 2;
    int avail = w - 8;
    /* 原来只在 cw<12 时兜底, 但 n 大(吃罚牌)时 12 宽 × n 仍会超出右边缘,
     * 后面几张牌被裁掉。这里按可用宽度反算, 不够就先缩牌距再缩牌宽。 */
    if (n * (cw + gap) > avail) {
        gap = 1;
        cw = avail / n - gap;
        if (cw < 11) { gap = 0; cw = avail / n; }
        if (cw < 6) cw = 6;
    }
    int x0 = 4;
    /* 结算面板底边在 y=96, 手牌 78~108 会被盖掉上半截只留一条彩边(实测难看),
     * 局末不再画手牌, 让结算面板独占这块区域。 */
    for (int i = 0; !_over && i < n; i++) {
        int x = x0 + i * (cw + gap);
        int y = (i == _sel && _cur == 0) ? 78 : 82;
        drawCard(x, y, cw, ch, _hand[0][i], false);
        if (i == _sel && _cur == 0) {
            /* 原来黄框套在牌上, 黄牌/白底牌上几乎看不出哪张被选中(实测)。
             * 改成白色双层框: 外圈 1px 框在牌外, 内圈把牌自己的黑边盖成白色。 */
            _canvas->drawRect(x - 1, y - 1, cw + 2, ch + 2, TFT_WHITE);
            _canvas->drawRect(x, y, cw, ch, TFT_WHITE);
        }
    }

    /* 底部常驻一行操作提示: 以前局内只有 "H=帮助", 第一次玩的人完全不知道该按什么。
     * 出牌被拒时优先显示具体原因 1.6s。 */
    if (h >= 118 && !_over) {
        /* 10px 下"选牌/喊"这类复杂字会糊(实测), 12px 整行约 150px 放得下。 */
        _canvas->setFont(&fonts::efontCN_12);
        const char* line = "A/D 选牌  ENTER 出牌  U 喊UNO";
        bool hasHint = _hint[0] && (int32_t)(_hint_until - GetHAL().millis()) > 0;
        if (hasHint) {
            line = _hint;
            _canvas->setTextColor(TFT_YELLOW);
        } else {
            _canvas->setTextColor(TFT_LIGHTGREY);
        }
        int tw = _canvas->textWidth(line);
        _canvas->fillRect(0, h - 13, w, 13, TFT_BLACK);
        _canvas->setCursor((w - tw) / 2, h - 13);
        _canvas->print(line);
    }

    // ---- 变色选色 ----
    if (_choosing == 0) {
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->fillRect(60, 40, 120, 46, TFT_BLACK);
        _canvas->drawRect(60, 40, 120, 46, TFT_WHITE);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(66, 44);
        _canvas->print("选颜色");
        for (int c = 0; c < 4; c++) {
            int x = 74 + c * 24;
            drawColorSwatch(x, 64, c + 1);
            if (c == _color_sel) _canvas->drawRect(x - 2, 62, 16, 16, TFT_YELLOW);
        }
    }
    if (_choosing == 1) {
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setCursor(66, 44);
        _canvas->print("对方选颜色..");
    }

    // ---- 局末 ----
    if (_over) {
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->fillRect(30, 34, 180, 62, TFT_BLACK);
        _canvas->drawRect(30, 34, 180, 62, TFT_YELLOW);
        _canvas->setTextColor(TFT_YELLOW);
        const char* wt = _match_end
            ? (_winner == 0 ? "你赢得比赛!" : "对方赢得比赛!")
            : (_winner == 0 ? "本局你赢!" : "本局对方赢!");
        int tw = _canvas->textWidth(wt);
        _canvas->setCursor((w - tw) / 2, 40);
        _canvas->print(wt);
        /* 比分行放进面板: 面板会盖住右栏的"P1 x张 分N", 不在这里显示就没处看了 */
        char sb[32];
        if (_mode == 1) snprintf(sb, sizeof(sb), "你 %d : AI %d", _score[0], _score[1]);
        else snprintf(sb, sizeof(sb), "P1 %d : P2 %d", _score[0], _score[1]);
        _canvas->setTextColor(TFT_WHITE);
        tw = _canvas->textWidth(sb);
        _canvas->setCursor((w - tw) / 2, 57);
        _canvas->print(sb);
        /* 10px 的"下一局"三个字会糊, 用 12px 浅灰 */
        _canvas->setTextColor(TFT_LIGHTGREY);
        const char* hint = _match_end ? "Enter 重开新赛" : "Enter 下一局";
        tw = _canvas->textWidth(hint);
        _canvas->setCursor((w - tw) / 2, 76);
        _canvas->print(hint);
    }

    appCanvasPush(_canvas);
}

// ---------------- 帮助 ----------------
void AppUno::drawHelp(int w, int h) {
    (void)h;
    _canvas->fillScreen(TFT_BLACK);
    /* 10px 下"选牌/出牌"这类汉字会糊(实测), 正文改 12px, 行距压到 17 行得下。 */
    _canvas->setFont(&fonts::efontCN_12);

    _canvas->setTextColor(TFT_YELLOW);
    _canvas->setCursor(6, 2);
    _canvas->print("UNO 操作说明");
    _canvas->setTextColor(TFT_DARKGREY);
    _canvas->setCursor(w - 58, 2);
    _canvas->print("H/Esc 关");

    static const char* kLines[] = {
        "A / D  选牌",
        "Enter 出牌，没牌摸1张",
        "摸到能出的牌可立刻出",
        "U 喊 UNO（剩 1 张）",
        "按住 P 偷窥  Esc 退出",
    };
    _canvas->setTextColor(TFT_WHITE);
    int y = 20;
    for (int i = 0; i < 5; i++) {
        _canvas->setCursor(6, y);
        _canvas->print(kLines[i]);
        y += 17;
    }
    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(6, y - 1);   // 贴底会被画布裁掉 1~2px, 上提一行
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

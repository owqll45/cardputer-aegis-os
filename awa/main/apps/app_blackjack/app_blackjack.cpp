#include "app_blackjack.h"
#include <mooncake_log.h>
#include <apps/utils/app_canvas.h>
#include <apps/utils/common.h>
#include <esp_random.h>
#include <stdio.h>

// 牌编码: 0..51, rank = id % 13 (0=A, 10=J, 11=Q, 12=K), suit = id / 13
static const char* kRankName[13] = {"A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};

AppBlackjack::AppBlackjack()
{
    setAppInfo().name = "Blackjack";
    setAppInfo().icon = nullptr;
}

AppBlackjack::~AppBlackjack()
{
    _canvas = nullptr;
}

void AppBlackjack::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");

    GetHAL().display().fillScreen(TFT_BLACK);
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    _chips = 100;
    _bet = 10;
    _round = 1;
    deal();

    _keyq.clear();
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        _keyq.push(e);
    });

    draw();
    appCanvasPush(_canvas);
}

void AppBlackjack::shuffleDeck()
{
    for (int i = 0; i < 52; i++) _deck[i] = (uint8_t)i;
    for (int i = 51; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = _deck[i];
        _deck[i] = _deck[j];
        _deck[j] = t;
    }
    _deck_n = 52;
}

uint8_t AppBlackjack::drawCard()
{
    if (_deck_n <= 0) shuffleDeck();
    return _deck[--_deck_n];
}

int AppBlackjack::handTotal(int who) const
{
    int total = 0, aces = 0;
    for (int i = 0; i < _hn[who]; i++) {
        int r = _hand[who][i] % 13;
        if (r == 0) { total += 11; aces++; }
        else if (r >= 9) { total += 10; }
        else total += r + 1;
    }
    while (total > 21 && aces > 0) { total -= 10; aces--; }
    return total;
}

void AppBlackjack::deal()
{
    if (_deck_n < 20) shuffleDeck();
    _hn[0] = _hn[1] = 0;
    _dealer_hide = true;
    _result = 0;
    // 轮流发牌: 玩家 / 庄 / 玩家 / 庄(暗)
    _hand[0][_hn[0]++] = drawCard();
    _hand[1][_hn[1]++] = drawCard();
    _hand[0][_hn[0]++] = drawCard();
    _hand[1][_hn[1]++] = drawCard();

    if (_chips < _bet) {
        _chips = 100;  // 破产自动补 100, 不打断游玩
    }
    _chips -= _bet;

    if (handTotal(0) == 21) stand();  // 天生黑杰克直接摊牌
}

void AppBlackjack::hit()
{
    if (_result != 0) return;
    if (_hn[0] >= 12) return;
    _hand[0][_hn[0]++] = drawCard();
    if (handTotal(0) > 21) settle();
}

void AppBlackjack::stand()
{
    if (_result != 0) return;
    _dealer_hide = false;
    while (handTotal(1) < 17 && _hn[1] < 12) {
        _hand[1][_hn[1]++] = drawCard();
    }
    settle();
}

void AppBlackjack::settle()
{
    _dealer_hide = false;
    int p = handTotal(0);
    int d = handTotal(1);
    bool p_bj = (p == 21 && _hn[0] == 2);
    bool d_bj = (d == 21 && _hn[1] == 2);

    if (p > 21) {
        _result = 2;
    } else if (p_bj && !d_bj) {
        _result = 4;
        _chips += _bet * 2 + _bet / 2;
    } else if (d > 21) {
        _result = 1;
        _chips += _bet * 2;
    } else if (p > d) {
        _result = 1;
        _chips += _bet * 2;
    } else if (p == d) {
        _result = 3;
        _chips += _bet;
    } else {
        _result = 2;
    }

    mclog::tagInfo(getAppInfo().name, "round=%d player=%d dealer=%d result=%d chips=%d",
                   _round, p, d, _result, _chips);
}

void AppBlackjack::handleKey(const Keyboard::KeyEvent_t& e)
{
    if (e.isModifier || !e.state) return;
    if (e.keyCode == KEY_ESC) { close(); return; }

    if (_result == 0) {
        if (e.keyCode == KEY_H) hit();
        else if (e.keyCode == KEY_S) stand();
        return;
    }

    if (e.keyCode == KEY_N || e.keyCode == KEY_ENTER) {
        _round++;
        deal();
    } else if (e.keyCode == KEY_R) {
        _chips = 100;
        _round = 1;
        deal();
    }
}

void AppBlackjack::drawCard(int x, int y, uint8_t id, bool hidden)
{
    auto* c = _canvas;
    const int w = 22, h = 28;
    if (hidden) {
        c->fillRect(x, y, w, h, 0x2945);
        c->drawRect(x, y, w, h, TFT_WHITE);
        c->setFont(&fonts::efontCN_10);
        c->setTextColor(TFT_WHITE);
        c->setTextDatum(middle_center);
        c->drawString("?", x + w / 2, y + h / 2);
        c->setTextDatum(top_left);
        return;
    }
    int rank = id % 13;
    int suit = id / 13;
    bool red = (suit == 1 || suit == 2);  // 红桃 / 方块
    c->fillRect(x, y, w, h, TFT_WHITE);
    c->drawRect(x, y, w, h, red ? TFT_RED : TFT_BLACK);
    c->setFont(&fonts::efontCN_12);
    c->setTextColor(red ? TFT_RED : TFT_BLACK);
    c->setTextDatum(middle_center);
    c->drawString(kRankName[rank], x + w / 2, y + h / 2 - 1);
    c->setTextDatum(top_left);
}

void AppBlackjack::draw()
{
    if (!_canvas) return;
    auto* c = _canvas;
    int w = c->width();
    int h = c->height();
    c->fillScreen(TFT_BLACK);
    c->setTextDatum(top_left);

    // 顶栏
    c->setFont(&fonts::efontCN_10);
    c->setTextColor(TFT_YELLOW);
    c->setCursor(4, 2);
    c->printf("21点 筹码%d 注%d", _chips, _bet);
    c->setTextColor(TFT_DARKGREY);
    c->setCursor(w - 46, 2);
    c->printf("第%d局", _round);

    // 庄家
    c->setTextColor(TFT_MAGENTA);
    c->setCursor(4, 18);
    if (_dealer_hide) c->print("庄 ?");
    else c->printf("庄 %d", handTotal(1));
    int sp1 = (_hn[1] > 8) ? 16 : 24;
    for (int i = 0; i < _hn[1]; i++) {
        drawCard(34 + i * sp1, 14, _hand[1][i], _dealer_hide && i == 1);
    }

    // 玩家
    c->setTextColor(TFT_CYAN);
    c->setCursor(4, 52);
    c->printf("你 %d", handTotal(0));
    int sp0 = (_hn[0] > 8) ? 16 : 24;
    for (int i = 0; i < _hn[0]; i++) {
        drawCard(34 + i * sp0, 48, _hand[0][i], false);
    }

    // 底栏 / 结果
    c->setFont(&fonts::efontCN_10);
    c->setCursor(4, h - 11);
    if (_result == 0) {
        c->setTextColor(TFT_DARKGREY);
        c->print("H=要牌  S=停牌  Esc=退出");
    } else {
        const char* rt = "";
        uint16_t rc = TFT_WHITE;
        switch (_result) {
            case 1: rt = "你赢了!"; rc = TFT_GREEN; break;
            case 2: rt = "你输了"; rc = TFT_RED; break;
            case 3: rt = "平局, 退还下注"; rc = TFT_YELLOW; break;
            default: rt = "黑杰克! +15"; rc = TFT_ORANGE; break;
        }
        c->setTextColor(rc);
        c->setTextDatum(middle_center);
        c->drawString(rt, w / 2, h - 22);
        c->setTextColor(TFT_DARKGREY);
        c->drawString("N/Enter=下一局  R=重置筹码", w / 2, h - 9);
        c->setTextDatum(top_left);
    }
}

void AppBlackjack::onRunning()
{
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    Keyboard::KeyEvent_t e;
    while (_keyq.pop(e)) {
        handleKey(e);
        if (!_canvas) return;
    }

    draw();
    appCanvasPush(_canvas);
}

void AppBlackjack::onClose()
{
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
    _keyq.clear();
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

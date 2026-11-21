#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <apps/utils/key_event_queue.h>

// 21 点: 键盘对庄家, 固定下注, 带筹码
class AppBlackjack : public mooncake::AppAbility {
public:
    AppBlackjack();
    ~AppBlackjack();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    KeyEventQueue _keyq;

    uint8_t _deck[52];
    int _deck_n = 0;

    uint8_t _hand[2][12];
    int _hn[2] = {0, 0};
    bool _dealer_hide = true;

    int _chips = 100;
    int _bet = 10;
    int _result = 0;      // 0 进行中 1 赢 2 输 3 平 4 黑杰克
    int _round = 1;

    void shuffleDeck();
    uint8_t drawCard();
    int handTotal(int who) const;
    void deal();
    void hit();
    void stand();
    void settle();
    void handleKey(const Keyboard::KeyEvent_t& e);
    void drawCard(int x, int y, uint8_t id, bool hidden);
    void draw();
};

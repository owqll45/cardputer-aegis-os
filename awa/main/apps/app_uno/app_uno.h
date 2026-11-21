/*
 * UNO 纸牌: 单人(vs AI) / 双人联机(C3+Xbox手柄=P2)
 * 规则全开: 数字/跳过/反转/罚2/变色/罚4变色 + 罚牌叠加 + UNO喊牌 + 计分赛
 * 偷窥: P1 按住 P 偷看 P2 手牌(P2 的 C3 出警告); P2 按住 Y 偷看 P1(Cardputer 出警告)
 * 镜像: UNOSTATE 0x24 64B 全量包, C3 本地重绘
 */
#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>

class AppUno : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    static const int NP_MAX = 4;
    static const int HAND_MAX = 32;
    static const int TARGET_SCORE = 500;

    int _mode = 0;          // 0菜单 1单人AI 2双人联机
    int _menu_sel = 0;

    uint8_t _deck[108];
    int _deck_n = 0;
    uint8_t _discard[108];      // 出过的牌(最后一张=桌面顶牌), 牌堆空了用它重洗
    int _discard_n = 0;
    uint8_t _hand[NP_MAX][HAND_MAX];
    int _hn[NP_MAX] = { 0 };
    int _np = 2;            // 当前人数(先做2, 留4扩展)
    int _top = -1;
    int _cur_color = 1;     // 1红 2黄 3绿 4蓝
    int _cur = 0;
    int _dir = 1;
    int _pending = 0;       // 累加罚牌数
    int _pkind = 0;         // 12=罚2 14=罚4
    int _sel = 0;
    int _choosing = -1;     // >=0: 该玩家正在选变色颜色
    int _color_sel = 0;
    bool _over = false;
    int _winner = -1;
    bool _match_end = false;
    int _score[NP_MAX] = { 0 };
    int _round = 1;
    bool _uno_ok[NP_MAX] = { false };
    bool _uno_pending[NP_MAX] = { false };
    uint32_t _uno_deadline[NP_MAX] = { 0 };
    bool _peek_local = false;
    bool _peek_remote = false;
    bool _help = false;
    uint16_t _btn_prev = 0;
    uint32_t _ai_ms = 0;
    bool _dirty = true;
    bool _want_close = false;   // ESC 只置标记, 由 onRunning 统一 close(回调里直接关不安全)
    char _hint[40] = { 0 };     // 出牌被拒等一次性提示(画在底部提示行)
    uint32_t _hint_until = 0;

    // 规则
    static int cardColor(int id) { return id < 100 ? id / 25 + 1 : 0; }
    static int cardValue(int id) {
        if (id >= 100) return id >= 104 ? 14 : 13;
        int r = id % 25;
        if (r == 0) return 0;
        return (r - 1) / 2 + 1;   // 1..9 10=skip 11=rev 12=draw2
    }
    bool canPlay(int id) const;
    bool anyPlayable(int p) const;
    void buildDeck();
    void recycleDiscard();
    void drawCards(int p, int n);
    void startRound();
    void doPlay(int p, int idx);
    void applyEffect(int p, int id);
    void advance(int steps);
    void confirmColor(int color);
    void doDrawTurn(int p);
    void endRound(int p);
    void aiTurn(uint32_t now);
    void updateUno(uint32_t now);
    void handleButtons(uint32_t now);
    void sendMirrorState();
    void draw();
    void drawHelp(int w, int h);
    void drawCard(int x, int y, int w, int h, int id, bool hidden);
    void drawColorSwatch(int x, int y, int color);

public:
    AppUno();
    ~AppUno();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

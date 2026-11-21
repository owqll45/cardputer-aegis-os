#pragma once
#include <mooncake.h>
#include <M5GFX.h>

/*
 * 斗地主（240x135）
 * 三家：本机(0) / 联机手柄(1) / AI(2)
 * 联机：沿用 glink 的 C3 手柄控制通道；没连手柄时 P2 自动交给 AI 托管。
 * 作弊：C 键开菜单，左上角显示 CHEAT；AI 累积怀疑，达阈值触发问责。
 */
class AppDoudizhu : public mooncake::AppAbility {
public:
    AppDoudizhu();
    ~AppDoudizhu();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // ---- 牌 ----
    // 0..51: suit*13 + rank（rank 0=3 … 12=2）；52=小王；53=大王
    static const int MAXHAND = 20;
    int _hand[3][MAXHAND];
    int _hand_n[3];
    int _bottom[3];

    // ---- 牌型 ----
    enum ComboType {
        CT_NONE = 0,
        CT_SINGLE,
        CT_PAIR,
        CT_TRIO,
        CT_TRIO1,
        CT_TRIO2,
        CT_STRAIGHT,
        CT_PAIRS,
        CT_PLANE,
        CT_PLANE1,
        CT_PLANE2,
        CT_FOUR2,
        CT_FOUR22,
        CT_BOMB,
        CT_ROCKET,
    };
    struct Combo {
        int type = CT_NONE;
        int rank = 0;   // 主牌 rank（0..12, 13=小王, 14=大王）
        int len = 0;    // 顺子/连对/飞机的连数
        int n = 0;      // 张数
    };

    // ---- 局面 ----
    enum Phase { PH_DEAL, PH_BID, PH_PLAY, PH_OVER, PH_ACCUSE };
    Phase _phase = PH_DEAL;
    int _landlord = -1;
    int _winner = -1;      // 上一局出完牌的人(结算面板要显示谁赢)
    int _cur = 0;
    int _leader = -1;      // 最近一次有效出牌者
    int _last_player = -1;
    int _pass_count = 0;
    Combo _last_combo;
    int _last_cards[MAXHAND];
    int _last_n = 0;
    int _bid_turn = 0;
    int _bid_count = 0;

    // ---- 本机操作 ----
    int _cursor = 0;
    bool _sel[MAXHAND];
    bool _reveal = false;     // 作弊透视
    int _cheat_menu = 0;      // 0=关 1=开
    int _suspicion = 0;
    bool _cheat_used = false;
    bool _accuse_yes = false;
    bool _ai_angry = false;
    uint32_t _msg_until = 0;
    char _msg[48] = {0};
    uint32_t _ai_wait_until = 0;
    uint32_t _remote_wait_until = 0;
    bool _p2_manual = false;        // 手柄真的操作过才把 P2 交给玩家，否则 AI 托管
    uint32_t _p2_deadline = 0;
    bool _two_human = false;        // 双人对战: 本机 + 手柄两位真人, AI 只坐第三家

    // ---- 嘴炮(与五子棋同一套: 台词表 + 随机 + 红字 1px 黑描边, 布局不同) ----
    char _taunt[40] = {0};
    uint32_t _taunt_until = 0;
    void showTaunt(int kind);
    void drawTaunt();

    // ---- 比分 ----
    int _score[3] = { 0, 0, 0 };
    int _round = 0;

    // ---- 渲染 ----
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // ---- 流程 ----
    void resetRound();
    void dealCards();
    void startBid();
    void bidAction(int who, bool call);
    void beginPlay();
    void nextTurn();
    void playCards(int who, const int* cards, int n);
    void passTurn(int who);
    void finishGame(int winner);
    void aiBid();
    void aiAct(bool force = false);

    bool isHuman(int who) const;
    bool isTeammate(int a, int b) const;

    // ---- 规则 ----
    static int cardRank(int c);
    static int cardSuit(int c);
    static const char* rankName(int rank);
    static const char* suitName(int c);
    static bool buildCombo(const int* cards, int n, Combo& out);
    static bool comboBeats(const Combo& a, const Combo& b);
    static void sortHand(int* h, int n);
    bool removeCards(int who, const int* cards, int n);

    bool findBeat(int who, const Combo& last, int* out, int& out_n) const;
    bool findLead(int who, int* out, int& out_n) const;
    bool canFinishInOne(int who, int* out, int& out_n) const;

    // ---- 作弊 ----
    void cheatReveal();
    void cheatSwap();
    void cheatForcePass();
    void addSuspicion(int v);
    void triggerAccuse();
    void resolveAccuse(bool admit);

    // ---- 输入 ----
    void processKeys();
    void handleKey(int keyCode, const char* keyName);
    void remoteInput();
    void moveCursor(int d);
    void toggleSelect();

    // ---- 绘制 ----
    void render();
    void drawCard(int x, int y, int w, int h, int card, bool selected, bool faceDown);
    void drawHand();
    void drawTable();
    void drawHud();
    void drawOverlay();
    void showMsg(const char* text, uint32_t ms = 1500);
    void pushMirror();
};

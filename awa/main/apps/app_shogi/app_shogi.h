#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>
#include <apps/utils/key_event_queue.h>
#include <vector>

// 将棋 (Shogi) - 9x9 完整规则 + alpha-beta 搜索
class AppShogi : public mooncake::AppAbility {
public:
    AppShogi();
    ~AppShogi();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // ---- 棋盘常量 ----
    static const int SIZE = 9;
    int _cell = 13;
    int _board_w = 117;
    int _board_x = 61;
    int _board_y = 9;

    // 棋子种类
    enum Kind {
        PAWN = 1,
        LANCE,
        KNIGHT,
        SILVER,
        GOLD,
        BISHOP,
        ROOK,
        KING
    };

    struct Move {
        int8_t from;    // 棋盘坐标 0..80，落子为 -1
        int8_t to;      // 目标 0..80
        uint8_t drop;   // 0 = 走子，否则为打入的棋子种类
        bool promote;
    };

    struct Undo {
        uint8_t moved;
        uint8_t captured;
        int8_t from;
        int8_t to;
        uint8_t drop;
        uint8_t side;
    };

    struct Snapshot {
        uint8_t board[81];
        uint8_t hand[2][8];
        uint8_t turn;
    };

    // ---- 画布 ----
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    KeyEventQueue _keyq;
    void handleKey(const Keyboard::KeyEvent_t& e);

    // ---- 棋局状态 ----
    uint8_t _board[81];      // 0 空；编码见 makePiece()
    uint8_t _hand[2][8];     // 持驹数量
    uint8_t _turn = 1;       // 1 = 玩家(下方)，2 = AI(上方)

    int _cursor_r = 6;
    int _cursor_c = 4;
    int _sel_r = -1;
    int _sel_c = -1;
    int _drop_kind = 0;      // 0 表示未进入打入模式
    bool _prompt = false;    // 是否正在询问是否成子
    Move _pending{};
    Move _last_move{-1, -1, 0, false};

    int _level = 1;          // 1..5, starts at 1 star
    bool _two_player = false; // 双人同屏: 黑白双方都由键盘操作
    bool _help = false;       // H 键帮助层
    bool _game_over = false;
    int _winner = 0;
    bool _checked = false;
    uint32_t _ai_timer = 0;
    bool _ai_thinking = false;
    int _move_count = 0;

    // 非法操作/没轮到你时的一行提示(画在棋盘下方空白处)
    char _msg[28] = { 0 };
    uint32_t _msg_until = 0;

    // Win -> level-up transition
    uint32_t _win_anim_start = 0;
    bool _win_anim_active = false;
    bool _level_up_pending = false;

    std::vector<Snapshot> _history;

    // ---- AI ----
    bool _abort = false;
    int64_t _deadline_us = 0;
    uint32_t _nodes = 0;
    Move _best_move{-1, -1, 0, false};
    static const int MAX_MOVES = 160;
    Move _move_buf[6][MAX_MOVES];   // 按 ply 复用的走法缓冲

    // ---- 棋子编码 ----
    static int  ownerOf(uint8_t v);
    static int  kindOf(uint8_t v);
    static bool promotedOf(uint8_t v);
    static uint8_t makePiece(int owner, int kind, bool promoted);
    static int  baseValue(int kind, bool promoted);
    static const char* glyphOf(int kind, int owner);

    // ---- 规则 ----
    void setupBoard();
    void resetGame();
    static bool inBounds(int r, int c);
    static bool inPromotionZone(int owner, int r);
    static bool canPromoteKind(int kind);
    bool pawnFileHasOwn(int owner, int c);
    bool dropLegal(int side, int kind, int r, int c);
    void applyMove(const Move& m, Undo& u);
    void revertMove(const Move& m, const Undo& u);
    void genPseudoMoves(int side, Move* out, int& n, int maxN);
    void genLegalMoves(Move* out, int& n, int maxN);
    bool isLegal(const Move& m, int side);
    bool attackedSquare(int r, int c, int attacker);
    bool kingAttacked(int side);
    bool hasAnyLegalMove(int side);

    // ---- 搜索 ----
    int  evaluateBoard();              // 正数对玩家有利
    int  evaluateFor(int side);
    int  moveOrderScore(const Move& m);
    int  aiSearch(int depth, int alpha, int beta, int ply);
    void aiDecide();

    // ---- 交互 ----
    void handleEnter();
    void handleUndo();
    void pushSnapshot();
    void restoreSnapshot(const Snapshot& s);
    bool mustPromote(const Move& m, int side);
    bool mayPromote(const Move& m, int side);
    void commitMove(const Move& m, bool human);
    void checkGameEnd();

    // ---- 绘制 ----
    void drawBoard();
    void drawPiecesAndGrid();
    void drawHandsAndStatus();
    void drawPromotePrompt();
    void drawCheckMark();
    void sendMirrorState();
};

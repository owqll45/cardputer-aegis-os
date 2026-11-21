#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <stdint.h>
#include <vector>

class AppGomoku : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    uint8_t* _fb = nullptr; // 自申请的整屏 8bpp 缓冲(普通内部 RAM), setBuffer 绑定给 canvas
    bool _ui_closing = false;
    uint32_t _close_req_ms = 0; // 返回键请求时间: close() 没生效时用来重试, 防止卡在原界面
    void processPendingKeys(); // 在 app_task 里取出 desktop_task 入队的按键并执行
    int _handle_key_event_slot_id = -1;

    // Game state
    static const int BOARD_SIZE = 15;
    uint8_t _board[BOARD_SIZE][BOARD_SIZE]; // 0: Empty, 1: Black, 2: White
    int _cursor_x = 7;
    int _cursor_y = 7;
    int _current_player = 1; // 1: Black (Player), 2: White (AI/P2)
    bool _game_over = false;
    int _winner = 0;
    int _difficulty = 1; // 1-10 Stars, starts at 1 star and grows after each win
    bool _is_ai_mode = true;
    bool _is_ai_assist_mode = false; // User is White, AI plays White, User enters Black
    bool _ai_thinking = false;       // AI 正在思考 (negamax/vcf 跑的时候封锁玩家输入)
    uint32_t _ai_move_timer = 0;

    // In-game taunts + win/level-up transition
    uint32_t _taunt_timer = 0;
    bool _taunt_visible = false;
    int _taunt_kind = 0;   // 0=普通 1=长考 2=作弊 3=胜利
    uint32_t _win_anim_start = 0;
    bool _win_anim_active = false;
    bool _level_up_pending = false;

    // Win tracking (kept for the whole power-on session)
    static int s_player_wins;
    static int s_ai_wins;
    int _taunt_index = 0;

    // History for Undo
    struct Move {
        int r, c, player;
    };
    std::vector<Move> _move_history;

    // ---- AI engine ----
    uint8_t _near[BOARD_SIZE][BOARD_SIZE];   // 距离 2 内的棋子数（候选点用）
    int32_t _score[3];                       // 双方局面分（增量维护）
    int32_t _nodes = 0;
    /* 本次 AI 落子的累计节点数。_nodes 是"单次搜索"的计数器(超限就中止),
     * 会被 vcf/negamax 的多段搜索反复清零; 日志必须打这个累计值才可信。 */
    int32_t _nodes_move = 0;
    inline void resetSearchNodes() { _nodes_move += _nodes; _nodes = 0; }
    /* 落子/音效/绘制那段耗时(placePiece 内, 含阻塞式音频播放)。
     * 它跟搜索节点数无关, 必须从"思考耗时"里刨掉, 否则会出现
     * "nodes=0 却耗时 500ms"这种看起来像计数坏了的假象。 */
    uint32_t _place_ms = 0;
    /* 是否已经记下"本局转折点"(第一手走完对手就必胜的棋) */
    bool _worst_is_lost = false;
    int64_t _deadline_us = 0;
    int _best_r = -1;
    int _best_c = -1;
    bool _search_aborted = false;
    uint8_t _forbid[BOARD_SIZE][BOARD_SIZE]; // 作弊重来时禁手
    int _cheat_depth = 0;
    int _cheat_count = 0;
    int _cheat_kind_last = -1;
    bool _cheat_extra_move = false;          // 作弊：连走两手

    // ---- 右侧信息面板 ----
    bool _show_panel = true;      // G 键开关
    int _danger_moves = 0;        // AI 还要几步把你打死（0 = 没算出来）
    int8_t _trend[24];            // 局势趋势（-5..5）
    int _trend_head = 0;
    int _trend_count = 0;
    uint32_t _turn_start_ms = 0;  // 玩家回合开始（长考嘲讽）
    bool _long_taunt_done = false;

    // ---- 棋盘标记 ----
    // 0=无 1=你能在这里成五 2=AI 落这里会成四 3=AI 落这里直接成五
    uint8_t _threat[BOARD_SIZE][BOARD_SIZE];
    bool _show_threat = false;   // 默认关闭，T 键开启

    // ---- AI 性格 / 迭代加深 / 复盘 ----
    int _persona = 0;            // 0=稳健 1=凶狠 2=诡诈（K 键切换）
    int _iter_best_r = -1;       // 上一层迭代的最佳点（用于排序）
    int _iter_best_c = -1;
    int _blunders = 0;           // 玩家本局失误手数(还能救却没救)
    int _worst_loss = 0;         // 最大一手损失
    int _worst_move_no = 0;      // 最大失误是第几手
    int _futile_cnt = 0;         // 必挡·无奈: 已成必输局后被迫挡五的手数
    int _futile_first = 0;       // 第一手必挡·无奈是第几手

    // ---- 情绪 / 连击 / 成就 / 战报 ----
    bool _help_visible = false;  // H 键帮助界面
    int _mood = 0;               // 0=平静 1=急了（落后） 2=浪（领先）
    int _min_danger = 999;       // 本局出现过的最小危险步数（反杀判定）
    uint32_t _game_start_ms = 0;
    bool _result_visible = false;
    uint32_t _result_ms = 0;
    uint32_t _next_game_ms = 0;  // 结算面板右下角倒计时: 到点自动进下一局(等级已结算)
    uint16_t _achv = 0;          // 已解锁成就位
    uint16_t _achv_new = 0;      // 本局新解锁（战报显示）
    static int s_streak;         // 玩家连胜
    static int s_best_streak;

    // ---- 智力测试（评估模式）----
    // 每手用"悔手度"(走完后局面分 vs 最优候选的差)衡量, 不再用 reviewPlayerMove 的旧口径
    bool _assess_mode = false;
    bool _assess_banner = false;      // 开启后显示几秒"作弊键已禁用"
    uint32_t _assess_banner_ms = 0;
    int _assess_moves = 0;
    int32_t _assess_regret_sum = 0;
    int32_t _assess_regret_max = 0;
    int _assess_def_ok = 0;           // 对手有成五点而这一手挡掉了
    int _assess_def_need = 0;         // 对手有成五点、必须挡的次数
    int _assess_make4 = 0;            // 走完后自己出现成五点的次数(主动进攻)
    int32_t _assess_think_sum = 0;    // 累计思考毫秒
    int32_t _assess_think_max = 0;
    int32_t _assess_swing = 0;        // 单步局面分波动累计(稳定性)
    int32_t _assess_last_diff = 0;
    int _assess_atk = 0, _assess_def = 0, _assess_eff = 0, _assess_sta = 0;
    int _assess_total = 0, _assess_iq = 0;
    const char* _assess_rank = "-";
    void assessPlayerMove(int r, int c);
    void computeAssessment();
    void drawAssessResult();

    // ---- 作弊模式 / 作弊问责剧情 ----
    bool _cheat_mode = false;         // Y 键开关（仅单人 AI 局）
    int _cheat_alert = 0;             // 0=无 1=冻结中 2=等待 YES/NO
    uint32_t _cheat_freeze_ms = 0;
    int _cheat_sel = 0;               // 0=YES 1=NO
    int _cheat_ask_idx = 0;           // 问责条里随机挑的那句骂
    bool _caught_deny = false;        // 这次质问选了 NO
    bool _cheat_extra_player_move = false; // 玩家版"连走两手"
    void playerCheat(int kind);
    void triggerCheatAlert();
    void resolveCheatChoice();
    void cheatPlayerSteal();
    void cheatPlayerErase();
    void drawCheatDialog();

    // ---- 双人对战 / 限时 / 回放 ----
    bool _pvp = false;           // P 键：双人对战（同机热座，无 AI）
    bool _blitz = false;         // T 键：限时 60 秒，超时判负
    int32_t _clock_ms[3] = { 0, 0, 0 }; // 双方剩余毫秒
    uint32_t _clock_last_ms = 0;
    bool _replay_active = false;        // V 键：棋谱回放
    std::vector<Move> _replay_moves;    // 上局棋谱快照
    int _replay_idx = 0;
    uint32_t _replay_timer = 0;
    uint32_t _r_confirm_ms = 0;  // R 键二次确认（防误触）
    uint32_t _v_debounce_ms = 0; // V 键防抖（狂按回放防闪退）
    uint32_t _ai_cooldown_ms = 0; // AI 冷却, 回放/作弊后等几秒再让 AI 动
    uint8_t _saved_board[BOARD_SIZE][BOARD_SIZE]; // 回放前棋盘快照
    // 回放前完整游戏状态快照 (防止 stopReplay 后状态错乱导致能下对方棋等 bug)
    struct ReplaySnapshot {
        int current_player;
        bool game_over;
        int winner;
        bool result_visible;
        uint32_t result_ms;
        bool win_anim_active;
        bool is_ai_mode;
        bool is_ai_assist_mode;
        bool net_pvp;
        bool long_taunt_done;
    } _replay_snap;

    // ---- 联机对战（C3 + Xbox 手柄）----
    bool _net_pvp = false;         // 手柄在线 = 进入联机对战
    bool _net_started = false;     // 本局已开局（双方都准备好）
    bool _net_ready_local = false; // 键盘方已准备 (Enter)
    bool _net_ready_remote = false;// 手柄方已准备 (A)
    uint16_t _net_btn_prev = 0;
    uint32_t _net_taunt_seen = 0;
    void handleNetPvp(uint32_t now);
    void netStartGame();
    void resetNetReady();
    void netTaunt(bool from_remote = false);

    // ---- 双人对战的作弊辅助(AI 提示该下哪) ----
    bool _net_cheat = false;        // 键盘方(X 键)作弊辅助
    bool _net_cheat_remote = false; // 手柄方(手柄 Y 键)作弊辅助
    bool _taunt_from_remote = false;// 当前嘲讽是手柄方发的(只显示给对方)
    int _hint_r = -1;
    int _hint_c = -1;
    int _hint_mc = -1;
    void toggleNetCheat();
    void computeHint();
    void drawCheatBanner(uint32_t now);
    bool _banner_on = false;
    int _banner_tw = 0;
    uint32_t _banner_ms = 0;

    void endGame(int winner);
    void startReplay();
    void stopReplay();

    void updateMood();
    void drawResult();
    void drawHelp();
    void dumpBoardLog(const char* why);   // 串口打印当前棋盘(作弊改盘后给自动化/取证用)
    void unlockAchv(int bit);
    void reviewPlayerMove(int r, int c);
    bool oppForcedWin(int opp);
    int resolveTauntLines(const char** lines); // 解析当前嘲讽文本行(最多 3 行), 返回行数
    void sendMirrorState();                    // 打包全量状态发给 C3 监视器(GMKSTATE 238B)

    // UI constants (Dynamic calculation in cpp)
    int CELL_SIZE = 9;
    int BOARD_PIXEL_SIZE = 0;
    int START_X = 0;
    int START_Y = 0;

    void resetGame();
    void drawBoard();
    void drawTrend(int x, int y, int w, int h);
    void moveCursor(int dx, int dy);
    void placePiece();
    bool checkWin(int r, int c, int player);
    void aiMove();
    void aiMoveInner();
    void cheatAndRetry();
    void undoMove();

    bool isValid(int r, int c);
    void playAt(int r, int c);
    void showTaunt(int kind);
    void updateDanger();
    void markThreats();
    void drawInfoPanel(int x, int w);
    int scanWinPoints(int player, int outR[], int outC[], int maxOut);
    void rebuildScores();
    void rebuildFromBoard();
    bool noFiveOnBoard();
    bool applyCheat();
    void cheatShuffle();
    void cheatSteal();
    void cheatRelocate();
    void cheatSwap();
    void cheatEraseThreat();
    bool cheatUndoBestPlayerMove();

    // ---- AI core ----
    void calcDelta(int r, int c, int p, int32_t& dp, int32_t& dq);
    void makeMove(int r, int c, int p);
    void unmakeMove(int r, int c, int p);
    int32_t evalFor(int player);
    bool isFive(int r, int c, int p);
    int winPoints(int r, int c, int p, int outR[], int outC[], int maxOut);
    int genMoves(int outR[], int outC[], int maxCount, int player);
    int negamax(int player, int depth, int alpha, int beta, int ply);
    bool searchBest(int depth, int& br, int& bc);
    bool vcf(int player, int depth, int& outR, int& outC);
    int vcfDistance(int player, int maxDepth);
    bool findWinPoint(int player, int& r, int& c);
    int countWinPoints(int player, int& r, int& c);
    /* 双四/活四杀点: 落在这里能同时造出 >=2 个成五点(对手只能挡一个, 必杀)。
     * 传自己 = 找杀棋; 传对手 = 提前占掉他的杀点。仅十星使用。 */
    bool decisivePoint(int p, int& r, int& c);
    bool openingBook(int& r, int& c);
    void heuristicMove(int lvl, int& br, int& bc);
    void recordTrend();
    int heuristicPoint(int r, int c, int player);

public:
    AppGomoku();
    ~AppGomoku();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

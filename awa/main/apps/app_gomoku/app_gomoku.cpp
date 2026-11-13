#include "app_gomoku.h"
#include <apps/utils/glink/glink.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/icons/default_icons.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <functional>

int AppGomoku::s_player_wins = 0;
int AppGomoku::s_ai_wins = 0;
int AppGomoku::s_streak = 0;
int AppGomoku::s_best_streak = 0;

// ================= 键盘跨任务隔离 =================
// 键盘回调由 desktop_task(HAL update) 触发, 而游戏状态/画布只允许 app_task(onRunning) 碰。
// 以前回调里直接 drawBoard()/改 _board, 与 app_task 并发写同一个 sprite 和 SPI 总线:
// 连按回放键/方向键就会花屏、颜色错乱、SPI 卡死(画面冻结且不再响应)。
// 现在回调只入队, 统一由 processPendingKeys() 在 app_task 里执行。
#define KEY_QN 16
static std::function<void(const Keyboard::KeyEvent_t&)> s_key_handler;
static Keyboard::KeyEvent_t s_keyq[KEY_QN];
static volatile int s_key_head = 0;
static volatile int s_key_tail = 0;
static portMUX_TYPE s_key_mux = portMUX_INITIALIZER_UNLOCKED;

// ================= 基础常量 =================

// 一个 5 连窗口里，己方子数（对方 0 子）对应的价值
static const int32_t kWin5[6] = { 0, 2, 26, 300, 6500, 1000000 };
static const int kDR[4] = { 0, 1, 1, 1 };
static const int kDC[4] = { 1, 0, 1, -1 };
static const int WIN_SCORE = 2000000;

// ---- 堆完整性探针: 把"静默越界写"锁到具体阶段 ----
static void heapProbe(const char* stage) {
    if (heap_caps_check_integrity_all(false)) return;
    ESP_LOGE("GOMOKU", "HEAP CORRUPT at [%s] heap=%u largest=%u", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
// 未订阅 TWDT 的任务直接 reset 会刷 "task not found", 先查状态
static inline void feed_wdt() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}
static const int INF_SCORE = 4000000;

// 注意: 必须用 int64_t。esp_timer_get_time() 返回 64 位微秒,
// 之前强转 int32_t 会在开机约 35.8 分钟后溢出成负数,
// 导致所有 deadline 判断失效(AI 不下棋 / 搜索失控)
static inline int64_t now_us() { return esp_timer_get_time(); }

// ================= 嘲讽台词 =================
// 每行最多 5 个汉字（右侧栏只有 54px）

// 每行最多 4 个汉字（右侧栏 64px，14px 字体刚好放得下）

static const char* kTauntNormal[][2] = {
    { "就这水平", "还想赢我" },
    { "算力为零", "回家种地" },
    { "这步真臭", "屎一样臭" },
    { "闭着眼下", "也能赢你" },
    { "别挣扎了", "丢人现眼" },
    { "菜就多练", "嘴还挺硬" },
    { "让你三子", "照样暴打" },
    { "手抖了吧", "稳住啊" },
    { "菜得抠脚", "纯废物" },
    { "脑干缺失", "可怜虫" },
    { "你在思考", "还是在睡" },
    { "下得挺好", "可惜没用" },
    { "又是这招", "早看穿了" },
    { "有进步啊", "从零到一" },
    { "手速挺快", "脑子太慢" },
    { "我看你是", "不想赢了" },
    { "这棋下得", "像在送礼" },
    { "慢慢想吧", "反正都输" },
    { "你的棋路", "一眼看穿" },
    { "再练十年", "再来找我" },
    { "输不起就", "别玩了" },
    { "真可怜啊", "输成这样" },
    { "这也能错", "天才啊" },
    { "我都懒得", "认真下了" },
    { "你是在", "帮我下吗" },
    { "幼儿园", "都比你强" },
    { "认输吧", "省点时间" },
    { "别看了", "你没机会" },
    { "我让你", "悔棋三次" },
    { "这就是", "你的全力" },
    { "瞎下一通", "还挺自信" },
    { "想赢我", "下辈子吧" },
    { "狗屎运都", "救不了你" },
    { "臭棋篓子", "实至名归" },
    { "你这脑子", "捐了吧" },
    { "对面是人", "还是猪啊" },
    { "一坨烂棋", "惨不忍睹" },
    { "垃圾操作", "辣眼睛" },
    { "你下棋像", "在拉屎" },
    { "滚回老家", "玩泥巴" },
    { "智商欠费", "请充值" },
    { "这水平", "敢开机？" },
    { "废物点心", "不值一提" },
    { "键盘烫手", "不会下吧" },
    { "我在放水", "你都输" },
    { "棋力检测", "结果为零" },
    { "建议改行", "斗地主" },
    { "你的胜率", "是负数" },
    { "别怪键盘", "怪脑子" },
    { "我在梦游", "都赢你" },
    { "让你十子", "也白搭" },
    { "心平气和", "虐菜中" },
    { "你走这？", "笑死" },
    { "投降按钮", "在R键" },
    { "我脚指头", "比你强" },
    { "显示器前", "是条狗" },
    { "难绷", "真难绷" },
    { "破防了吧", "哈哈哈" },
    { "建议卸载", "这游戏" },
    { "你在下", "五子棋吗" },
    { "我以为你", "很强呢" },
    { "就这？", "就这？？" },
    { "棋都下歪", "眼神不行" },
    { "你这一手", "载入屎册" },
    { "我奶奶来", "都赢了" },
    { "赶紧认输", "省电费" },
};

static const char* kTauntLong[][2] = {
    { "想这么久", "也没用" },
    { "卡住了吧", "脑子不动" },
    { "要不要我", "提示一下" },
    { "算不出来", "就认输吧" },
    { "天荒地老", "也没用" },
    { "想清楚再", "落子啊" },
    { "你思考的", "时间够了" },
    { "再想也", "是输" },
    { "慢慢算", "我等你" },
    { "想到了吗", "该你了" },
};

// 情绪台词：急了 / 浪了
static const char* kTauntAngry[][2] = {
    { "你怎么敢", "这么下" },
    { "给我等着", "认真了" },
    { "逼我开挂", "是吧" },
    { "这不可能", "再来一次" },
    { "你是不是", "在作弊" },
    { "气死我了", "看招吧" },
    { "信不信我", "弄死你" },
    { "惹毛我了", "完蛋了" },
    { "你死定了", "这盘" },
    { "妈的", "跟我斗" },
};

static const char* kTauntLazy[][2] = {
    { "随便下下", "也够用" },
    { "让你一步", "都能赢" },
    { "我已经", "看到结局" },
    { "不急着赢", "慢慢玩" },
    { "陪你玩玩", "别急啊" },
    { "这局我", "闭眼赢" },
    { "虐菜真", "没挑战" },
    { "赢你都", "不带喘" },
    { "你慢慢", "挣扎吧" },
    { "逗你玩呢", "别当真" },
};

// AI 思考时的碎碎念
static const char* kTauntThink[][2] = {
    { "让我想想", "下哪好呢" },
    { "嗯……", "有点意思" },
    { "别急", "我在算" },
    { "这步棋", "得好好想" },
    { "你这步", "有点东西" },
    { "等一下", "快算完了" },
    { "我看看", "哪里最疼" },
    { "思考中", "别催我" },
};

static const int kTauntAngryN = (int)(sizeof(kTauntAngry) / sizeof(kTauntAngry[0]));
static const int kTauntThinkN = (int)(sizeof(kTauntThink) / sizeof(kTauntThink[0]));

// AI 性格专属口头禅：0=稳健 1=凶狠 2=诡诈
static const char* kTauntPersona[3][4][2] = {
    { { "稳一点", "别急" }, { "按部就班", "你就输了" }, { "不急不躁", "慢慢磨" }, { "这叫", "基本功" } },
    { { "撕碎你", "别哭" }, { "我要", "吃人了" }, { "血都热了", "来啊" }, { "这盘", "必须赢" } },
    { { "你猜我", "下哪" }, { "嘿嘿", "有意思" }, { "规则", "是活的" }, { "别信", "你的眼睛" } },
};

static const char* kPersonaName[3] = { "稳健", "凶狠", "诡诈" };
static const uint16_t kPersonaColor[3] = { TFT_GREEN, TFT_RED, TFT_MAGENTA };

static const int kTauntLazyN = (int)(sizeof(kTauntLazy) / sizeof(kTauntLazy[0]));

// 作弊台词按作弊种类区分，让每一招都看得出来
// 0=打乱棋盘 1=偷子 2=挪子 3=连走两手 4=悔棋重下 5=换子 6=吃子
static const char* kTauntCheat[7][2] = {
    { "棋盘重洗", "重新来过" },
    { "你的子",   "归我了" },
    { "这颗子",   "太碍眼" },
    { "我再下一", "手怎么了" },
    { "刚才那手", "不算数" },
    { "咱俩换换", "位置" },
    { "这颗子",   "我吃了" },
};

// 作弊被抓包：选 YES（承认）/ 选 NO（嘴硬）两套台词
static const char* kTauntCaughtYes[][2] = {
    { "算你老实", "下不为例" },
    { "哦？手滑", "是吧" },
    { "承认就好", "脸呢" },
    { "行吧", "继续挨打" },
    { "这点出息", "还作弊" },
    { "勇气可嘉", "棋力可悲" },
    { "知道了", "当我没看见" },
    { "嘴挺甜", "棋挺臭" },
};
static const char* kTauntCaughtNo[][2] = {
    { "嘴硬？", "那就别怪我" },
    { "不承认？", "我认真了" },
    { "行啊", "十倍奉还" },
    { "当我瞎？", "看招" },
    { "死鸭子", "嘴硬" },
    { "很好", "你完了" },
    { "骗我？", "找死" },
    { "装是吧", "我成全你" },
};
static const int kTauntCaughtYesN = (int)(sizeof(kTauntCaughtYes) / sizeof(kTauntCaughtYes[0]));
static const int kTauntCaughtNoN  = (int)(sizeof(kTauntCaughtNo) / sizeof(kTauntCaughtNo[0]));

static const char* kTauntWin[][3] = {
    { "人类就这", "棋力堪忧", "别玩了" },
    { "算力碾压", "差距太大", "滚去练" },
    { "早看穿了", "还想赢？", "做梦吧" },
    { "废物一个", "回家练吧", "十年后" },
    { "再来十局", "结果一样", "输麻了" },
    { "就这水平", "也敢来", "挑战我" },
    { "输得漂亮", "下次别来", "丢人" },
    { "别哭了", "擦擦眼泪", "回家吧" },
};

static const int kTauntNormalN = (int)(sizeof(kTauntNormal) / sizeof(kTauntNormal[0]));
static const int kTauntLongN = (int)(sizeof(kTauntLong) / sizeof(kTauntLong[0]));
static const int kTauntWinN = (int)(sizeof(kTauntWin) / sizeof(kTauntWin[0]));

// ================= 生命周期 =================

AppGomoku::AppGomoku() {
    setAppInfo().name = "Gomoku";
    setAppInfo().icon = nullptr;
}

AppGomoku::~AppGomoku() {
    _canvas = nullptr;
}

void AppGomoku::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Application opened");

    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    _canvas->deleteSprite();

    _canvas->setColorDepth(8);
    _canvas->setPsram(false);

    // 本机无 PSRAM: createSprite 走 DMA 内存, 整屏 8bpp(240x135=32.4KB) 常拿不到连续块,
    // 会降级成 240x109, 底部 26 行不被 pushSprite 覆盖 → 画面像被"划了一刀"。
    // 这里改用普通内部 RAM 自申请整屏缓冲, 用 setBuffer 绑定, 保证全高。
    int panel_w = GetHAL().display().width();
    int panel_h = GetHAL().display().height();
    if (!_fb) {
        _fb = (uint8_t*)heap_caps_malloc((size_t)panel_w * panel_h, MALLOC_CAP_8BIT);
    }
    if (_fb) {
        _canvas->setBuffer(_fb, panel_w, panel_h);
        ESP_LOGI("GOMOKU", "canvas full %dx%d fb=%p heap=%u",
                 panel_w, panel_h, _fb, (unsigned)esp_get_free_heap_size());
    } else {
        mclog::tagError(getAppInfo().name, "Failed to create sprite, fallback");
        _canvas->createSprite(panel_w, 109);
        ESP_LOGW("GOMOKU", "canvas fallback %dx%d",
                 (int)_canvas->width(), (int)_canvas->height());
    }

    int screen_h = _canvas->height();
    int screen_w = _canvas->width();

    CELL_SIZE = (screen_h - 6) / BOARD_SIZE;
    if (CELL_SIZE < 4) CELL_SIZE = 4;
    if (CELL_SIZE > 8) CELL_SIZE = 8;

    BOARD_PIXEL_SIZE = BOARD_SIZE * CELL_SIZE;
    START_X = (screen_w - BOARD_PIXEL_SIZE) / 2;
    // 棋盘整体上移 7px, 给底部那条作弊滚动横幅留出 13px, 横幅不压棋盘
    START_Y = (screen_h - BOARD_PIXEL_SIZE) / 2 - 7;
    if (START_Y < 0) START_Y = 0;

    resetGame();
    glink_acquire("Gomoku", GLINK_GID_GOMOKU, GLINK_ICON_GOMOKU); // ESP-NOW 主机:开始广播等 C3
    heapProbe("open");

    _handle_key_event_slot_id = -1;
    _ui_closing = false;
    s_key_handler = [this](const Keyboard::KeyEvent_t& keyEvent) {
        heapProbe("key");
        if (!keyEvent.isModifier && keyEvent.state) {
            // 返回桌面: 绝对最优先, 不被帮助/回放/作弊冻结/答题/结算任何状态吞掉
            if (keyEvent.keyCode == KEY_ESC || keyEvent.keyCode == KEY_BACKSPACE) {
                ESP_LOGI("GOMOKU", "back key kb=0x%02x -> close", keyEvent.keyCode);
                _ui_closing = true;
                _close_req_ms = GetHAL().millis();
                close();
                return;
            }

            // 帮助界面打开时，任意键关闭
            if (_help_visible) {
                _help_visible = false;
                drawBoard();
                return;
            }

            // 回放中任意键退出回放 (V 键除外, V 键走自己的 toggle)
            if (_replay_active && keyEvent.keyCode != KEY_V) {
                stopReplay();
                return;
            }

            // ---- 作弊问责: 冻结/答题期间吞掉一切按键, 防止误落子 ----
            if (_cheat_alert) {
                if (_cheat_alert == 2) {
                    if (keyEvent.keyCode == KEY_LEFT || keyEvent.keyCode == KEY_A ||
                        keyEvent.keyCode == KEY_COMMA) {
                        _cheat_sel = 0;
                        drawBoard();
                    } else if (keyEvent.keyCode == KEY_RIGHT || keyEvent.keyCode == KEY_D ||
                               keyEvent.keyCode == KEY_SEMICOLON) {
                        _cheat_sel = 1;
                        drawBoard();
                    } else if (keyEvent.keyCode == KEY_ENTER) {
                        resolveCheatChoice();
                    }
                }
                return;
            }

            // 联机对战(手柄在线): 只放开必要按键
            if (_net_pvp) {
                if (!_net_started || _game_over) {
                    // 还没开局: Enter = 准备/取消准备
                    if (keyEvent.keyCode == KEY_ENTER) {
                        _net_ready_local = !_net_ready_local;
                        drawBoard();
                    } else if (keyEvent.keyCode == KEY_C) {
                        netTaunt();
                    }
                    return;
                }
                // 对局中禁掉会重置局面的设置键
                if (keyEvent.keyCode == KEY_R || keyEvent.keyCode == KEY_P ||
                    keyEvent.keyCode == KEY_M || keyEvent.keyCode == KEY_L ||
                    keyEvent.keyCode == KEY_T) return;
                if (_current_player != 1 && keyEvent.keyCode != KEY_C) return; // 轮到对方
            }

            if (keyEvent.keyCode == KEY_H) {
                _help_visible = true;
                drawBoard();
                return;
            }

            // I 键：智力测试开关（开启后作弊键全部失效）
            if (keyEvent.keyCode == KEY_I) {
                _assess_mode = !_assess_mode;
                if (_assess_mode) _cheat_mode = false;
                _assess_banner = _assess_mode;
                _assess_banner_ms = GetHAL().millis();
                resetGame();
                drawBoard();
                ESP_LOGI("GOMOKU", "assess mode=%d (cheat keys %s)", (int)_assess_mode,
                         _assess_mode ? "disabled" : "enabled");
                return;
            }

            // Y 键：作弊模式开关（仅单人 AI 局；智力测试里按了不给开）
            if (keyEvent.keyCode == KEY_Y) {
                if (_assess_mode) {
                    _assess_banner = true;
                    _assess_banner_ms = GetHAL().millis();
                    drawBoard();
                    return;
                }
                if (_pvp || _is_ai_assist_mode) return;
                _cheat_mode = !_cheat_mode;
                drawBoard();
                ESP_LOGI("GOMOKU", "cheat mode=%d", (int)_cheat_mode);
                return;
            }

            // 1/2/3 键：玩家作弊（需先开作弊模式）
            if (keyEvent.keyCode == KEY_1 || keyEvent.keyCode == KEY_2 ||
                keyEvent.keyCode == KEY_3) {
                if (_assess_mode) {
                    // 智力测试: 明确提示作弊键已禁用
                    _assess_banner = true;
                    _assess_banner_ms = GetHAL().millis();
                    drawBoard();
                    return;
                }
                if (!_cheat_mode || _pvp || _is_ai_assist_mode) return;
                if (_game_over || _win_anim_active) return;
                if (_current_player != 1 || (int)_move_history.size() < 2) return;
                playerCheat(keyEvent.keyCode == KEY_1 ? 1
                            : keyEvent.keyCode == KEY_2 ? 2 : 3);
                return;
            }

            if (keyEvent.keyCode == KEY_R) {
                // 双击确认重开，防误触
                uint32_t nowr = GetHAL().millis();
                if (_r_confirm_ms && nowr - _r_confirm_ms < 2000) {
                    _r_confirm_ms = 0;
                    resetGame();
                } else {
                    _r_confirm_ms = nowr;
                }
                drawBoard();
                return;
            }

            if (keyEvent.keyCode == KEY_M) {
                _is_ai_assist_mode = !_is_ai_assist_mode;
                if (_is_ai_assist_mode) _pvp = false;
                resetGame();
                drawBoard();
                return;
            }

            // P 键：双人对战（同机热座）
            if (keyEvent.keyCode == KEY_P) {
                _pvp = !_pvp;
                if (_pvp) _is_ai_assist_mode = false;
                _is_ai_mode = !_pvp;
                resetGame();
                drawBoard();
                return;
            }

            // T 键：限时 60 秒开关
            if (keyEvent.keyCode == KEY_T) {
                _blitz = !_blitz;
                resetGame();
                drawBoard();
                return;
            }

            // V 键：回放中按 V = 停止回放; 非回放时按 V = 开始回放
            if (keyEvent.keyCode == KEY_V) {
                uint32_t nowv = GetHAL().millis();
                if (nowv - _v_debounce_ms < 300) return;
                _v_debounce_ms = nowv;
                if (_replay_active) {
                    stopReplay();     // 回放中按 V → 停止回放 (恢复原棋盘)
                } else {
                    startReplay();    // 否则 → 开始回放
                }
                return;
            }

            // 难度 1..10 循环，换难度直接开新局
            if (keyEvent.keyCode == KEY_L) {
                _difficulty = (_difficulty % 10) + 1;
                resetGame();
                drawBoard();
                return;
            }

            // 右侧信息面板开关
            if (keyEvent.keyCode == KEY_G) {
                _show_panel = !_show_panel;
                drawBoard();
                return;
            }

            // 威胁点标记开关（默认关闭，X 键切换，切换时重算全盘威胁）
            if (keyEvent.keyCode == KEY_X) {
                if (_pvp) { toggleNetCheat(); return; }
                _show_threat = !_show_threat;
                if (_show_threat) markThreats();
                drawBoard();
                return;
            }

            // AI 性格切换：稳健 / 凶狠 / 诡诈
            if (keyEvent.keyCode == KEY_K) {
                _persona = (_persona + 1) % 3;
                drawBoard();
                return;
            }

            if (keyEvent.keyCode == KEY_U) {
                undoMove();
                return;
            }

            // C 键：对键盘方自己发一条嘲讽(联机时 C3 也会显示)
            if (keyEvent.keyCode == KEY_C) {
                if (_net_pvp) netTaunt();
                else showTaunt(0);
                return;
            }

            if (_game_over || _win_anim_active) return;

            // 方向键和回车: 只有玩家回合且 AI 没在思考时才处理
            bool can_player_act = !(_is_ai_mode && _current_player != 1) && !_ai_thinking;

            if (can_player_act && (keyEvent.keyCode == KEY_E || keyEvent.keyCode == KEY_UP || keyEvent.keyCode == KEY_DOT)) moveCursor(0, -1);
            else if (can_player_act && (keyEvent.keyCode == KEY_S || keyEvent.keyCode == KEY_DOWN || keyEvent.keyCode == KEY_SLASH)) moveCursor(0, 1);
            else if (can_player_act && (keyEvent.keyCode == KEY_A || keyEvent.keyCode == KEY_LEFT || keyEvent.keyCode == KEY_COMMA)) moveCursor(-1, 0);
            else if (can_player_act && (keyEvent.keyCode == KEY_D || keyEvent.keyCode == KEY_RIGHT || keyEvent.keyCode == KEY_SEMICOLON)) moveCursor(1, 0);
            else if (can_player_act && keyEvent.keyCode == KEY_ENTER) placePiece();
            // 其他键 (R/V/L/T/H/ESC 等) 任何时候都放行
        }
    };

    // 真正的键盘回调只负责入队 (desktop_task 上下文, 不碰画布/棋盘)
    s_key_head = s_key_tail = 0;
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([](const Keyboard::KeyEvent_t& e) {
        portENTER_CRITICAL(&s_key_mux);
        int nt = (s_key_tail + 1) % KEY_QN;
        if (nt != s_key_head) {          // 满了就丢最旧的节奏, 绝不阻塞输入任务
            s_keyq[s_key_tail] = e;
            s_key_tail = nt;
        }
        portEXIT_CRITICAL(&s_key_mux);
    });

    drawBoard();
}

void AppGomoku::processPendingKeys() {
    for (;;) {
        Keyboard::KeyEvent_t e;
        portENTER_CRITICAL(&s_key_mux);
        if (s_key_head == s_key_tail) {
            portEXIT_CRITICAL(&s_key_mux);
            break;
        }
        e = s_keyq[s_key_head];
        s_key_head = (s_key_head + 1) % KEY_QN;
        portEXIT_CRITICAL(&s_key_mux);
        if (s_key_handler) s_key_handler(e);
    }
}

void AppGomoku::onRunning() {
    uint32_t now = GetHAL().millis();

    // 返回桌面按键(G0 物理键): 永远第一优先, 不受冻结/答题/结算/帮助等任何状态影响
    if (GetHAL().goButton().wasClicked()) {
        ESP_LOGI("GOMOKU", "back key G0 -> close");
        _ui_closing = true;
        _close_req_ms = now;
        close();
        return;
    }

    // 先把 desktop_task 入队的按键拿到 app_task 里执行: 棋盘修改/绘制只允许单任务
    processPendingKeys();

    // close 请求如果没生效(状态机被改回去), 1.5s 后再提一次, 保证不会卡在原界面
    if (_ui_closing) {
        if (now - _close_req_ms > 1500) {
            _close_req_ms = now;
            close();
        }
        return;
    }

    glink_poll(); // ESP-NOW 信标/心跳/补发
    {
        static bool s_prev_link = false;
        bool lk = glink_is_linked();
        if (lk && !s_prev_link) drawBoard(); // 新连接立即推一帧
        s_prev_link = lk;
    }

    // ---- 胜负动画 ----
    if (_win_anim_active) {
        uint32_t el = GetHAL().millis() - _win_anim_start;
        if (el >= 1800) {
            _win_anim_active = false;
            if (_level_up_pending) {
                _level_up_pending = false;
                if (_difficulty < 10) _difficulty++;
                resetGame();
            }
            drawBoard();
        } else if ((el / 120) % 2 == 0 || el > 600) {
            drawBoard();
        }
        return;
    }

    // ---- 作弊问责: 冻结 1.5 秒后弹出选择题 ----
    if (_cheat_alert == 1 && now - _cheat_freeze_ms >= 1500) {
        _cheat_alert = 2;
        drawBoard();
    }

    // 智力测试提示 3.5 秒后擦掉（重画一帧, 不留残影）
    if (_assess_banner && now - _assess_banner_ms >= 3500) {
        _assess_banner = false;
        drawBoard();
    }

    // 联机对战: 手柄上线/掉线、准备确认、按键落子
    handleNetPvp(now);

    // 镜像心跳: 画面没变化也每秒推一次全量状态, 监视器不会停在旧画面
    {
        static uint32_t s_mirror_ping = 0;
        if (glink_is_linked() && now - s_mirror_ping > 1000) {
            s_mirror_ping = now;
            sendMirrorState();
        }
    }

    // 平局:显示 DRAW 1.8 秒后自动开新局(联机对战要双方确认, 不自动开)
    // 智力测试下平局也停在结算面板, 否则玩家看不到评估结果
    if (_game_over && _winner == 0 && _result_ms > 0 && now - _result_ms > 1800 && !_net_pvp &&
        !_assess_mode) {
        resetGame();
        return;
    }

    // ---- 棋谱回放：每 350ms 摆一手 ----
    if (_replay_active) {
        if (now - _replay_timer > 350) {
            _replay_timer = now;
            if (_replay_idx < (int)_replay_moves.size()) {
                Move& m = _replay_moves[_replay_idx++];
                // 越界保护: 防止坏数据写爆内存
                if (m.r >= 0 && m.r < BOARD_SIZE && m.c >= 0 && m.c < BOARD_SIZE) {
                    _board[m.r][m.c] = (uint8_t)m.player;
                }
                audio::play_random_tone();
                drawBoard();
            } else {
                // 回放完了自动恢复原棋盘
                stopReplay();
            }
        }
        return;
    }

    // ---- 限时钟：当前行棋方扣时间，超时判负（问责冻结期间停表）----
    if (_blitz && !_game_over && !_win_anim_active && !_cheat_alert) {
        int32_t dt = (int32_t)(now - _clock_last_ms);
        _clock_last_ms = now;
        if (dt > 0 && dt < 5000) {
            _clock_ms[_current_player] -= dt;
            if (_clock_ms[_current_player] <= 0) {
                _clock_ms[_current_player] = 0;
                endGame(3 - _current_player);
            }
        }
        // 每秒刷一次钟面
        static uint32_t s_clk_draw = 0;
        if (now - s_clk_draw > 500) {
            s_clk_draw = now;
            drawBoard();
        }
    } else {
        _clock_last_ms = now;
    }

    // 结束后 2.2 秒弹出战报
    if (_game_over && !_result_visible && _result_ms > 0 && now - _result_ms > 2200) {
        _result_visible = true;
        drawBoard();
    }

    // 普通嘲讽自动消失
    if (_taunt_visible && _taunt_kind != 3 && now - _taunt_timer > 2900) {
        _taunt_visible = false;
        drawBoard();
    }

    // R 重开确认提示超时消失
    if (_r_confirm_ms && now - _r_confirm_ms > 2000) {
        _r_confirm_ms = 0;
        drawBoard();
    }

    // 对手长考：超过 7 秒来一句
    if (!_pvp && !_game_over && !_is_ai_assist_mode && _current_player == 1 && !_long_taunt_done) {
        if (now - _turn_start_ms > 7000) {
            _long_taunt_done = true;
            showTaunt(1);
        }
    }

    // 平时随机来一句（骂得勤一点）
    if (!_pvp && !_game_over && !_is_ai_assist_mode && _current_player == 1 && !_taunt_visible) {
        if (now - _taunt_timer > 3800) {
            _taunt_timer = now;
            if ((esp_random() % 100) < 78) showTaunt(0);
        }
    }

    // ---- AI 行动 ----
    if (!_game_over && _current_player == 2 && _is_ai_mode && !_cheat_alert) {
        // AI 冷却 (回放/作弊后等几秒再动)
        if (_ai_cooldown_ms && now < _ai_cooldown_ms) { _ai_move_timer = 0; return; }

        if (_ai_move_timer == 0) _ai_move_timer = now;

        int wait = (_difficulty >= 7) ? 700 : 450;
        if (now - _ai_move_timer > (uint32_t)wait) {
            _ai_move_timer = 0;
            aiMove();
        }
    } else {
        _ai_move_timer = 0;
    }
}

void AppGomoku::onClose() {
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    }

    glink_release(); // 不反初始化: 链接留给 launcher/下个 app 继续用

    s_key_handler = nullptr;
    s_key_head = s_key_tail = 0;
    _canvas = &GetHAL().canvas();
    _canvas->deleteSprite(); // 先解绑(sprite 对 _fb 只是引用, 不会自己 free)
    if (_fb) {
        free(_fb);
        _fb = nullptr;
    }
    _canvas->setColorDepth(16);
    _canvas->createSprite(GetHAL().display().width(), 109);

    _canvas = nullptr;
    ESP_LOGI("GOMOKU", "onClose done heap=%u", (unsigned)esp_get_free_heap_size());
}

void AppGomoku::resetGame() {
    memset(_board, 0, sizeof(_board));
    memset(_near, 0, sizeof(_near));
    memset(_forbid, 0, sizeof(_forbid));
    memset(_trend, 0, sizeof(_trend));
    memset(_threat, 0, sizeof(_threat));
    _score[0] = _score[1] = _score[2] = 0;
    _trend_head = 0;
    _trend_count = 0;

    _cursor_x = 7;
    _cursor_y = 7;
    _current_player = 1;
    _game_over = false;
    _winner = 0;
    _move_history.clear();

    _taunt_visible = false;
    _taunt_kind = 0;
    _taunt_timer = GetHAL().millis();
    _win_anim_active = false;
    _level_up_pending = false;

    _danger_moves = 0;
    _cheat_depth = 0;
    _cheat_count = 0;
    _cheat_kind_last = -1;
    _cheat_extra_move = false;
    _ai_move_timer = 0;
    _ai_thinking = false;
    _ai_cooldown_ms = 0;
    _turn_start_ms = GetHAL().millis();
    _long_taunt_done = false;

    _mood = 0;
    _min_danger = 999;
    _game_start_ms = GetHAL().millis();
    _result_visible = false;
    _result_ms = 0;
    _achv_new = 0;
    _help_visible = false;
    _iter_best_r = _iter_best_c = -1;
    _blunders = 0;
    _worst_loss = 0;
    _worst_move_no = 0;

    // 限时钟 / 回放
    _clock_ms[1] = _clock_ms[2] = 60 * 1000;
    _clock_last_ms = GetHAL().millis();
    _replay_active = false;
    _replay_idx = 0;
    _r_confirm_ms = 0;

    // 智力测试: 每局重新采样（模式开关本身保留）
    _assess_moves = 0;
    _assess_regret_sum = 0;
    _assess_regret_max = 0;
    _assess_def_ok = 0;
    _assess_def_need = 0;
    _assess_make4 = 0;
    _assess_think_sum = 0;
    _assess_think_max = 0;
    _assess_swing = 0;
    _assess_last_diff = 0;
    _assess_atk = _assess_def = _assess_eff = _assess_sta = 0;
    _assess_total = _assess_iq = 0;
    _assess_rank = "-";

    // 作弊问责: 新局不再处于冻结/答题状态
    _cheat_alert = 0;
    _cheat_sel = 0;
    _caught_deny = false;
    _cheat_extra_player_move = false;
    _cheat_freeze_ms = 0;

    recordTrend();
}

void AppGomoku::undoMove() {
    if (_move_history.empty()) return;

    int steps = (_is_ai_mode && !_game_over && _current_player == 1) ? 2 : 1;

    if (_game_over) {
        steps = 1;
        _game_over = false;
        _winner = 0;
    }

    while (steps > 0 && !_move_history.empty()) {
        Move last = _move_history.back();
        _move_history.pop_back();
        unmakeMove(last.r, last.c, last.player);
        _current_player = last.player;
        steps--;
    }

    _danger_moves = 0;
    _turn_start_ms = GetHAL().millis();
    _long_taunt_done = false;
    recordTrend();
    markThreats();
    drawBoard();
}

// ================= 联机对战(手柄方 = 白棋/玩家2) =================

void AppGomoku::resetNetReady() {
    _net_ready_local = false;
    _net_ready_remote = false;
}

void AppGomoku::netStartGame() {
    resetGame();
    _pvp = true;
    _is_ai_mode = false;
    _is_ai_assist_mode = false;
    _current_player = (esp_random() & 1) ? 1 : 2;  // 随机先手
    _net_started = true;
    resetNetReady();
    _hint_mc = -1;
    _hint_r = _hint_c = -1;
    _turn_start_ms = GetHAL().millis();
    drawBoard();
    ESP_LOGI("gomoku", "net pvp start, first=%d", _current_player);
}

void AppGomoku::handleNetPvp(uint32_t now) {
    (void)now;
    bool conn = (glink_remote_flags() & 0x01) != 0;

    if (conn && !_net_pvp) {
        // 手柄上线: 进联机对战, 等双方准备
        _net_pvp = true;
        _pvp = true;
        _is_ai_mode = false;
        _is_ai_assist_mode = false;
        _net_started = false;
        resetNetReady();
        _net_taunt_seen = glink_taunt_seq();
        _net_btn_prev = glink_buttons();
        resetGame();
        drawBoard();
        ESP_LOGI("gomoku", "net pvp on");
    } else if (!conn && _net_pvp) {
        // 手柄掉线: 退出联机, 回 AI 模式
        _net_pvp = false;
        _net_started = false;
        resetNetReady();
        _pvp = false;
        _is_ai_mode = true;
        resetGame();
        drawBoard();
        ESP_LOGI("gomoku", "net pvp off");
        return;
    }
    if (!_net_pvp) return;

    // 对方(手柄)发来的嘲讽: 在自己(键盘方)屏幕上显示出来
    if (glink_taunt_seq() != _net_taunt_seen) {
        _net_taunt_seen = glink_taunt_seq();
        netTaunt(true);
    }

    // 手柄按键: 只处理"刚按下"的位
    uint16_t m = glink_buttons();
    uint16_t pressed = (uint16_t)(m & ~_net_btn_prev);
    _net_btn_prev = m;

    // Y = 手柄方作弊辅助: 开关在 C3 上做, 这里通过 GMKPVP bit1 同步(掉包也能自愈)
    bool cheat_remote = (glink_remote_flags() & 0x02) != 0;
    if (cheat_remote != _net_cheat_remote) {
        _net_cheat_remote = cheat_remote;
        _hint_mc = -1;
        drawBoard();
    }

    // 作弊辅助开着: 轮到谁就重算一次提示
    if ((_net_cheat || _net_cheat_remote) && _net_started && !_game_over) {
        int mc = (int)_move_history.size();
        if (mc != _hint_mc) {
            _hint_mc = mc;
            computeHint();
            drawBoard();
        }
    }

    if (!_net_started || _game_over) {
        if (pressed & 0x10) {   // A = 准备/取消准备
            _net_ready_remote = !_net_ready_remote;
            drawBoard();
        }
    } else if (_current_player == 2) {
        // 方向键: 按下走一格, 长按自动连走
        static uint16_t hold_dir = 0;
        static uint32_t hold_ms = 0;
        static uint32_t rep_ms = 0;
        uint16_t dirs = (uint16_t)(m & 0x000F);

        if (pressed & 0x0001) { moveCursor(0, -1); hold_dir = dirs; hold_ms = now; rep_ms = now; }
        else if (pressed & 0x0002) { moveCursor(0, 1); hold_dir = dirs; hold_ms = now; rep_ms = now; }
        else if (pressed & 0x0004) { moveCursor(-1, 0); hold_dir = dirs; hold_ms = now; rep_ms = now; }
        else if (pressed & 0x0008) { moveCursor(1, 0); hold_dir = dirs; hold_ms = now; rep_ms = now; }
        else if (dirs && dirs == hold_dir && now - hold_ms > 420 && now - rep_ms > 110) {
            rep_ms = now;
            if (dirs & 0x0001) moveCursor(0, -1);
            else if (dirs & 0x0002) moveCursor(0, 1);
            else if (dirs & 0x0004) moveCursor(-1, 0);
            else moveCursor(1, 0);
        }
        if (!dirs) hold_dir = 0;

        if (pressed & 0x0010) placePiece();
        else if (pressed & 0x0020) undoMove();
    }

    // 双方都准备 → 开局
    if ((!_net_started || _game_over) && _net_ready_local && _net_ready_remote) {
        netStartGame();
    }

    if (_net_cheat_remote) {
        // 只重画横幅那一条再推屏, 不重画整个棋盘(整屏重画太慢, 横幅会一卡一卡)
        if (now - _banner_ms > 33) {
            _banner_ms = now;
            drawCheatBanner(now);
            _canvas->pushSprite(0, 0);
            _banner_on = true;
        }
    } else if (_banner_on) {
        _banner_on = false;
        drawBoard();
    }
}

void AppGomoku::moveCursor(int dx, int dy) {
    int nx = _cursor_x + dx;
    int ny = _cursor_y + dy;
    if (isValid(ny, nx)) {
        _cursor_x = nx;
        _cursor_y = ny;
        drawBoard();
    }
}

bool AppGomoku::isValid(int r, int c) {
    return r >= 0 && r < BOARD_SIZE && c >= 0 && c < BOARD_SIZE;
}

void AppGomoku::playAt(int r, int c) {
    if (!isValid(r, c) || _board[r][c] != 0) return;
    _cursor_x = c;
    _cursor_y = r;
    placePiece();
}

void AppGomoku::placePiece() {
    // 作弊问责冻结/答题期间: 任何人都不许落子
    if (_cheat_alert) return;
    if (_board[_cursor_y][_cursor_x] != 0) return;
    // 玩家回合封锁已经在键盘回调里做了, 这里只检查 AI 不能主动 placePiece
    // (AI 走 playAt 直接调 makeMove 相关, 不走这里)

    int p = _current_player;
    if (p == 1 && !_is_ai_assist_mode && !_pvp) {
        // 智力测试用新的"悔手度"口径; 普通模式仍用原来的复盘提示
        if (_assess_mode) assessPlayerMove(_cursor_y, _cursor_x);
        else reviewPlayerMove(_cursor_y, _cursor_x);
    }

    makeMove(_cursor_y, _cursor_x, p);
    _move_history.push_back({ _cursor_y, _cursor_x, p });
    ESP_LOGI("GOMOKU", "move #%d %c%d by P%d heap=%u",
             (int)_move_history.size(), 'A' + _cursor_x, _cursor_y + 1, p,
             (unsigned)esp_get_free_heap_size());

    audio::play_random_tone();
    recordTrend();
    markThreats();
    heapProbe("move");

    if (checkWin(_cursor_y, _cursor_x, p)) {
        endGame(p);
    } else if ((int)_move_history.size() >= BOARD_SIZE * BOARD_SIZE) {
        endGame(0); // 棋盘下满:平局
    } else {
        if (_cheat_extra_move && p == 2) {
            // 作弊：连走两手，回合权仍然留在 AI 这边
            _cheat_extra_move = false;
            _long_taunt_done = false;
        } else if (_cheat_extra_player_move && p == 1) {
            // 玩家作弊：连走两手，回合权仍然留在玩家这边
            _cheat_extra_player_move = false;
            _turn_start_ms = GetHAL().millis();
            _long_taunt_done = false;
        } else {
            _current_player = (p == 1) ? 2 : 1;
            if (_current_player == 1) {
                _turn_start_ms = GetHAL().millis();
                _long_taunt_done = false;
            }
        }
    }

    drawBoard();
}

// ================= 终局 / 回放 =================

void AppGomoku::endGame(int winner) {
    _game_over = true;
    _winner = winner;
    _taunt_visible = false;
    _long_taunt_done = true;
    _result_visible = false;
    _result_ms = GetHAL().millis();
    resetNetReady();   // 联机对战: 下一局要双方重新确认

    // 棋谱快照，供 V 键回放
    _replay_moves = _move_history;

    bool player_win = (winner == 1);
    bool draw_game = (winner == 0);
    bool ranked = !_is_ai_assist_mode && !_pvp; // 辅助/双人局不计战绩

    if (ranked && !draw_game) {
        if (player_win) s_player_wins++;
        else s_ai_wins++;
    }

    if (player_win && ranked) {
        s_streak++;
        if (s_streak > s_best_streak) s_best_streak = s_streak;

        if (s_player_wins == 1) unlockAchv(0);            // 首次胜利
        if (_min_danger <= 3) unlockAchv(1);              // 绝地反杀
        if (_difficulty >= 10) unlockAchv(2);             // 十星屠龙
        if (s_streak >= 3) unlockAchv(3);                 // 三连霸
        if (_cheat_count == 0) unlockAchv(4);             // 零作弊胜
        if (_blunders == 0) unlockAchv(5);                // 全程无失误

        if (_assess_mode) {
            // 智力测试: 不自动升级重开, 停在结算面板让玩家看评分
            _win_anim_active = false;
            _level_up_pending = false;
            audio::play_melody({ 76, 80, 83, 88 }, 0.05);
        } else {
            _win_anim_start = GetHAL().millis();
            _win_anim_active = true;
            _level_up_pending = true;
            audio::play_melody({ 76, 80, 83, 88 }, 0.05);
        }
    } else if (draw_game) {
        audio::play_melody({ 66, 66 }, 0.08);
    } else if (!player_win) {
        if (ranked) {
            s_streak = 0;
            showTaunt(3);
        }
        audio::play_melody({ 55, 52, 48 }, 0.07);
    }

    if (_assess_mode) computeAssessment();
}

void AppGomoku::startReplay() {
    if (_replay_moves.empty()) return;
    // 防呆: 如果已经在回放, 先停掉
    if (_replay_active) stopReplay();
    // 任何时候都能回放上一局（回放只动 _board，结束后恢复原盘面）

    memcpy(_saved_board, _board, sizeof(_board));
    // 同时存完整游戏状态快照
    _replay_snap = { _current_player, _game_over, _winner, _result_visible, _result_ms,
                     _win_anim_active, _is_ai_mode, _is_ai_assist_mode, _net_pvp, _long_taunt_done };

    memset(_board, 0, sizeof(_board));
    _replay_active = true;
    _replay_idx = 0;
    _replay_timer = 0;
    _result_visible = false;
    drawBoard();
}

void AppGomoku::stopReplay() {
    if (!_replay_active) return;
    memcpy(_board, _saved_board, sizeof(_board));
    _replay_active = false;
    // 恢复完整游戏状态快照 (关键! 否则回放后 current_player 等被改乱导致能下对方棋)
    _current_player = _replay_snap.current_player;
    _game_over = _replay_snap.game_over;
    _winner = _replay_snap.winner;
    _result_visible = _replay_snap.result_visible;
    _result_ms = _replay_snap.result_ms;
    _win_anim_active = _replay_snap.win_anim_active;
    _is_ai_mode = _replay_snap.is_ai_mode;
    _is_ai_assist_mode = _replay_snap.is_ai_assist_mode;
    _net_pvp = _replay_snap.net_pvp;
    _long_taunt_done = _replay_snap.long_taunt_done;
    // 如果恢复的是已结束但战报未弹的状态, 回放期间肯定过了 2.2s, 直接显示战报
    if (_game_over && !_result_visible && _result_ms > 0) _result_visible = true;
    // AI 冷却 3 秒, 等玩家确认
    _ai_cooldown_ms = GetHAL().millis() + 3000;
    _ai_move_timer = 0;
    // 不调 markThreats() — winPoints 递归可能爆栈, 威胁标记等下次按 T 再算
    drawBoard();
}

bool AppGomoku::checkWin(int r, int c, int player) {
    for (int i = 0; i < 4; i++) {
        int count = 1;
        for (int k = 1; k < 5; k++) {
            int nr = r + kDR[i] * k;
            int nc = c + kDC[i] * k;
            if (isValid(nr, nc) && _board[nr][nc] == player) count++;
            else break;
        }
        for (int k = 1; k < 5; k++) {
            int nr = r - kDR[i] * k;
            int nc = c - kDC[i] * k;
            if (isValid(nr, nc) && _board[nr][nc] == player) count++;
            else break;
        }
        if (count >= 5) return true;
    }
    return false;
}

// ================= 增量局面分 =================
// 只对"经过落子点"的 5 连窗口更新分数，落一子约 20 个窗口。

void AppGomoku::calcDelta(int r, int c, int p, int32_t& dp, int32_t& dq) {
    int q = 3 - p;
    int32_t ap = 0, aq = 0;

    for (int d = 0; d < 4; d++) {
        const int dr = kDR[d], dc = kDC[d];
        for (int s = -4; s <= 0; s++) {
            int cntP = 0, cntQ = 0;
            bool ok = true;
            for (int k = 0; k < 5; k++) {
                int nr = r + dr * (s + k);
                int nc = c + dc * (s + k);
                if (nr < 0 || nr >= BOARD_SIZE || nc < 0 || nc >= BOARD_SIZE) { ok = false; break; }
                int v = (nr == r && nc == c) ? p : _board[nr][nc];
                if (v == p) cntP++;
                else if (v == q) cntQ++;
            }
            if (!ok) continue;

            // cntP 已经把中心点当作己方子，所以落子前是 cntP-1
            if (cntQ == 0) ap += kWin5[cntP] - kWin5[cntP - 1];
            if (cntP == 1) aq -= kWin5[cntQ];
        }
    }
    dp = ap;
    dq = aq;
}

void AppGomoku::makeMove(int r, int c, int p) {
    int32_t dp, dq;
    calcDelta(r, c, p, dp, dq);
    _board[r][c] = (uint8_t)p;
    _score[p] += dp;
    _score[3 - p] += dq;

    for (int dr = -2; dr <= 2; dr++) {
        for (int dc = -2; dc <= 2; dc++) {
            if (!dr && !dc) continue;
            int nr = r + dr, nc = c + dc;
            if (nr >= 0 && nr < BOARD_SIZE && nc >= 0 && nc < BOARD_SIZE) _near[nr][nc]++;
        }
    }
}

void AppGomoku::unmakeMove(int r, int c, int p) {
    int32_t dp, dq;
    calcDelta(r, c, p, dp, dq);
    _board[r][c] = 0;
    _score[p] -= dp;
    _score[3 - p] -= dq;

    for (int dr = -2; dr <= 2; dr++) {
        for (int dc = -2; dc <= 2; dc++) {
            if (!dr && !dc) continue;
            int nr = r + dr, nc = c + dc;
            if (nr >= 0 && nr < BOARD_SIZE && nc >= 0 && nc < BOARD_SIZE) _near[nr][nc]--;
        }
    }
}

int32_t AppGomoku::evalFor(int player) {
    int32_t a = _score[player];
    int32_t b = _score[3 - player];
    // 性格决定防守权重：稳健 1/2、凶狠 1/8、诡诈 1/4
    int pid = (_persona >= 0 && _persona < 3) ? _persona : 0;
    static const int kDefShift[3] = { 1, 3, 2 };
    return a - b - (b >> kDefShift[pid]);
}

bool AppGomoku::isFive(int r, int c, int p) {
    for (int d = 0; d < 4; d++) {
        int cnt = 1;
        for (int k = 1; k < 5; k++) {
            int nr = r + kDR[d] * k, nc = c + kDC[d] * k;
            if (!isValid(nr, nc) || _board[nr][nc] != p) break;
            cnt++;
        }
        for (int k = 1; k < 5; k++) {
            int nr = r - kDR[d] * k, nc = c - kDC[d] * k;
            if (!isValid(nr, nc) || _board[nr][nc] != p) break;
            cnt++;
        }
        if (cnt >= 5) return true;
    }
    return false;
}

// 假设在 (r,c) 落 p 子：
//   返回 -1 表示直接成五
//   否则返回能成五的空点数量（这些点就是"四"的威胁点）
int AppGomoku::winPoints(int r, int c, int p, int outR[], int outC[], int maxOut) {
    int n = 0;
    for (int d = 0; d < 4; d++) {
        const int dr = kDR[d], dc = kDC[d];
        for (int s = -4; s <= 0; s++) {
            int cntP = 0, er = -1, ec = -1;
            bool ok = true, blocked = false;
            for (int k = 0; k < 5; k++) {
                int nr = r + dr * (s + k);
                int nc = c + dc * (s + k);
                if (nr < 0 || nr >= BOARD_SIZE || nc < 0 || nc >= BOARD_SIZE) { ok = false; break; }
                int v = (nr == r && nc == c) ? p : _board[nr][nc];
                if (v == p) cntP++;
                else if (v == 0) { er = nr; ec = nc; }
                else { blocked = true; break; }
            }
            if (!ok || blocked) continue;
            if (cntP == 5) return -1;
            if (cntP == 4 && er >= 0) {
                bool dup = false;
                for (int i = 0; i < n; i++) {
                    if (outR[i] == er && outC[i] == ec) { dup = true; break; }
                }
                if (!dup && n < maxOut) {
                    outR[n] = er;
                    outC[n] = ec;
                    n++;
                }
            }
        }
    }
    return n;
}

int AppGomoku::genMoves(int outR[], int outC[], int maxCount, int player) {
    // 直接维护"前 maxCount 名"，全盘扫描，绝不因为先收满 64 个而漏掉关键堵点
    static int sc[64];
    if (maxCount > 64) maxCount = 64;
    int m = 0;

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c]) continue;
            if (_near[r][c] == 0) continue;

            int32_t dp = 0, dq = 0;
            calcDelta(r, c, player, dp, dq);
            int s = (int)(dp - dq); // dq 是负数，减掉等于加上封堵价值

            int pos = m;
            while (pos > 0 && sc[pos - 1] < s) pos--;

            if (pos >= maxCount) continue;

            int end = (m < maxCount) ? m : maxCount - 1;
            for (int k = end; k > pos; k--) {
                sc[k] = sc[k - 1];
                outR[k] = outR[k - 1];
                outC[k] = outC[k - 1];
            }
            sc[pos] = s;
            outR[pos] = r;
            outC[pos] = c;
            if (m < maxCount) m++;
        }
    }
    return m;
}

int AppGomoku::negamax(int player, int depth, int alpha, int beta, int ply) {
    if (++_nodes > 80000) { _search_aborted = true; return evalFor(player); }
    if ((_nodes & 15) == 0 && now_us() > _deadline_us) { _search_aborted = true; return evalFor(player); }
    if ((_nodes & 255) == 0) feed_wdt();  // 每 256 节点喂狗, 防 INT WDT
    if (depth <= 0) return evalFor(player);

    int R[16], C[16];
    int n = genMoves(R, C, 16, player);
    if (n <= 0) return evalFor(player);

    int opp = 3 - player;
    int best = -INF_SCORE;
    int used = 0;

    // 根节点：把上一层迭代的最佳点提到最前，提高剪枝效率
    if (ply == 0 && _iter_best_r >= 0) {
        for (int i = 1; i < n; i++) {
            if (R[i] == _iter_best_r && C[i] == _iter_best_c) {
                int tr = R[0], tc = C[0];
                R[0] = R[i]; C[0] = C[i];
                R[i] = tr;   C[i] = tc;
                break;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        int r = R[i], c = C[i];
        if (ply == 0 && _forbid[r][c]) continue;
        used++;

        makeMove(r, c, player);
        int v;
        if (isFive(r, c, player)) v = WIN_SCORE - ply;
        else v = -negamax(opp, depth - 1, -beta, -alpha, ply + 1);
        unmakeMove(r, c, player);

        if (_search_aborted) return best;

        if (v > best) {
            best = v;
            if (ply == 0) { _best_r = r; _best_c = c; }
        }
        if (v > alpha) alpha = v;
        if (alpha >= beta) break;
    }

    if (used == 0) return evalFor(player);
    return best;
}

// 迭代加深：从浅到深搜，把上一层的结果当作下一层的首选点。
// 好处是时间不够时也能拿出"上一层搜完的那一手"，不会一无所获。
bool AppGomoku::searchBest(int depth, int& br, int& bc) {
    _nodes = 0;
    _search_aborted = false;
    _iter_best_r = _iter_best_c = -1;
    br = bc = -1;

    for (int d = 2; d <= depth; d += 2) {
        _best_r = _best_c = -1;
        negamax(2, d, -INF_SCORE, INF_SCORE, 0);

        if (_best_r >= 0) {
            br = _best_r;
            bc = _best_c;
            _iter_best_r = br;
            _iter_best_c = bc;
        }
        if (_search_aborted) break;
        if (br >= 0 && now_us() > _deadline_us) break;
    }

    return br >= 0;
}

// VCF：连续冲四搜索。player 能否靠连续四把对方冲死。
bool AppGomoku::vcf(int player, int depth, int& outR, int& outC) {
    if (depth <= 0) return false;
    if (++_nodes > 50000) return false;
    if ((_nodes & 15) == 0 && now_us() > _deadline_us) return false;
    if ((_nodes & 255) == 0) feed_wdt();

    int R[24], C[24];
    int n = genMoves(R, C, 24, player);
    if (n == 0) return false;

    int opp = 3 - player;

    // 一步成五
    for (int i = 0; i < n; i++) {
        int a[8], b[8];
        if (winPoints(R[i], C[i], player, a, b, 8) == -1) {
            outR = R[i];
            outC = C[i];
            return true;
        }
    }

    for (int i = 0; i < n; i++) {
        int a[8], b[8];
        int k = winPoints(R[i], C[i], player, a, b, 8);
        if (k <= 0) continue;

        // 双四：对手只能挡一个
        if (k >= 2) {
            outR = R[i];
            outC = C[i];
            return true;
        }

        // 单四：对手唯一挡点
        int br = a[0], bc = b[0];
        int x[8], y[8];
        if (winPoints(br, bc, opp, x, y, 8) == -1) continue; // 对手挡的同时成五，冲四失败

        makeMove(R[i], C[i], player);
        makeMove(br, bc, opp);
        bool win = vcf(player, depth - 2, outR, outC);
        unmakeMove(br, bc, opp);
        unmakeMove(R[i], C[i], player);

        if (win) {
            outR = R[i];
            outC = C[i];
            return true;
        }
    }
    return false;
}

int AppGomoku::vcfDistance(int player, int maxDepth) {
    int r, c;
    for (int d = 2; d <= maxDepth; d += 2) {
        _nodes = 0;
        if (vcf(player, d, r, c)) return d / 2;
    }
    return 0;
}

// 全盘扫描所有"落子即五连"的点。不依赖候选表，绝不会漏掉唯一堵点。
int AppGomoku::scanWinPoints(int player, int outR[], int outC[], int maxOut) {
    int n = 0;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c]) continue;
            int a[8], b[8];
            if (winPoints(r, c, player, a, b, 8) == -1) {
                if (n < maxOut) {
                    outR[n] = r;
                    outC[n] = c;
                }
                n++;
            }
        }
    }
    return n;
}

bool AppGomoku::findWinPoint(int player, int& r, int& c) {
    int R[32], C[32];
    if (scanWinPoints(player, R, C, 32) > 0) {
        r = R[0];
        c = C[0];
        return true;
    }
    return false;
}

// 返回对手/自己有多少个"成五点"，并给出最该处理的那个
int AppGomoku::countWinPoints(int player, int& r, int& c) {
    int R[32], C[32];
    int n = scanWinPoints(player, R, C, 32);
    if (n > 0) {
        r = R[0];
        c = C[0];
    } else {
        r = c = -1;
    }
    return n;
}

// ================= 作弊 =================

void AppGomoku::rebuildScores() {
    memset(_score, 0, sizeof(_score));
    memset(_near, 0, sizeof(_near));

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            int p = _board[r][c];
            if (!p) continue;

            int32_t dp = 0, dq = 0;
            calcDelta(r, c, p, dp, dq);
            _score[p] += dp;
            _score[3 - p] += dq;

            for (int dr = -2; dr <= 2; dr++) {
                for (int dc = -2; dc <= 2; dc++) {
                    if (!dr && !dc) continue;
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < BOARD_SIZE && nc >= 0 && nc < BOARD_SIZE) _near[nr][nc]++;
                }
            }
        }
    }
}

void AppGomoku::rebuildFromBoard() {
    uint8_t tmp[BOARD_SIZE][BOARD_SIZE];
    memcpy(tmp, _board, sizeof(_board));
    memset(_board, 0, sizeof(_board));
    memset(_score, 0, sizeof(_score));
    memset(_near, 0, sizeof(_near));
    _move_history.clear();

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (tmp[r][c] == 0) continue;
            makeMove(r, c, tmp[r][c]);
            _move_history.push_back({ r, c, (int)tmp[r][c] });
        }
    }
}

bool AppGomoku::noFiveOnBoard() {
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c] && isFive(r, c, _board[r][c])) return false;
        }
    }
    return true;
}

// 打乱整个棋盘：双方棋子数量不变，位置全部随机重排
void AppGomoku::cheatShuffle() {
    uint8_t backup[BOARD_SIZE][BOARD_SIZE];
    memcpy(backup, _board, sizeof(_board));
    int32_t before = _score[2] - _score[1];
    int before_cheat = _cheat_count;

    uint8_t stones[225];
    int n = 0;
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int c = 0; c < BOARD_SIZE; c++)
            if (_board[r][c]) stones[n++] = _board[r][c];

    if (n < 8) return;

    for (int attempt = 0; attempt < 20; attempt++) {
        memset(_board, 0, sizeof(_board));
        int placed = 0;
        int guard = 0;
        while (placed < n && guard < 20000) {
            int pos = (int)(esp_random() % 225);
            int r = pos / BOARD_SIZE, c = pos % BOARD_SIZE;
            guard++;
            if (_board[r][c]) continue;
            _board[r][c] = stones[placed++];
        }
        if (placed < n) continue;

        rebuildScores();
        if (!noFiveOnBoard()) continue;

        // 不允许重排后直接把玩家送到嘴边（对手有成五点就重来）
        int tr[8], tc[8];
        if (scanWinPoints(1, tr, tc, 8) > 0) continue;

        int32_t after = _score[2] - _score[1];
        if (after > before || attempt >= 19) {
            rebuildFromBoard();
            _cheat_count = before_cheat;
            return;
        }
    }

    memcpy(_board, backup, sizeof(_board));
    rebuildFromBoard();
    _cheat_count = before_cheat;
}

// 偷子：把玩家一颗关键的棋子变成 AI 的
void AppGomoku::cheatSteal() {
    int bestR = -1, bestC = -1, bestS = -1;

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c] != 1) continue;
            int s = heuristicPoint(r, c, 1);
            if (s > bestS) { bestS = s; bestR = r; bestC = c; }
        }
    }
    if (bestR < 0) return;

    uint8_t backup[BOARD_SIZE][BOARD_SIZE];
    memcpy(backup, _board, sizeof(_board));
    _board[bestR][bestC] = 2;

    // 偷完之后棋盘上不能凭空出现五连（否则对手/自己会莫名其妙已经赢了）
    rebuildScores();
    if (!noFiveOnBoard()) {
        memcpy(_board, backup, sizeof(_board));
        rebuildFromBoard();
        return;
    }
    rebuildFromBoard();
}

// 移子：把玩家最值钱的一颗棋子，扔到棋盘上最没用的空地
void AppGomoku::cheatRelocate() {
    int pr = -1, pc = -1, ps = -1;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c] != 1) continue;
            int s = heuristicPoint(r, c, 1);
            if (s > ps) { ps = s; pr = r; pc = c; }
        }
    }
    if (pr < 0) return;

    // 目标点：周围 2 格内没棋子的空地优先，其次离中心越远越"废"
    int tr = -1, tc = -1, tbest = -1;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c]) continue;
            int d = abs(r - 7) + abs(c - 7);
            int s = (_near[r][c] == 0 ? 100 : 0) + d;
            if (s > tbest) { tbest = s; tr = r; tc = c; }
        }
    }
    if (tr < 0) return;

    uint8_t backup[BOARD_SIZE][BOARD_SIZE];
    memcpy(backup, _board, sizeof(_board));

    _board[pr][pc] = 0;
    _board[tr][tc] = 1;

    rebuildScores();
    if (!noFiveOnBoard()) {
        memcpy(_board, backup, sizeof(_board));
        rebuildFromBoard();
        return;
    }
    rebuildFromBoard();
}

// 换子：你的好棋和我的废棋直接对调
void AppGomoku::cheatSwap() {
    int pr = -1, pc = -1, ps = -1;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c] != 1) continue;
            int s = heuristicPoint(r, c, 1);
            if (s > ps) { ps = s; pr = r; pc = c; }
        }
    }

    int ar = -1, ac = -1, as = 0x7fffffff;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c] != 2) continue;
            int s = heuristicPoint(r, c, 2);
            if (s < as) { as = s; ar = r; ac = c; }
        }
    }

    if (pr < 0 || ar < 0) return;

    uint8_t backup[BOARD_SIZE][BOARD_SIZE];
    memcpy(backup, _board, sizeof(_board));

    _board[pr][pc] = 2;
    _board[ar][ac] = 1;

    rebuildScores();
    if (!noFiveOnBoard()) {
        memcpy(_board, backup, sizeof(_board));
        rebuildFromBoard();
        return;
    }
    rebuildFromBoard();
}

// 吃子：直接把对你最有用的一颗棋子从棋盘上抹掉
void AppGomoku::cheatEraseThreat() {
    int tr = -1, tc = -1, ts = -1;

    // 优先看玩家最近一手，往往就是他刚做出来的威胁
    if (!_move_history.empty() && _move_history.back().player == 1) {
        Move m = _move_history.back();
        if (_board[m.r][m.c] == 1) {
            tr = m.r;
            tc = m.c;
            ts = heuristicPoint(m.r, m.c, 1);
        }
    }

    // 最近一手不痛不痒，就找全盘最值钱的玩家棋子
    int br = -1, bc = -1, bs = -1;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c] != 1) continue;
            int s = heuristicPoint(r, c, 1);
            if (s > bs) { bs = s; br = r; bc = c; }
        }
    }
    if (bs > ts * 2) { tr = br; tc = bc; }

    if (tr < 0) return;

    for (int i = (int)_move_history.size() - 1; i >= 0; i--) {
        if (_move_history[i].r == tr && _move_history[i].c == tc && _move_history[i].player == 1) {
            _move_history.erase(_move_history.begin() + i);
            break;
        }
    }
    unmakeMove(tr, tc, 1);
}

// 悔棋：从玩家最近 8 手里，挑"删掉之后 AI 局面分涨得最多"的那一手删掉
// 不是无脑撤最后一手，所以经常能把玩家真正的杀招拆掉
bool AppGomoku::cheatUndoBestPlayerMove() {
    int n = (int)_move_history.size();
    if (n < 3) return false;

    int32_t before = _score[2] - _score[1];
    int bestIdx = -1;
    int32_t bestGain = 0;

    int scan = (n < 8) ? n : 8;
    for (int k = 1; k <= scan; k++) {
        int i = n - k;
        Move m = _move_history[i];
        if (m.player != 1) continue;
        unmakeMove(m.r, m.c, 1);
        int32_t gain = (_score[2] - _score[1]) - before;
        makeMove(m.r, m.c, 1);
        if (gain > bestGain) { bestGain = gain; bestIdx = i; }
    }

    if (bestIdx < 0 || bestGain < 60) return false;

    Move m = _move_history[bestIdx];
    _move_history.erase(_move_history.begin() + bestIdx);
    unmakeMove(m.r, m.c, 1);
    return true;
}

// 返回 true 表示已经作弊完毕，可以继续正常落子
bool AppGomoku::applyCheat() {
    uint8_t snapshot[BOARD_SIZE][BOARD_SIZE];
    memcpy(snapshot, _board, sizeof(_board));

    int wr[8], wc[8];
    int nw = scanWinPoints(1, wr, wc, 8);

    int kind;
    if (nw >= 1) {
        // 你已经做出成五点了：优先吃子/偷子，精准拆掉，不随机
        kind = ((esp_random() % 100) < 60) ? 6 : 1;
    } else if (_score[2] - _score[1] < -1500) {
        // 大劣势：上重手
        static const int heavy[4] = { 0, 1, 4, 5 };
        kind = heavy[esp_random() % 4];
    } else {
        kind = (int)(esp_random() % 7);
    }

    switch (kind) {
        case 0:
            cheatShuffle();
            break;
        case 1:
            cheatSteal();
            break;
        case 2:
            cheatRelocate();
            break;
        case 3:
            _cheat_extra_move = true;
            break;
        case 4:
            if (!cheatUndoBestPlayerMove()) cheatShuffle();
            break;
        case 5:
            cheatSwap();
            break;
        default:
            cheatEraseThreat();
            break;
    }

    // 如果这一招没真正改变棋盘（找不到目标），退化成打乱，保证一定会使坏
    if (kind != 3 && memcmp(snapshot, _board, sizeof(_board)) == 0) {
        cheatShuffle();
        kind = 0;
    }

    _cheat_kind_last = kind;
    _cheat_count++;
    _danger_moves = 0;
    markThreats();
    showTaunt(2);
    drawBoard();
    return true;
}

// ================= 玩家作弊(作弊模式) =================
// 只是"玩家版"的三招, 都让玩家占便宜; 用完立刻触发 AI 问责剧情。

// 偷子: 把 AI 最值钱的一颗子变成玩家的
void AppGomoku::cheatPlayerSteal() {
    int br = -1, bc = -1, bs = -1;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c] != 2) continue;
            int s = heuristicPoint(r, c, 2);
            if (s > bs) { bs = s; br = r; bc = c; }
        }
    }
    if (br < 0) return;

    uint8_t backup[BOARD_SIZE][BOARD_SIZE];
    memcpy(backup, _board, sizeof(_board));
    _board[br][bc] = 1;
    rebuildScores();
    if (!noFiveOnBoard()) {           // 变颜色直接连成五子就不许
        memcpy(_board, backup, sizeof(_board));
        rebuildFromBoard();
        return;
    }
    rebuildFromBoard();
}

// 吃子: 直接抹掉 AI 最有用的一颗子
void AppGomoku::cheatPlayerErase() {
    int br = -1, bc = -1, bs = -1;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c] != 2) continue;
            int s = heuristicPoint(r, c, 2);
            if (s > bs) { bs = s; br = r; bc = c; }
        }
    }
    if (br < 0) return;

    _board[br][bc] = 0;
    rebuildFromBoard();
}

void AppGomoku::playerCheat(int kind) {
    uint8_t before[BOARD_SIZE][BOARD_SIZE];
    memcpy(before, _board, sizeof(_board));

    if (kind == 1)      cheatPlayerSteal();
    else if (kind == 2) cheatPlayerErase();
    else                _cheat_extra_player_move = true;

    // 偷/吃没找到目标时兜底: 改成"连走两手", 保证作弊一定有收益
    if (kind != 3 && memcmp(before, _board, sizeof(_board)) == 0) {
        _cheat_extra_player_move = true;
    }

    markThreats();
    ESP_LOGI("GOMOKU", "player cheat kind=%d heap=%u", kind, (unsigned)esp_get_free_heap_size());
    triggerCheatAlert();
}

// 立刻冻结全场, 1.5 秒后给出选择题
void AppGomoku::triggerCheatAlert() {
    _cheat_alert = 1;
    _cheat_freeze_ms = GetHAL().millis();
    _cheat_sel = 0;
    _ai_move_timer = 0;
    _taunt_visible = false;
    drawBoard();
    ESP_LOGI("GOMOKU", "AI caught cheat! freeze=1500ms heap=%u", (unsigned)esp_get_free_heap_size());
}

void AppGomoku::resolveCheatChoice() {
    _caught_deny = (_cheat_sel == 1);
    _cheat_alert = 0;
    _cheat_freeze_ms = 0;
    _ai_move_timer = 0;                 // 恢复后 AI 重新计思考时间
    _turn_start_ms = GetHAL().millis(); // 冻结/答题不算进玩家的思考时间
    _long_taunt_done = false;

    _taunt_kind = 5;
    _taunt_index = (int)(esp_random() % (uint32_t)(_caught_deny ? kTauntCaughtNoN : kTauntCaughtYesN));
    _taunt_visible = true;
    _taunt_timer = GetHAL().millis();

    if (_caught_deny) {
        // 否认: 直接最高难度(十星, 该档 AI 会作弊), 从原局面继续
        _difficulty = 10;
        audio::play_melody({ 48, 45, 41 }, 0.08);
    } else {
        audio::play_melody({ 60, 57 }, 0.06);
    }

    drawBoard();
    ESP_LOGI("GOMOKU", "cheat answer=%s difficulty=%d", _caught_deny ? "NO" : "YES", _difficulty);
}

// 开局库：白方第一手贴身斜扛，第二手之前交给搜索
bool AppGomoku::openingBook(int& r, int& c) {
    int stones = (int)_move_history.size();
    if (stones != 1) return false;

    Move m = _move_history[0];
    int dr = (m.r <= 7) ? 1 : -1;
    int dc = (m.c <= 7) ? 1 : -1;
    int rr = m.r + dr, cc = m.c + dc;
    if (isValid(rr, cc) && _board[rr][cc] == 0) { r = rr; c = cc; return true; }

    if (_board[7][7] == 0) { r = 7; c = 7; return true; }
    return false;
}

int AppGomoku::heuristicPoint(int r, int c, int p) {
    int total = 0;
    for (int d = 0; d < 4; d++) {
        int cnt = 1;
        int openA = 0, openB = 0;

        int k = 1;
        for (; k < 5; k++) {
            int nr = r + kDR[d] * k, nc = c + kDC[d] * k;
            if (!isValid(nr, nc) || _board[nr][nc] != p) break;
            cnt++;
        }
        {
            int nr = r + kDR[d] * k, nc = c + kDC[d] * k;
            if (isValid(nr, nc) && _board[nr][nc] == 0) openA = 1;
        }

        k = 1;
        for (; k < 5; k++) {
            int nr = r - kDR[d] * k, nc = c - kDC[d] * k;
            if (!isValid(nr, nc) || _board[nr][nc] != p) break;
            cnt++;
        }
        {
            int nr = r - kDR[d] * k, nc = c - kDC[d] * k;
            if (isValid(nr, nc) && _board[nr][nc] == 0) openB = 1;
        }

        int open = openA + openB;
        if (cnt >= 5) total += 1000000;
        else if (cnt == 4) total += (open == 2) ? 100000 : (open == 1 ? 12000 : 0);
        else if (cnt == 3) total += (open == 2) ? 4000 : (open == 1 ? 700 : 0);
        else if (cnt == 2) total += (open == 2) ? 300 : (open == 1 ? 60 : 0);
        else total += (open == 2) ? 20 : 4;
    }
    return total;
}

void AppGomoku::heuristicMove(int lvl, int& br, int& bc) {
    int R[64], C[64];
    int n = genMoves(R, C, 40, 2);
    if (n <= 0) { br = -1; bc = -1; return; }

    if (lvl <= 1) {
        int i = (int)(esp_random() % n);
        br = R[i];
        bc = C[i];
        return;
    }

    long best = -1;
    br = R[0];
    bc = C[0];

    for (int i = 0; i < n; i++) {
        int atk = heuristicPoint(R[i], C[i], 2);
        int def = heuristicPoint(R[i], C[i], 1);
        long s = 0;
        if (lvl == 2) s = atk;
        else if (lvl == 3) s = atk + def / 2;
        else s = (long)atk * 3 + (long)def * 2;

        if (lvl == 2) s += esp_random() % (atk / 2 + 1);
        else if (lvl == 3) s += esp_random() % (atk / 10 + 1);

        s += (7 - abs(R[i] - 7) - abs(C[i] - 7));

        if (s > best) { best = s; br = R[i]; bc = C[i]; }
    }
}

// 玩家每落一子，跟"当前局面下的最优点"比一下，记录失误
void AppGomoku::reviewPlayerMove(int r, int c) {
    int R[40], C[40];
    int n = genMoves(R, C, 40, 1);
    if (n <= 0) return;

    // 一个点的价值 = 自己下这里的进攻价值 + 挡住对手的价值
    int mine = heuristicPoint(r, c, 1) + heuristicPoint(r, c, 2);
    int best = mine;
    for (int i = 0; i < n; i++) {
        int v = heuristicPoint(R[i], C[i], 1) + heuristicPoint(R[i], C[i], 2);
        if (v > best) best = v;
    }

    int loss = best - mine;
    if (loss > 1500) _blunders++;
    if (loss > _worst_loss) {
        _worst_loss = loss;
        _worst_move_no = (int)_move_history.size() + 1;
    }
}

void AppGomoku::recordTrend() {
    int32_t v = _score[2] - _score[1];
    int b;
    if (v >= 15000) b = 5;
    else if (v >= 6000) b = 4;
    else if (v >= 2500) b = 3;
    else if (v >= 1000) b = 2;
    else if (v >= 300) b = 1;
    else if (v > -300) b = 0;
    else if (v > -1000) b = -1;
    else if (v > -2500) b = -2;
    else if (v > -6000) b = -3;
    else if (v > -15000) b = -4;
    else b = -5;

    _trend[_trend_head] = (int8_t)b;
    _trend_head = (_trend_head + 1) % 24;
    if (_trend_count < 24) _trend_count++;
}

void AppGomoku::updateDanger() {
    _danger_moves = 0;
    updateMood();

    if (_difficulty >= 3) {
        static int dr_[32], dc_[32];

        // 1) AI 手里已经有两个成五点（双四），下一手必杀
        int nw = scanWinPoints(2, dr_, dc_, 32);
        if (nw >= 2) {
            _danger_moves = 2;
        } else if (nw == 0) {
            // 2) 连续冲四（VCF）的必然杀
            //    只有 1 个成五点时玩家挡掉就没事，不算必杀，走下面的估算
            _deadline_us = now_us() + (_difficulty >= 8 ? 260000 : 120000);
            _nodes = 0;
            int d = vcfDistance(2, (_difficulty >= 8) ? 10 : 6);
            _nodes = 0;
            if (d > 0) _danger_moves = d + 1;   // 冲四序列 + 最后成五的那一手
        }

        // 3) 没有必然杀，就按全局优势估"还能撑多少手"，覆盖从十几步到几十步
        if (_danger_moves == 0) {
            int32_t adv = _score[2] - _score[1];
            if (adv > 20000) _danger_moves = 8;
            else if (adv > 10000) _danger_moves = 12;
            else if (adv > 5000) _danger_moves = 18;
            else if (adv > 2000) _danger_moves = 26;
            else if (adv > 800) _danger_moves = 36;
            else if (adv > 200) _danger_moves = 48;
            else if (adv > 0) _danger_moves = 60;
            else _danger_moves = 80;
        }
    }

    if (_danger_moves > 0 && _danger_moves < _min_danger) _min_danger = _danger_moves;
    drawBoard();
}

// 情绪：AI 落后会急（算更深、作弊更凶），领先会浪（少算一层）
void AppGomoku::updateMood() {
    if (_taunt_kind == 3) return;
    int32_t gap = _score[1] - _score[2];
    if (gap > 1500) _mood = 1;
    else if (gap < -2500) _mood = 2;
    else _mood = 0;
}

// 联机对战的嘲讽: 不走 showTaunt 的 PVP 拦截, 双方都能手动发
// from_remote=true 表示是手柄方发来的, 只显示给对方(不显示在自己屏幕上)
void AppGomoku::netTaunt(bool from_remote) {
    _taunt_from_remote = from_remote;
    _taunt_kind = 0;
    _taunt_index = (int)(esp_random() % kTauntNormalN);
    _taunt_visible = true;
    _taunt_timer = GetHAL().millis();
    drawBoard();
}

// 底部滚动横幅: 手柄方开作弊时, 文字从左进、向右滑出、再重来
// 棋盘已经上移 7px, 这条横幅只占最底部 13px, 不会压住棋盘
// 注意: 文字先离屏渲染成位图缓存, 每帧手动裁剪+逐行 memcpy 进 canvas。
// 之前直接用负 x 光标 print, LovyanGFX 对半在屏外的字符串裁剪行为让
// 横幅在两端"停几秒", 手动位块拷贝后左进右出完全平滑。
void AppGomoku::drawCheatBanner(uint32_t now) {
    bool on = _pvp && _net_cheat_remote;
    if (!on || !_canvas || !_canvas->getBuffer()) {
        _banner_on = false;
        return;
    }
    int sw = _canvas->width();
    int sh = _canvas->height();
    const int h = 13;
    int y = sh - h;
    const char* t = "对方玩家正在启用作弊模式  ★  ";

    // 文本位图缓存(只做一次, 8bit 索引与 canvas 同格式)
    static uint8_t* s_txt = nullptr;
    static int s_tw = 0;
    if (!s_txt) {
        _canvas->setFont(&fonts::efontCN_12);
        int tw = _canvas->textWidth(t);
        if (tw <= 0) tw = 1;
        LGFX_Sprite tmp;
        tmp.setColorDepth(8);
        if (tmp.createSprite(tw, h)) {
            tmp.fillScreen(TFT_BLACK);
            tmp.setFont(&fonts::efontCN_12);
            tmp.setTextColor(TFT_YELLOW);
            tmp.setCursor(0, 0);
            tmp.print(t);
            s_txt = (uint8_t*)malloc(tw * h);
            if (s_txt) {
                memcpy(s_txt, tmp.getBuffer(), tw * h);
                s_tw = tw;
            }
        }
        tmp.deleteSprite();
        if (!s_txt) return;
    }
    _banner_tw = s_tw;

    _canvas->fillRect(0, y, sw, h, TFT_BLACK);
    int span = sw + s_tw;
    int off = (int)((now / 22) % (uint32_t)span);
    int x = -s_tw + off;              // 负 x = 文字左部还在屏外
    int dx0 = x > 0 ? x : 0;          // 可见区左端
    int dx1 = x + s_tw < sw ? x + s_tw : sw; // 可见区右端
    if (dx1 > dx0) {
        uint8_t* buf = (uint8_t*)_canvas->getBuffer();
        int sx0 = dx0 - x;
        int w = dx1 - dx0;
        for (int j = 0; j < h; j++)
            memcpy(buf + (y + j) * sw + dx0, s_txt + j * s_tw + sx0, w);
    }
    _banner_on = true;
}


// 键盘方的作弊辅助开关
void AppGomoku::toggleNetCheat() {
    _net_cheat = !_net_cheat;
    _hint_mc = -1;
    _hint_r = _hint_c = -1;
    if (_net_cheat) computeHint();
    drawBoard();
}

void AppGomoku::computeHint() {
    _hint_r = _hint_c = -1;
    if (!_net_started || _game_over || _move_history.empty()) return;

    int me = _current_player, op = 3 - me;
    int a[8], b[8];

    // 1) 自己能直接成五
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c]) continue;
            if (winPoints(r, c, me, a, b, 8) == -1) { _hint_r = r; _hint_c = c; return; }
        }

    // 2) 对手能成五 → 必须挡
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c]) continue;
            if (winPoints(r, c, op, a, b, 8) == -1) { _hint_r = r; _hint_c = c; return; }
        }

    // 3) 一般情况: 自己活四 > 挡对方活四 > 靠近已有棋子
    int best = -1;
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c]) continue;
            int km = winPoints(r, c, me, a, b, 8);
            int ko = winPoints(r, c, op, a, b, 8);
            int sc = km * 120 + ko * 100 + _near[r][c] * 2;
            if (sc > best) { best = sc; _hint_r = r; _hint_c = c; }
        }
}

void AppGomoku::showTaunt(int kind) {
    if (_pvp) return; // 双人对战没有 AI，不嘲讽
    _taunt_kind = kind;
    if (kind == 0) {
        // 性格口头禅 / 情绪台词 / 普通台词 混着来，不要死板
        int pr = (int)(esp_random() % 100);
        if (pr < 32) _taunt_index = 300 + _persona * 10 + (int)(esp_random() % 4);
        else if (_mood == 1) _taunt_index = 100 + (int)(esp_random() % kTauntAngryN);
        else if (_mood == 2) _taunt_index = 200 + (int)(esp_random() % kTauntLazyN);
        else _taunt_index = (int)(esp_random() % kTauntNormalN);
    }
    else if (kind == 1) _taunt_index = (int)(esp_random() % kTauntLongN);
    else if (kind == 2) _taunt_index = (_cheat_kind_last < 0) ? 0 : _cheat_kind_last;
    else if (kind == 4) _taunt_index = (int)(esp_random() % kTauntThinkN);
    else _taunt_index = (int)(esp_random() % kTauntWinN);
    _taunt_visible = true;
    _taunt_timer = GetHAL().millis();
    drawBoard();
}

// ================= 决策 =================

void AppGomoku::aiMove() {
    memset(_forbid, 0, sizeof(_forbid));
    _cheat_depth = 0;
    _ai_thinking = true;  // 封锁玩家输入, 防止 AI 搜索期间玩家改 _board 导致竞态闪退

    // 思考时偶尔冒出一句碎碎念（让我想想我下哪……）
    if (!_is_ai_assist_mode && (int)(esp_random() % 100) < 55) {
        showTaunt(4);
    }

    uint32_t t0 = GetHAL().millis();
    ESP_LOGI("GOMOKU", "ai start lv=%d moves=%d heap=%u stack_hw=%u",
             _difficulty, (int)_move_history.size(),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    heapProbe("ai_start");
    aiMoveInner();
    heapProbe("ai_end");

    ESP_LOGI("GOMOKU", "ai done %ums nodes=%d heap=%u stack_hw=%u",
             (unsigned)(GetHAL().millis() - t0), (int)_nodes,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    _ai_thinking = false;
}

void AppGomoku::aiMoveInner() {
    int lvl = _difficulty;
    int r = -1, c = -1;

    // 1) 自己能直接成五
    if (findWinPoint(2, r, c)) { playAt(r, c); updateDanger(); return; }

    // 1.5) 十星：落后就使坏；局势胶着时也会偶尔"看心情"来一手
    if (lvl >= 10 && _cheat_depth == 0 && _cheat_count < 40 && _move_history.size() >= 6) {
        int32_t gap = _score[1] - _score[2];
        // 性格决定作弊意愿：稳健 60/10、凶狠 75/15、诡诈 90/30
        static const int kCheatBehind[3] = { 60, 75, 90 };
        static const int kCheatRandom[3] = { 10, 15, 30 };
        int pid = (_persona >= 0 && _persona < 3) ? _persona : 0;

        bool do_cheat = false;
        if (gap > 600 && (int)(esp_random() % 100) < kCheatBehind[pid]) do_cheat = true;
        else if (_move_history.size() >= 12 && (int)(esp_random() % 100) < kCheatRandom[pid]) do_cheat = true;
        if (do_cheat) { applyCheat(); return; }  // 作弊后直接 return, 让玩家先看到结果再下一手
    }

    // 2) 对手有成五点
    int wr = -1, wc = -1;
    int nw = countWinPoints(1, wr, wc);
    if (nw >= 1) {
        // 十星：对手两个成五点（双四），常规挡不住 —— 直接悔棋
        if (nw >= 2 && lvl >= 10 && _cheat_depth < 3 && _cheat_count < 20 && _move_history.size() >= 3) {
            cheatAndRetry();
            return;
        }
        if (wr >= 0) { playAt(wr, wc); updateDanger(); return; }
    }

    // 3) 开局库
    if (lvl >= 3 && openingBook(r, c)) { playAt(r, c); updateDanger(); return; }

    // 4) 低星：纯启发式，故意留破绽
    if (lvl <= 4) {
        heuristicMove(lvl, r, c);
        if (r >= 0) { playAt(r, c); updateDanger(); }
        return;
    }

    // 5) 中高星：先 VCF 找连续冲四杀
    int vd = (lvl >= 9) ? 10 : (lvl >= 8 ? 8 : 6);
    {
        _nodes = 0;
        _deadline_us = now_us() + (lvl >= 9 ? 450000 : 250000);
    }
    if (vcf(2, vd, r, c)) { playAt(r, c); updateDanger(); return; }

    // 6) 深搜（性格 + 情绪都会影响深度：急了多算，领先了浪，诡诈少算靠作弊补）
    int depth = (lvl >= 9) ? 6 : (lvl >= 7 ? 4 : 2);
    if (_persona == 1 && depth < 6) depth += 2;   // 凶狠：多算两层
    if (_persona == 2 && depth > 2) depth -= 2;   // 诡诈：少算两层
    if (_mood == 1 && depth < 6) depth += 2;      // 急了
    if (_mood == 2 && depth > 2) depth -= 2;      // 浪了
    if (depth > 7) depth = 7;  // 硬封顶: 递归太深会爆栈

    int budget = (lvl >= 10) ? 600 : (lvl >= 9 ? 500 : (lvl >= 8 ? 400 : (lvl >= 7 ? 300 : 200)));

    _nodes = 0;
    _deadline_us = now_us() + budget * 1000;
    if (!searchBest(depth, r, c) || r < 0) {
        heuristicMove(5, r, c);
    }
    if (r < 0) {
        // 彻底兜底: 就算所有 AI 算法都失败了, 至少下中心或第一个空位
        for (int try_y = 7; try_y >= 0; try_y--) for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int yy = try_y + dy, xx = try_y + dx;
                if (yy >= 0 && yy < BOARD_SIZE && xx >= 0 && xx < BOARD_SIZE && _board[yy][xx] == 0) { r = yy; c = xx; break; }
            }
        if (r < 0) r = 7, c = 7;  // 真不行下天元
    }

    // 7) 十星：这一步下去会被对手 VCF 反杀 → 禁手重下
    if (lvl >= 10 && _cheat_depth < 2) {
        bool bad = false;
        makeMove(r, c, 2);
        if (!isFive(r, c, 2)) {
            int dr, dc;
            _nodes = 0;
            _deadline_us = now_us() + 220000;
            bad = vcf(1, 6, dr, dc);
        }
        unmakeMove(r, c, 2);

        if (bad) {
            _forbid[r][c] = 1;
            _cheat_count++;
            _cheat_kind_last = 4;
            showTaunt(2);
            _nodes = 0;
            _deadline_us = now_us() + budget * 1000;
            if (searchBest(depth, r, c) && r >= 0) {
                playAt(r, c);
                updateDanger();
                return;
            }
            cheatAndRetry();
            return;
        }
    }

    playAt(r, c);
    updateDanger();
}

void AppGomoku::cheatAndRetry() {
    if (_cheat_depth >= 3 || _cheat_count >= 20 || _move_history.size() < 3) {
        // 作弊额度用完：老老实实落一手，绝不能把 AI 卡在原地不动
        int r = -1, c = -1;
        heuristicMove(6, r, c);
        if (r >= 0) playAt(r, c);
        updateDanger();
        return;
    }

    _cheat_depth++;
    _cheat_count++;

    // 1) 先撤掉自己刚才那一手（多半是昏招），并设为禁手，重下时不会再走同一点
    int fbR = -1, fbC = -1;
    if (!_move_history.empty() && _move_history.back().player == 2) {
        Move m = _move_history.back();
        _move_history.pop_back();
        unmakeMove(m.r, m.c, 2);
        fbR = m.r;
        fbC = m.c;
    }
    if (fbR >= 0) _forbid[fbR][fbC] = 1;

    // 2) 再从你最近几手里挑"最伤我"的一手删掉，而不是无脑撤最后一手
    cheatUndoBestPlayerMove();

    _cheat_kind_last = 4;
    _current_player = 2;
    _game_over = false;
    _winner = 0;
    _danger_moves = 0;
    _turn_start_ms = GetHAL().millis();
    _long_taunt_done = false;

    markThreats();
    showTaunt(2);
    drawBoard();
    aiMoveInner();
}

// ================= 绘制 =================

// 全盘扫描威胁点，供棋盘上的小标记使用
// 1 = 你下这里直接成五   2 = AI 下这里会成四   3 = AI 下这里直接成五
void AppGomoku::markThreats() {
    memset(_threat, 0, sizeof(_threat));

    int a[8], b[8];
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            if (_board[r][c]) continue;
            int k = winPoints(r, c, 2, a, b, 8);
            if (k == -1) { _threat[r][c] = 3; continue; }
            if (k >= 1) { _threat[r][c] = 2; continue; }
            if (winPoints(r, c, 1, a, b, 8) == -1) _threat[r][c] = 1;
        }
    }
}

void AppGomoku::drawTrend(int x, int y, int w, int h) {
    _canvas->drawRect(x, y, w, h, 0x4208);
    int mid = y + h / 2;
    _canvas->drawFastHLine(x + 1, mid, w - 2, 0x18E3);

    if (_trend_count < 1) return;

    int px = -1, py = -1;
    for (int i = 0; i < _trend_count; i++) {
        int idx = (_trend_head - _trend_count + i + 24) % 24;
        int v = _trend[idx];
        int xx = x + 1 + i * (w - 2) / 23;
        int yy = mid - v * (h / 2 - 2) / 5;
        uint16_t col = (v >= 0) ? TFT_GREEN : TFT_RED;
        if (px >= 0) {
            _canvas->drawLine(px, py, xx, yy, col);
            _canvas->drawLine(px, py + 1, xx, yy + 1, col);
        }
        _canvas->fillRect(xx - 1, yy - 1, 3, 3, col);
        px = xx;
        py = yy;
    }
}

void AppGomoku::drawInfoPanel(int x, int w) {
    if (_pvp) {
        // 双人对战: 隐藏 AI 的危险步数/性格/作弊/情绪, 只留手数/用时/局势条
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setTextColor(TFT_CYAN);
        _canvas->setCursor(x + 2, 2);
        _canvas->print("双人对战");
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(x + 2, 20);
        _canvas->printf("MOVE %d", (int)_move_history.size());
        uint32_t el = (GetHAL().millis() - _game_start_ms) / 1000;
        _canvas->setCursor(x + 2, 38);
        _canvas->printf("%d:%02d", (int)(el / 60), (int)(el % 60));

        int32_t s1 = (_score[1] > 0) ? _score[1] : 0;
        int32_t s2 = (_score[2] > 0) ? _score[2] : 0;
        int split = (int)((int64_t)(s1 + 1) * w / (s1 + s2 + 2));
        _canvas->fillRect(x, 60, split, 4, TFT_GREEN);
        _canvas->fillRect(x + split, 60, w - split, 4, TFT_RED);

        // 作弊辅助: 直接告诉该下哪
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setCursor(x + 2, 74);
        if (_net_cheat && _hint_r >= 0) _canvas->printf("下 %c%d", 'A' + _hint_c, _hint_r + 1);
        else if (_net_cheat) _canvas->print("作弊中");

        _canvas->setFont(&fonts::Font0);
        return;
    }

    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(x, 2);
    _canvas->print("你还剩");

    char buf[4];
    if (_danger_moves >= 10) {
        buf[0] = (char)('0' + (_danger_moves / 10) % 10);
        buf[1] = (char)('0' + _danger_moves % 10);
        buf[2] = '\0';
    } else if (_danger_moves > 0) {
        buf[0] = (char)('0' + _danger_moves);
        buf[1] = '\0';
    } else {
        buf[0] = '-';
        buf[1] = '-';
        buf[2] = '\0';
    }

    _canvas->setFont(&fonts::Font2);
    int tw = _canvas->textWidth(buf);
    _canvas->setTextColor(_danger_moves > 0
                              ? ((_danger_moves <= 3) ? TFT_RED : (_danger_moves <= 10 ? TFT_ORANGE : TFT_YELLOW))
                              : TFT_DARKGREY);
    _canvas->setCursor(x + 2, 14);
    _canvas->print(buf);

    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextColor(TFT_DARKGREY);
    _canvas->setCursor(x + 4 + tw, 22);
    _canvas->print("步");

    // 性格竖着放在右侧竖条空位：字大才看得清
    {
        int pid = (_persona >= 0 && _persona < 3) ? _persona : 0;
        const char* pn = kPersonaName[pid];
        _canvas->setFont(&fonts::efontCN_14);
        _canvas->setTextColor(kPersonaColor[pid]);
        int vx = x + w - 16;
        for (int i = 0; i < 2 && pn[i * 3]; i++) {
            char ch[4] = { pn[i * 3], pn[i * 3 + 1], pn[i * 3 + 2], 0 };
            _canvas->setCursor(vx, 2 + i * 16);
            _canvas->print(ch);
        }
        _canvas->setFont(&fonts::efontCN_12);
    }

    drawTrend(x, 34, w, 20);

    _canvas->setFont(&fonts::Font0);
    _canvas->setTextColor(TFT_LIGHTGREY);
    _canvas->setCursor(x, 57);
    _canvas->printf("MOVE %d", (int)_move_history.size());

    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextColor(TFT_MAGENTA);
    _canvas->setCursor(x, 66);
    _canvas->printf("作弊%d %s", _cheat_count, (_mood == 1) ? "急" : (_mood == 2 ? "浪" : "稳"));

    // 局势平衡条：绿色 = 你占优，红色 = AI 占优
    if (!(_taunt_visible && _taunt_kind == 3)) {
        int32_t s1 = (_score[1] > 0) ? _score[1] : 0;
        int32_t s2 = (_score[2] > 0) ? _score[2] : 0;
        int split = (int)((int64_t)(s1 + 1) * w / (s1 + s2 + 2));
        _canvas->fillRect(x, 114, split, 3, TFT_GREEN);
        _canvas->fillRect(x + split, 114, w - split, 3, TFT_RED);

        // 胜率预估：把 AI 的优势映射成 5%~95%
        int32_t adv = _score[2] - _score[1];
        int ai_rate = 50 - (int)(adv / 400);
        if (ai_rate < 5) ai_rate = 5;
        if (ai_rate > 95) ai_rate = 95;
        int my_rate = 100 - ai_rate;

        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setTextColor(my_rate >= 50 ? TFT_GREEN : TFT_RED);
        _canvas->setCursor(x + 2, 120);
        _canvas->printf("你%d%%", my_rate);
    }
}

// 解析当前嘲讽文本行(供 drawBoard 与 sendMirrorState 共用, 保证两端一致)
int AppGomoku::resolveTauntLines(const char** lines) {
    lines[0] = lines[1] = lines[2] = nullptr;
    if (_taunt_kind == 5) {
        // 作弊被抓包: 承认/否认两套台词
        if (_caught_deny) {
            for (int i = 0; i < 2; i++) lines[i] = kTauntCaughtNo[_taunt_index % kTauntCaughtNoN][i];
        } else {
            for (int i = 0; i < 2; i++) lines[i] = kTauntCaughtYes[_taunt_index % kTauntCaughtYesN][i];
        }
        return 2;
    }
    if (_taunt_kind == 4) {
        for (int i = 0; i < 2; i++) lines[i] = kTauntThink[_taunt_index % kTauntThinkN][i];
        return 2;
    }
    if (_taunt_kind == 3) {
        for (int i = 0; i < 3; i++) lines[i] = kTauntWin[_taunt_index % kTauntWinN][i];
        return 3;
    }
    if (_taunt_kind == 2) {
        for (int i = 0; i < 2; i++) lines[i] = kTauntCheat[_taunt_index % 7][i];
        return 2;
    }
    if (_taunt_kind == 1) {
        for (int i = 0; i < 2; i++) lines[i] = kTauntLong[_taunt_index % kTauntLongN][i];
        return 2;
    }
    if (_taunt_index >= 300) {
        int pi = (_taunt_index - 300) / 10;
        int li = (_taunt_index - 300) % 10;
        if (pi < 0 || pi > 2) pi = 0;
        if (li > 3) li %= 4;
        for (int i = 0; i < 2; i++) lines[i] = kTauntPersona[pi][li][i];
    } else if (_taunt_index >= 200) {
        for (int i = 0; i < 2; i++) lines[i] = kTauntLazy[(_taunt_index - 200) % kTauntLazyN][i];
    } else if (_taunt_index >= 100) {
        for (int i = 0; i < 2; i++) lines[i] = kTauntAngry[(_taunt_index - 100) % kTauntAngryN][i];
    } else {
        for (int i = 0; i < 2; i++) lines[i] = kTauntNormal[_taunt_index % kTauntNormalN][i];
    }
    return 2;
}

// ================= C3 监视器状态同步 =================
// GMKSTATE 全量快照 238B: 丢包无所谓, 下一包自愈
// 偏移布局(C3 端 parse_state 必须一致):
//   0-3 'G','M',0x20,seq | 4 flags0 | 5 flags1 | 6 winner | 7 cur_player
//   8 difficulty | 9 persona | 10 mood | 11-12 cursor | 13 move_count
//   14-15 last_move | 16 replay_idx | 17 replay_total | 18-21 比分/连胜
//   22 danger | 23 cheat | 24 blunders | 25 min_danger(999→255) | 26 worst_no
//   27-30 achv/achv_new | 31-32 clock_s | 33-34 game_sec | 35-42 score1/score2
//   43-44 trend count/head | 45-68 trend[24] | 69 taunt_kind | 70-123 taunt 3x18
//   124-180 threat 2bit | 181-237 board 2bit | 238 主机电量 | 239 保留
void AppGomoku::sendMirrorState() {
    if (!glink_is_linked()) return;

    uint8_t p[240];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x20; // p[3]=seq 由链路层填

    uint8_t f0 = 0;
    if (_game_over) f0 |= 1;
    if (_win_anim_active) f0 |= 2;
    if (_taunt_visible) f0 |= 4;
    if (_show_panel) f0 |= 8;
    if (_show_threat) f0 |= 16;
    if (_replay_active) f0 |= 32;
    if (_help_visible) f0 |= 64;
    if (_result_visible) f0 |= 128;
    p[4] = f0;

    uint8_t f1 = 0;
    if (_pvp) f1 |= 1;
    if (_is_ai_assist_mode) f1 |= 2;
    if (_blitz) f1 |= 4;
    if (_r_confirm_ms) f1 |= 8;
    if (_is_ai_mode) f1 |= 16;
    if (_net_cheat) f1 |= 32;   // 键盘方作弊辅助开启 → 监控仪(C3)顶部滚动横幅
    if (_taunt_from_remote) f1 |= 128;  // 这条嘲讽是手柄方发的, C3 自己不显示
    p[5] = f1;

    // 239: 联机对战状态 (bit0 联机中, bit1 键盘方准备, bit2 手柄方准备, bit3 已开局)
    uint8_t f2 = 0;
    if (_net_pvp) f2 |= 1;
    if (_net_ready_local) f2 |= 2;
    if (_net_ready_remote) f2 |= 4;
    if (_net_started) f2 |= 8;
    p[239] = f2;

    p[6] = (uint8_t)_winner;
    p[7] = (uint8_t)_current_player;
    p[8] = (uint8_t)_difficulty;
    p[9] = (uint8_t)_persona;
    p[10] = (uint8_t)_mood;
    p[11] = (uint8_t)_cursor_x;
    p[12] = (uint8_t)_cursor_y;

    int mc = (int)_move_history.size();
    p[13] = (uint8_t)(mc > 255 ? 255 : mc);

    int lr = 255, lc = 255;
    if (_replay_active) {
        if (_replay_idx > 0) { lr = _replay_moves[_replay_idx - 1].r; lc = _replay_moves[_replay_idx - 1].c; }
    } else if (mc > 0) {
        lr = _move_history.back().r;
        lc = _move_history.back().c;
    }
    p[14] = (uint8_t)lr; p[15] = (uint8_t)lc;
    p[16] = (uint8_t)(_replay_idx > 255 ? 255 : _replay_idx);
    p[17] = (uint8_t)(_replay_moves.size() > 255 ? 255 : _replay_moves.size());
    // 联机对战: 这两个字节(回放用不到)复用成作弊提示点坐标, 255 = 没有
    if (_pvp) {
        p[16] = (_hint_r >= 0 && _hint_r < 15) ? (uint8_t)_hint_r : 255;
        p[17] = (_hint_c >= 0 && _hint_c < 15) ? (uint8_t)_hint_c : 255;
    }

    p[18] = (uint8_t)(s_player_wins > 255 ? 255 : s_player_wins);
    p[19] = (uint8_t)(s_ai_wins > 255 ? 255 : s_ai_wins);
    p[20] = (uint8_t)(s_streak > 255 ? 255 : s_streak);
    p[21] = (uint8_t)(s_best_streak > 255 ? 255 : s_best_streak);
    p[22] = (uint8_t)(_danger_moves < 0 ? 0 : (_danger_moves > 99 ? 99 : _danger_moves));
    p[23] = (uint8_t)(_cheat_count > 255 ? 255 : _cheat_count);
    p[24] = (uint8_t)(_blunders > 255 ? 255 : _blunders);
    p[25] = (uint8_t)(_min_danger == 999 ? 255 : (_min_danger > 254 ? 254 : _min_danger));
    p[26] = (uint8_t)(_worst_move_no > 255 ? 255 : _worst_move_no);
    p[27] = (uint8_t)(_achv & 0xFF); p[28] = (uint8_t)(_achv >> 8);
    p[29] = (uint8_t)(_achv_new & 0xFF); p[30] = (uint8_t)(_achv_new >> 8);

    int c1 = (int)(_clock_ms[1] / 1000), c2 = (int)(_clock_ms[2] / 1000);
    p[31] = (uint8_t)(c1 < 0 ? 0 : (c1 > 255 ? 255 : c1));
    p[32] = (uint8_t)(c2 < 0 ? 0 : (c2 > 255 ? 255 : c2));
    uint32_t gs = (GetHAL().millis() - _game_start_ms) / 1000;
    if (gs > 5999) gs = 5999;
    p[33] = (uint8_t)(gs & 0xFF); p[34] = (uint8_t)(gs >> 8);

    int32_t s1 = _score[1], s2 = _score[2];
    memcpy(p + 35, &s1, 4);
    memcpy(p + 39, &s2, 4);

    p[43] = (uint8_t)_trend_count;
    p[44] = (uint8_t)_trend_head;
    memcpy(p + 45, _trend, 24);

    p[69] = (uint8_t)_taunt_kind;
    if (_taunt_visible && !_win_anim_active) {
        const char* lines[3];
        int nl = resolveTauntLines(lines);
        for (int i = 0; i < nl && i < 3; i++) {
            if (lines[i]) strncpy((char*)p + 70 + i * 18, lines[i], 17);
        }
    }

    // threat / board 2bit 打包(每字节 4 格, idx = r*15+c)
    for (int i = 0; i < 57; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 4; k++) {
            int idx = i * 4 + k;
            if (idx >= BOARD_SIZE * BOARD_SIZE) break;
            v |= (_threat[idx / BOARD_SIZE][idx % BOARD_SIZE] & 3) << (k * 2);
        }
        p[124 + i] = v;
    }
    for (int i = 0; i < 57; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 4; k++) {
            int idx = i * 4 + k;
            if (idx >= BOARD_SIZE * BOARD_SIZE) break;
            v |= (_board[idx / BOARD_SIZE][idx % BOARD_SIZE] & 3) << (k * 2);
        }
        p[181 + i] = v;
    }

    // 主机电量(0..100), 供 C3 右下角显示
    uint8_t bat = GetHAL().getBatLevel();
    p[238] = (bat <= 100) ? bat : 255;

    glink_send_state(p, sizeof(p));
}

void AppGomoku::drawBoard() {
    if (!_canvas || !_canvas->getBuffer() || _canvas->width() <= 0 || _canvas->height() <= 0) return;

    _canvas->fillScreen(THEME_COLOR_BG);
    _canvas->fillRect(START_X, START_Y, BOARD_PIXEL_SIZE, BOARD_PIXEL_SIZE, 0xE79C);

    _canvas->setColor(TFT_BLACK);
    for (int i = 0; i < BOARD_SIZE; i++) {
        _canvas->drawLine(START_X + CELL_SIZE / 2, START_Y + i * CELL_SIZE + CELL_SIZE / 2,
                          START_X + BOARD_PIXEL_SIZE - CELL_SIZE / 2, START_Y + i * CELL_SIZE + CELL_SIZE / 2);
        _canvas->drawLine(START_X + i * CELL_SIZE + CELL_SIZE / 2, START_Y + CELL_SIZE / 2,
                          START_X + i * CELL_SIZE + CELL_SIZE / 2, START_Y + BOARD_PIXEL_SIZE - CELL_SIZE / 2);
    }

    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            int cx = START_X + c * CELL_SIZE + CELL_SIZE / 2;
            int cy = START_Y + r * CELL_SIZE + CELL_SIZE / 2;
            if (_board[r][c] == 1) {
                _canvas->fillCircle(cx, cy, CELL_SIZE / 2 - 1, TFT_BLACK);
            } else if (_board[r][c] == 2) {
                _canvas->fillCircle(cx, cy, CELL_SIZE / 2 - 1, TFT_WHITE);
                _canvas->drawCircle(cx, cy, CELL_SIZE / 2 - 1, TFT_BLACK);
            }
        }
    }

    // 威胁点标记（默认关闭，按 T 开启）：
    //   红 = AI 下这里直接成五   橙 = AI 下这里成四   绿框 = 你下这里成五
    if (_show_threat && !_replay_active) {
        for (int r = 0; r < BOARD_SIZE; r++) {
            for (int c = 0; c < BOARD_SIZE; c++) {
                if (_board[r][c] || _threat[r][c] == 0) continue;
                int mx = START_X + c * CELL_SIZE + CELL_SIZE / 2;
                int my = START_Y + r * CELL_SIZE + CELL_SIZE / 2;
                if (_threat[r][c] == 3) _canvas->fillRect(mx - 2, my - 2, 5, 5, TFT_RED);
                else if (_threat[r][c] == 2) _canvas->fillRect(mx - 2, my - 2, 4, 4, TFT_ORANGE);
                else _canvas->drawRect(mx - 2, my - 2, 5, 5, TFT_GREEN);
            }
        }
    }

    // 最后一步高亮（回放时高亮刚摆上去的那手）
    if (_replay_active) {
        if (_replay_idx > 0) {
            Move& lm = _replay_moves[_replay_idx - 1];
            int lx = START_X + lm.c * CELL_SIZE;
            int ly = START_Y + lm.r * CELL_SIZE;
            _canvas->drawRect(lx, ly, CELL_SIZE, CELL_SIZE, TFT_CYAN);
        }
    } else if (!_move_history.empty()) {
        Move lm = _move_history.back();
        int lx = START_X + lm.c * CELL_SIZE;
        int ly = START_Y + lm.r * CELL_SIZE;
        _canvas->drawRect(lx, ly, CELL_SIZE, CELL_SIZE, TFT_CYAN);
    }

    // 作弊辅助: 黄色圈标出建议落子点
    if (_net_cheat && _hint_r >= 0 && _hint_c >= 0) {
        int hx = START_X + _hint_c * CELL_SIZE + CELL_SIZE / 2;
        int hy = START_Y + _hint_r * CELL_SIZE + CELL_SIZE / 2;
        _canvas->drawCircle(hx, hy, CELL_SIZE / 2 - 1, TFT_YELLOW);
        _canvas->drawCircle(hx, hy, CELL_SIZE / 2, TFT_YELLOW);
    }

    // 光标（回放时不画）
    if (!_replay_active) {
        int cx = START_X + _cursor_x * CELL_SIZE + CELL_SIZE / 2;
        int cy = START_Y + _cursor_y * CELL_SIZE + CELL_SIZE / 2;
        _canvas->drawRect(cx - CELL_SIZE / 2, cy - CELL_SIZE / 2, CELL_SIZE, CELL_SIZE, TFT_RED);
    }

    int screen_w = _canvas->width();
    int screen_h = _canvas->height();
    int left_x = 2;
    int right_x = START_X + BOARD_PIXEL_SIZE + 4;
    int right_w = screen_w - right_x;

    _canvas->setFont(&fonts::Font0);
    _canvas->setTextSize(1);

    // ---------- 左栏 ----------
    int y = 2;
    _canvas->setFont(&fonts::efontCN_10_b);
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(left_x, y);
    _canvas->print("五子棋");
    _canvas->setFont(&fonts::Font0);

    if (_pvp) {
        // 双人对战: 左栏换成对战状态, 单人那套 Lv/星级/YOU/AI/WIN 全隐藏
        _canvas->setFont(&fonts::efontCN_10_b);
        _canvas->setTextColor(TFT_CYAN);
        _canvas->setCursor(left_x, 14); _canvas->print(_net_pvp ? "联机对战" : "双人对战");
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(left_x, 25);
        if (_game_over) _canvas->print(_winner == 0 ? "平局" : (_winner == 1 ? "黑胜" : "白胜"));
        else _canvas->print(_current_player == 1 ? "黑方" : "白方");

        if (_net_pvp) {
            _canvas->setTextColor(_net_ready_local ? TFT_GREEN : TFT_DARKGREY);
            _canvas->setCursor(left_x, 36); _canvas->print(_net_ready_local ? "我 已备" : "我 未备");
            _canvas->setTextColor(_net_ready_remote ? TFT_GREEN : TFT_DARKGREY);
            _canvas->setCursor(left_x, 46); _canvas->print(_net_ready_remote ? "他 已备" : "他 未备");
            _canvas->setTextColor(TFT_LIGHTGREY);
            _canvas->setCursor(left_x, 58);
            if (!_net_started) _canvas->print("等双方准备");
            else _canvas->print(_current_player == 1 ? "轮到我" : "轮到对方");
            _canvas->setCursor(left_x, 68);
            _canvas->print((!_net_started || _game_over) ? "Enter准备" : "C嘲讽");
        }
        _canvas->setFont(&fonts::Font0);
    } else {
    y = 13;
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(left_x, y);
    _canvas->printf("Lv.%d", _difficulty);

    y = 22;
    _canvas->setTextColor(TFT_YELLOW);
    for (int row = 0; row < 2; row++) {
        _canvas->setCursor(left_x, y);
        for (int i = 0; i < 5; i++) {
            int idx = row * 5 + i;
            _canvas->print(idx < _difficulty ? "*" : " ");
        }
        y += 8;
    }

    int total_games = s_player_wins + s_ai_wins;
    int ai_rate = (total_games > 0) ? (s_ai_wins * 100 / total_games) : 0;

    y = 39;
    _canvas->setTextColor(TFT_GREEN);
    _canvas->setCursor(left_x, y); _canvas->printf("YOU%2d", s_player_wins); y += 8;
    _canvas->setTextColor(TFT_ORANGE);
    _canvas->setCursor(left_x, y); _canvas->printf("AI %2d", s_ai_wins); y += 8;
    _canvas->setTextColor(TFT_RED);
    _canvas->setCursor(left_x, y); _canvas->printf("WIN%2d%%", ai_rate); y += 9;

    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(left_x, y);
    if (_replay_active) {
        _canvas->print("REPLAY");
    } else if (_game_over) {
        if (_winner == 0) _canvas->print("DRAW");
        else if (_pvp) _canvas->print(_winner == 1 ? "BLK WIN" : "WHT WIN");
        else if (_is_ai_assist_mode) _canvas->print(_winner == 2 ? "AI WIN" : "OPP WIN");
        else _canvas->print(_winner == 1 ? "YOU WIN" : "AI WIN");
    } else {
        if (_pvp) _canvas->print(_current_player == 1 ? "BLACK" : "WHITE");
        else if (_is_ai_assist_mode) _canvas->print(_current_player == 1 ? "INPUT P1" : "THINK..");
        else _canvas->print(_current_player == 1 ? "YOURTURN" : "THINK..");
    }
    y += 9;

    _canvas->setTextColor(TFT_LIGHTGREY);
    _canvas->setCursor(left_x, y); _canvas->print(_pvp ? "PVP" : (_is_ai_assist_mode ? "Assist" : "Normal"));
    }

    // ---------- 左下角：密集状态区 ----------
    char stbuf[24];
    int hy = screen_h - 52;

    _canvas->setFont(&fonts::efontCN_10_b);
    _canvas->setTextColor(TFT_YELLOW);
    _canvas->setCursor(left_x, hy);
    _canvas->print("[H]帮助");

    _canvas->setTextColor(s_streak > 0 ? TFT_GREEN : TFT_DARKGREY);
    _canvas->setCursor(left_x, hy + 11);
    snprintf(stbuf, sizeof(stbuf), "连胜%d 最%d", s_streak, s_best_streak);
    _canvas->print(stbuf);

    // 钟 / 计时
    _canvas->setFont(&fonts::Font0);
    if (_blitz) {
        int cs = _game_over ? 0 : (_clock_ms[_current_player] / 1000);
        _canvas->setTextColor((!_game_over && _clock_ms[_current_player] < 10000) ? TFT_RED : TFT_CYAN);
        _canvas->setCursor(left_x, hy + 22);
        snprintf(stbuf, sizeof(stbuf), "CLK %d", cs);
    } else {
        uint32_t gt = (GetHAL().millis() - _game_start_ms) / 1000;
        _canvas->setTextColor(TFT_CYAN);
        _canvas->setCursor(left_x, hy + 22);
        snprintf(stbuf, sizeof(stbuf), "T %d:%02d", (int)(gt / 60), (int)(gt % 60));
    }
    _canvas->print(stbuf);

    // 失误 / 最险
    _canvas->setFont(&fonts::efontCN_10_b);
    _canvas->setTextColor(TFT_LIGHTGREY);
    _canvas->setCursor(left_x, hy + 31);
    snprintf(stbuf, sizeof(stbuf), "误%d 险%d", _blunders, _min_danger == 999 ? 0 : _min_danger);
    _canvas->print(stbuf);

    // 成就进度
    int achv_n = 0;
    for (int i = 0; i < 6; i++) if (_achv & (1u << i)) achv_n++;
    _canvas->setTextColor(achv_n >= 6 ? TFT_YELLOW : TFT_DARKGREY);
    _canvas->setCursor(left_x, hy + 41);
    snprintf(stbuf, sizeof(stbuf), "成就%d/6", achv_n);
    _canvas->print(stbuf);
    _canvas->setFont(&fonts::Font0);

    // ---------- 右栏：危险提示 + 趋势 + 嘲讽 ----------
    int ry = 7;
    if (_show_panel) {
        drawInfoPanel(right_x, right_w);
        ry = 79;
    }

    // 联机对战: 自己发的嘲讽只发给对方, 不在自己屏幕上显示
    if (_taunt_visible && !_win_anim_active && !(_pvp && !_taunt_from_remote)) {
        _canvas->setFont(&fonts::efontCN_14);
        _canvas->setTextColor(_taunt_kind == 4 ? TFT_CYAN : (_taunt_kind == 2 ? TFT_MAGENTA : (_taunt_kind == 1 ? TFT_ORANGE : TFT_RED)));

        const char* lines[3];
        int nl = resolveTauntLines(lines);

        int ty = ry;
        for (int i = 0; i < nl; i++) {
            const char* line = lines[i];
            if (!line) break;
            int tw = _canvas->textWidth(line);
            int tx = right_x + (right_w - tw) / 2;
            if (tx < right_x) tx = right_x;
            _canvas->setCursor(tx, ty);
            _canvas->print(line);
            ty += 15;
        }
        _canvas->setFont(&fonts::Font0);
    }

    // ---------- 胜利动画 ----------
    if (_win_anim_active) {
        uint32_t el = GetHAL().millis() - _win_anim_start;
        int bx = START_X + 6;
        int by = START_Y + BOARD_PIXEL_SIZE / 2 - 16;
        int bw = BOARD_PIXEL_SIZE - 12;
        _canvas->fillRect(bx, by, bw, 32, TFT_BLACK);
        _canvas->drawRect(bx, by, bw, 32, TFT_YELLOW);

        _canvas->setFont(&fonts::efontCN_10_b);
        _canvas->setTextDatum(middle_center);
        int cxm = bx + bw / 2;

        if (el < 600) {
            _canvas->setTextColor(TFT_GREEN);
            _canvas->drawString("你赢了！", cxm, by + 11);
            _canvas->setFont(&fonts::Font0);
            _canvas->setTextColor(TFT_WHITE);
            _canvas->drawString("WIN", cxm, by + 24);
        } else if (el < 1200) {
            _canvas->setTextColor(TFT_YELLOW);
            _canvas->drawString("难度提升", cxm, by + 11);
            _canvas->setFont(&fonts::Font0);
            char stars[16];
            int n = (_difficulty < 10) ? _difficulty + 1 : 10;
            for (int i = 0; i < n && i < 15; i++) stars[i] = '*';
            stars[n < 15 ? n : 15] = '\0';
            _canvas->drawString(stars, cxm, by + 24);
        } else {
            _canvas->setTextColor(TFT_CYAN);
            char buf[48];
            snprintf(buf, sizeof(buf), "第 %d 星 开新局", (_difficulty < 10) ? _difficulty + 1 : 10);
            _canvas->drawString(buf, cxm, by + 13);
        }

        _canvas->setFont(&fonts::Font0);
        _canvas->setTextDatum(top_left);
    }

    // ---------- R 重开确认提示 ----------
    if (_r_confirm_ms) {
        _canvas->setFont(&fonts::efontCN_10_b);
        const char* tip = "再按R确认重开";
        int tw = _canvas->textWidth(tip) + 8;
        int bx = START_X + (BOARD_PIXEL_SIZE - tw) / 2;
        _canvas->fillRect(bx, START_Y + 2, tw, 13, TFT_BLACK);
        _canvas->drawRect(bx, START_Y + 2, tw, 13, TFT_RED);
        _canvas->setTextColor(TFT_RED);
        _canvas->setCursor(bx + 4, START_Y + 3);
        _canvas->print(tip);
        _canvas->setFont(&fonts::Font0);
    }

    // ---------- 回放横幅 ----------
    if (_replay_active) {
        char rb[24];
        snprintf(rb, sizeof(rb), "回放 %d/%d", _replay_idx, (int)_replay_moves.size());
        _canvas->setFont(&fonts::efontCN_10_b);
        int bw2 = _canvas->textWidth(rb) + 8;
        int bx = START_X + (BOARD_PIXEL_SIZE - bw2) / 2;
        _canvas->fillRect(bx, START_Y + 2, bw2, 13, TFT_BLACK);
        _canvas->drawRect(bx, START_Y + 2, bw2, 13, TFT_CYAN);
        _canvas->setTextColor(TFT_CYAN);
        _canvas->setCursor(bx + 4, START_Y + 3);
        _canvas->print(rb);
        _canvas->setFont(&fonts::Font0);
    }

    // ---------- 作弊模式标识 ----------
    if (_cheat_mode && !_pvp && !_assess_mode && !_game_over) {
        _canvas->fillRect(1, 1, 44, 11, TFT_MAGENTA);
        _canvas->setFont(&fonts::Font0);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(3, 2);
        _canvas->print("CHEAT");
    }

    // ---------- 智力测试提示(开启后显示 3.5 秒) ----------
    if (_assess_banner) {
        if (GetHAL().millis() - _assess_banner_ms < 3500) {
            const char* tip = "智力测试：作弊键已禁用";
            _canvas->setFont(&fonts::efontCN_10_b);
            int tw = _canvas->textWidth(tip) + 10;
            int bx = (_canvas->width() - tw) / 2;
            _canvas->fillRect(bx, START_Y + 2, tw, 15, TFT_BLACK);
            _canvas->drawRect(bx, START_Y + 2, tw, 15, TFT_CYAN);
            _canvas->setTextColor(TFT_CYAN);
            _canvas->setCursor(bx + 5, START_Y + 4);
            _canvas->print(tip);
            _canvas->setFont(&fonts::Font0);
        } else {
            _assess_banner = false;
        }
    }

    // ---------- 作弊问责选择题 ----------
    if (_cheat_alert) drawCheatDialog();

    if (_result_visible) {
        // 智力测试: 用新的分项评分结算, 不再用旧口径的复盘数据
        if (_assess_mode) drawAssessResult();
        else drawResult();
    }
    if (_help_visible) drawHelp();

    // 联机时手柄方开作弊 → 底部滚动横幅(必须在推屏之前画)
    drawCheatBanner(GetHAL().millis());

    heapProbe("pre_push");
    _canvas->pushSprite(0, 0);

    // 状态同步到 C3 监视器(内部判断连接状态)
    sendMirrorState();
}

void AppGomoku::unlockAchv(int bit) {
    uint16_t mask = (uint16_t)(1u << bit);
    _achv |= mask;
    _achv_new |= mask;
}

// 帮助界面：列出全部键位说明
void AppGomoku::drawHelp() {
    int w = 190, h = 112;
    int x = (_canvas->width() - w) / 2;
    int yy = (_canvas->height() - h) / 2;
    _canvas->fillRect(x, yy, w, h, 0x0000);
    _canvas->drawRect(x, yy, w, h, TFT_YELLOW);

    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextColor(TFT_YELLOW);
    _canvas->setCursor(x + 8, yy + 5);
    _canvas->print("按键帮助");

    _canvas->setTextColor(TFT_WHITE);
    static const char* kHelp[] = {
        "方向/回车  走子落子",
        "X 危险标记  K AI性格",
        "G 面板      L 难度",
        "M 辅助      U 悔棋",
        "P 双人对战  V 回放",
        "T 限时60秒  R 重开",
        "H 本帮助    I 智力测试",
        "Y 作弊模式  1/2/3 作弊",
    };
    for (int i = 0; i < 8; i++) {
        _canvas->setCursor(x + 8, yy + 19 + i * 11);
        _canvas->print(kHelp[i]);
    }
}

// 战报结算画面
void AppGomoku::drawResult() {
    if (!_canvas) return;

    int w = 214, h = 112;
    int x = (_canvas->width() - w) / 2;
    int y = (_canvas->height() - h) / 2;

    _canvas->fillRect(x, y, w, h, 0x1082);
    _canvas->drawRect(x, y, w, h, TFT_WHITE);
    _canvas->drawFastHLine(x + 2, y + 20, w - 4, 0x4208);

    _canvas->setFont(&fonts::efontCN_14);
    _canvas->setTextDatum(middle_center);
    _canvas->setTextColor(_winner == 0 ? TFT_YELLOW : (_winner == 1 ? TFT_GREEN : TFT_RED));
    const char* title = _winner == 0 ? "平局"
                      : _pvp ? (_winner == 1 ? "黑方胜" : "白方胜")
                             : (_winner == 1 ? "你赢了" : "你输了");
    _canvas->drawString(title, x + w / 2, y + 11);
    _canvas->setTextDatum(top_left);

    uint32_t sec = (GetHAL().millis() - _game_start_ms) / 1000;
    if (sec > 5999) sec = 5999;

    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextColor(TFT_WHITE);

    char buf[48];
    snprintf(buf, sizeof(buf), "手数 %d  用时 %d:%02d", (int)_move_history.size(), (int)(sec / 60), (int)(sec % 60));
    _canvas->setCursor(x + 8, y + 24);
    _canvas->print(buf);

    snprintf(buf, sizeof(buf), "AI作弊 %d 次  最险 %d 步", _cheat_count, (_min_danger == 999) ? 0 : _min_danger);
    _canvas->setCursor(x + 8, y + 38);
    _canvas->print(buf);

    snprintf(buf, sizeof(buf), "连胜 %d   最高 %d", s_streak, s_best_streak);
    _canvas->setCursor(x + 8, y + 52);
    _canvas->print(buf);

    // 复盘：这局你有几手没下在要点上
    if (_worst_move_no > 0) {
        snprintf(buf, sizeof(buf), "失误 %d 手  最大第 %d 手", _blunders, _worst_move_no);
    } else {
        snprintf(buf, sizeof(buf), "失误 0 手  全程无破绽");
    }
    _canvas->setTextColor(_blunders == 0 ? TFT_GREEN : TFT_LIGHTGREY);
    _canvas->setCursor(x + 8, y + 66);
    _canvas->print(buf);

    // 成就
    static const char* kAchvName[6] = { "首胜", "反杀", "屠龙", "三连", "零作弊", "无失误" };
    int ax = x + 8, ay = y + 82;
    bool any = false;
    _canvas->setTextColor(TFT_YELLOW);
    for (int i = 0; i < 6; i++) {
        if (!(_achv_new & (1 << i))) continue;
        int tw = _canvas->textWidth(kAchvName[i]);
        if (ax + tw > x + w - 8) { ax = x + 8; ay += 14; }
        _canvas->setCursor(ax, ay);
        _canvas->print(kAchvName[i]);
        ax += tw + 8;
        any = true;
    }
    if (!any) {
        _canvas->setTextColor(TFT_DARKGREY);
        _canvas->setCursor(x + 8, y + 86);
        _canvas->print("R 重开  V 回放  L 难度");
    }
}

// ================= 智力测试（评估模式）=================

// 逐手采集: 悔手度 = 最优候选走完后的局面分 - 本手走完后的局面分。
// 这个口径天然会给"挡住对手成五点"高分(挡完对手分下降、局面分上升),
// 不会再像 reviewPlayerMove 那样把正确防守手判成最大失误。
void AppGomoku::assessPlayerMove(int r, int c) {
    if (_game_over || _board[r][c] != 0) return;

    // 1) 思考时间
    int32_t think = (int32_t)(GetHAL().millis() - _turn_start_ms);
    if (think < 0) think = 0;
    if (think > 60000) think = 60000;
    _assess_think_sum += think;
    if (think > _assess_think_max) _assess_think_max = think;

    // 2) 走之前对手/自己的成五点数量（判断该不该挡、有没有新做出进攻点）
    int ar, ac;
    int wp_before = countWinPoints(2, ar, ac);
    int my_wp_before = countWinPoints(1, ar, ac);

    // 3) 枚举候选, 找最优局面分
    int R[24], C[24];
    int n = genMoves(R, C, 24, 1);
    bool has_best = false;
    int32_t best = 0;
    for (int i = 0; i < n; i++) {
        if (R[i] == r && C[i] == c) continue;
        makeMove(R[i], C[i], 1);
        int32_t v = _score[1] - _score[2];
        unmakeMove(R[i], C[i], 1);
        if (!has_best || v > best) { best = v; has_best = true; }
    }

    // 4) 本手走完后的局面分 + 双方成五点变化
    makeMove(r, c, 1);
    int32_t played = _score[1] - _score[2];
    int wp_after = countWinPoints(2, ar, ac);   // 走完后对手还剩几个成五点
    int my_wp = countWinPoints(1, ar, ac);      // 自己是否做出了成五点(主动进攻)
    unmakeMove(r, c, 1);

    if (has_best) {
        int32_t regret = best - played;
        if (regret < 0) regret = 0;
        _assess_regret_sum += regret;
        if (regret > _assess_regret_max) _assess_regret_max = regret;
        if (regret >= 1500) _blunders++;        // 与普通模式同一个失误阈值
        if (regret > _worst_loss) {
            _worst_loss = regret;
            _worst_move_no = _assess_moves + 1;
        }
    }

    if (wp_before > 0) {
        _assess_def_need++;
        if (wp_after < wp_before) _assess_def_ok++;
    }
    if (my_wp > my_wp_before) _assess_make4++;   // 只有"新增"进攻点才算进攻

    if (_assess_moves > 0) {
        int32_t d = played - _assess_last_diff;
        _assess_swing += (d < 0) ? -d : d;
    }
    _assess_last_diff = played;
    _assess_moves++;
}

void AppGomoku::computeAssessment() {
    int n = (_assess_moves > 0) ? _assess_moves : 1;

    // 进攻: 主动做出成五点的频率
    _assess_atk = 20 + _assess_make4 * 9;
    if (_assess_atk > 100) _assess_atk = 100;

    // 防守: 对手有成五点时的化解率; 全程没被威胁就到 60 分基准
    if (_assess_def_need > 0) _assess_def = (100 * _assess_def_ok) / _assess_def_need;
    else _assess_def = 60;

    // 效率: 悔手度 + 平均思考时间
    int32_t regret_avg = _assess_regret_sum / n;
    int32_t think_avg = _assess_think_sum / n;
    int regret_pen = (int)(regret_avg / 40);
    if (regret_pen > 45) regret_pen = 45;
    int think_pen = (think_avg > 3000) ? (int)((think_avg - 3000) / 200) : 0;
    if (think_pen > 35) think_pen = 35;
    _assess_eff = 100 - regret_pen - think_pen;
    if (_assess_eff < 5) _assess_eff = 5;

    // 稳定: 单步最大悔手度 + 失误次数 + 局面分抖动
    int32_t swing_avg = _assess_swing / n;
    int regret_max_pen = (int)(_assess_regret_max / 50);
    if (regret_max_pen > 40) regret_max_pen = 40;
    int swing_pen = (int)(swing_avg / 80);
    if (swing_pen > 35) swing_pen = 35;
    _assess_sta = 100 - regret_max_pen - _blunders * 6 - swing_pen;
    if (_assess_sta < 5) _assess_sta = 5;
    if (_assess_sta > 100) _assess_sta = 100;

    _assess_total = (_assess_atk * 30 + _assess_def * 30 + _assess_eff * 25 + _assess_sta * 15) / 100;

    // 用实战结果校准(赢 +4 / 输 -4)
    if (_winner == 1) _assess_total += 4;
    else if (_winner == 2) _assess_total -= 4;
    if (_assess_total < 0) _assess_total = 0;
    if (_assess_total > 100) _assess_total = 100;

    _assess_iq = 60 + _assess_total * 8 / 10;   // 60 ~ 140

    if (_assess_total >= 90) _assess_rank = "王者";
    else if (_assess_total >= 80) _assess_rank = "钻石";
    else if (_assess_total >= 70) _assess_rank = "铂金";
    else if (_assess_total >= 60) _assess_rank = "黄金";
    else if (_assess_total >= 50) _assess_rank = "白银";
    else _assess_rank = "青铜";

    ESP_LOGI("ASSESS",
             "iq=%d rank=%s total=%d | atk=%d def=%d(%d/%d) eff=%d sta=%d | moves=%d "
             "regret_avg=%d regret_max=%d think_avg=%dms think_max=%dms blunders=%d winner=%d",
             _assess_iq, _assess_rank, _assess_total,
             _assess_atk, _assess_def, _assess_def_ok, _assess_def_need,
             _assess_eff, _assess_sta,
             _assess_moves, (int)regret_avg, (int)_assess_regret_max,
             (int)think_avg, (int)_assess_think_max, _blunders, _winner);
}

void AppGomoku::drawAssessResult() {
    if (!_canvas) return;

    int w = 214, h = 112;
    int x = (_canvas->width() - w) / 2;
    int y = (_canvas->height() - h) / 2;

    _canvas->fillRect(x, y, w, h, 0x1082);
    _canvas->drawRect(x, y, w, h, TFT_CYAN);
    _canvas->drawFastHLine(x + 2, y + 20, w - 4, 0x4208);

    _canvas->setFont(&fonts::efontCN_14);
    _canvas->setTextDatum(middle_center);
    _canvas->setTextColor(TFT_CYAN);
    _canvas->drawString("智力测试", x + w / 2, y + 11);
    _canvas->setTextDatum(top_left);

    _canvas->setFont(&fonts::efontCN_12);
    char buf[48];

    _canvas->setTextColor(TFT_YELLOW);
    snprintf(buf, sizeof(buf), "IQ %d   段位 %s", _assess_iq, _assess_rank);
    _canvas->setCursor(x + 8, y + 23);
    _canvas->print(buf);

    _canvas->setTextColor(TFT_WHITE);
    snprintf(buf, sizeof(buf), "进攻 %d    防守 %d", _assess_atk, _assess_def);
    _canvas->setCursor(x + 8, y + 39);
    _canvas->print(buf);

    snprintf(buf, sizeof(buf), "效率 %d    稳定 %d", _assess_eff, _assess_sta);
    _canvas->setCursor(x + 8, y + 53);
    _canvas->print(buf);

    int32_t avg_think = (_assess_moves > 0) ? _assess_think_sum / _assess_moves : 0;
    snprintf(buf, sizeof(buf), "手数 %d   均思 %d.%ds", _assess_moves,
             (int)(avg_think / 1000), (int)((avg_think % 1000) / 100));
    _canvas->setCursor(x + 8, y + 67);
    _canvas->print(buf);

    snprintf(buf, sizeof(buf), "失误 %d 手   悔手均 %d", _blunders,
             (_assess_moves > 0) ? (int)(_assess_regret_sum / _assess_moves) : 0);
    _canvas->setCursor(x + 8, y + 81);
    _canvas->print(buf);

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(TFT_DARKGREY);
    _canvas->setCursor(x + 8, y + 97);
    _canvas->print("R 重开   V 回放   I 退出测试");
}

// 作弊问责选择题(画在棋盘上, 由 app_task 统一绘制)
void AppGomoku::drawCheatDialog() {
    if (!_canvas || !_canvas->getBuffer()) return;

    int w = 200, h = 64;
    int x = (_canvas->width() - w) / 2;
    int y = (_canvas->height() - h) / 2;

    _canvas->fillRect(x, y, w, h, 0x0000);
    _canvas->drawRect(x, y, w, h, TFT_RED);
    _canvas->drawRect(x + 1, y + 1, w - 2, h - 2, 0x2104);

    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextDatum(middle_center);
    _canvas->setTextColor(TFT_WHITE, 0x0000);
    _canvas->drawString("你刚刚是不是按了个什么按键？", x + w / 2, y + 12);

    const int bw = 54, bh = 18, by = y + 24;
    int yx = x + 20;
    int nx = x + w - 20 - bw;

    bool yes_sel = (_cheat_sel == 0);
    _canvas->fillRect(yx, by, bw, bh, yes_sel ? TFT_YELLOW : 0x18E3);
    _canvas->drawRect(yx, by, bw, bh, yes_sel ? TFT_WHITE : TFT_DARKGREY);
    _canvas->setTextColor(yes_sel ? TFT_BLACK : TFT_WHITE, yes_sel ? TFT_YELLOW : 0x18E3);
    _canvas->drawString("YES", yx + bw / 2, by + bh / 2);

    bool no_sel = (_cheat_sel == 1);
    _canvas->fillRect(nx, by, bw, bh, no_sel ? TFT_YELLOW : 0x18E3);
    _canvas->drawRect(nx, by, bw, bh, no_sel ? TFT_WHITE : TFT_DARKGREY);
    _canvas->setTextColor(no_sel ? TFT_BLACK : TFT_WHITE, no_sel ? TFT_YELLOW : 0x18E3);
    _canvas->drawString("NO", nx + bw / 2, by + bh / 2);

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor((_cheat_alert == 1) ? TFT_ORANGE : TFT_LIGHTGREY, 0x0000);
    _canvas->drawString((_cheat_alert == 1) ? "AI 正在盯着你…（禁止落子）" : "左右选择   Enter 确认",
                        x + w / 2, y + h - 10);

    _canvas->setTextDatum(top_left);
}

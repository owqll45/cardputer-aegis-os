#include "app_doudizhu.h"
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <apps/utils/theme.h>
#include <apps/utils/common.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <esp_random.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ================= 嘴炮台词分类 =================
// 与五子棋同一套机制(台词表 + 随机 + 红字 1px 黑描边)，但斗地主的落点不同：
// 五子棋在右栏两行竖排；这里放在牌桌上方正中单行(120,36)，不占出牌区和手牌区。
enum { DZT_AI_PLAY, DZT_PASS, DZT_NEAR_WIN, DZT_AI_NEAR, DZT_HUMAN_WIN, DZT_AI_WIN, DZT_CHEAT };

// ================= 按键队列（回调只入队，状态只在 app 任务里改）=================
#define DZQ_N 16
static std::function<void(const Keyboard::KeyEvent_t&)> s_key_handler;
static Keyboard::KeyEvent_t s_keyq[DZQ_N];
static volatile int s_q_head = 0;
static volatile int s_q_tail = 0;
static portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;

static void dzq_push(const Keyboard::KeyEvent_t& e)
{
    taskENTER_CRITICAL(&s_q_mux);
    int n = (s_q_head + 1) % DZQ_N;
    if (n != s_q_tail) {
        s_keyq[s_q_head] = e;
        s_q_head         = n;
    }
    taskEXIT_CRITICAL(&s_q_mux);
}

static bool dzq_pop(Keyboard::KeyEvent_t& e)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_q_mux);
    if (s_q_tail != s_q_head) {
        e        = s_keyq[s_q_tail];
        s_q_tail = (s_q_tail + 1) % DZQ_N;
        ok       = true;
    }
    taskEXIT_CRITICAL(&s_q_mux);
    return ok;
}

static bool keyIs(const char* name, char ch)
{
    char c = name[0];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return c == ch;
}

// ================= 基础 =================

static const int kPlayerColor[3] = { TFT_WHITE, TFT_CYAN, TFT_YELLOW };
static const char* kPlayerName[3] = { "我", "P2", "AI" };

AppDoudizhu::AppDoudizhu()
{
    setAppInfo().name = "Doudizhu";
    setAppInfo().icon = nullptr;
}

AppDoudizhu::~AppDoudizhu()
{
    _canvas = nullptr;
}

void AppDoudizhu::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "open");
    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    /* 战绩跨"退出/重进"保留: 以前只写在成员变量里, 但 app 关闭时实例会被销毁,
     * 重进必然回到 0/0/0(实机验证过)。改为落 NVS, 才能真正跨退出保留。 */
    for (int i = 0; i < 3; i++) _hand_n[i] = 0;
    {
        auto& st = GetHAL().getSettings();
        _score[0] = st.GetInt("ddz_s0", 0);
        _score[1] = st.GetInt("ddz_s1", 0);
        _score[2] = st.GetInt("ddz_s2", 0);
        _round    = st.GetInt("ddz_round", 0);
    }

    s_key_handler = [](const Keyboard::KeyEvent_t& k) {
        if (k.isModifier) return;
        dzq_push(k);
    };
    _handle_key_event_slot_id = -1;
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(s_key_handler);

    glink_acquire("Doudizhu", GLINK_GID_DOUDIZHU, GLINK_ICON_GENERIC);

    resetRound();
}

void AppDoudizhu::onRunning()
{
    glink_poll();
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }

    processKeys();
    remoteInput();

    uint32_t now = GetHAL().millis();

    // 叫地主：AI / 托管
    if (_phase == PH_BID) {
        int who = _bid_turn;
        if (!isHuman(who)) {
            if ((int32_t)(now - _ai_wait_until) >= 0) {
                aiBid();
                _ai_wait_until = now + 700;
            }
        } else if (who == 1 && !_two_human && (int32_t)(now - _p2_deadline) >= 0) {
            aiBid();
            _p2_deadline = now + 7000;
        }
    } else if (_phase == PH_PLAY) {
        if (!isHuman(_cur)) {
            if ((int32_t)(now - _ai_wait_until) >= 0) {
                aiAct();
                _ai_wait_until = now + 650;
            }
        } else if (_cur == 1 && !_two_human && (int32_t)(now - _p2_deadline) >= 0) {
            aiAct(true);
            _p2_deadline = now + 7000;
        }
    } else if (_phase == PH_ACCUSE) {
        // 等玩家回答
    }

    render();
}

void AppDoudizhu::onClose()
{
    glink_release();
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
    s_key_handler = nullptr;
    Keyboard::KeyEvent_t dump;
    while (dzq_pop(dump)) {
    }
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

// ================= 发牌 / 叫地主 =================

void AppDoudizhu::resetRound()
{
    _round++;
    _landlord   = -1;
    _leader     = -1;
    _last_player = -1;
    _pass_count = 0;
    _last_combo = Combo();
    _last_n     = 0;
    _cursor     = 0;
    _cheat_menu = 0;
    _reveal     = false;
    _suspicion  = 0;
    _cheat_used = false;
    _ai_angry   = false;
    _msg[0]     = 0;
    _msg_until  = 0;
    for (int i = 0; i < MAXHAND; i++) _sel[i] = false;

    dealCards();
    startBid();
}

void AppDoudizhu::dealCards()
{
    int deck[54];
    for (int i = 0; i < 54; i++) deck[i] = i;
    for (int i = 53; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        int t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    for (int p = 0; p < 3; p++) {
        _hand_n[p] = 17;
        for (int i = 0; i < 17; i++) {
            _hand[p][i] = deck[p * 17 + i];
        }
        sortHand(_hand[p], _hand_n[p]);
    }
    for (int i = 0; i < 3; i++) _bottom[i] = deck[51 + i];

    _phase = PH_DEAL;
}

void AppDoudizhu::startBid()
{
    _phase     = PH_BID;
    _bid_turn  = (int)(esp_random() % 3);
    _bid_count = 0;
    _ai_wait_until = GetHAL().millis() + 700;
    showMsg("叫地主阶段", 1200);
}

void AppDoudizhu::bidAction(int who, bool call)
{
    if (_phase != PH_BID) return;

    if (call) {
        _landlord = who;
        for (int i = 0; i < 3; i++) {
            if (_hand_n[who] < MAXHAND) {
                _hand[who][_hand_n[who]++] = _bottom[i];
            }
        }
        sortHand(_hand[who], _hand_n[who]);
        beginPlay();
        char buf[48];
        snprintf(buf, sizeof(buf), "%s 当地主，底牌 %s %s %s", kPlayerName[who],
                 rankName(cardRank(_bottom[0])), rankName(cardRank(_bottom[1])),
                 rankName(cardRank(_bottom[2])));
        showMsg(buf, 2000);
        return;
    }

    _bid_count++;
    if (_bid_count >= 3) {
        showMsg("都不叫，重新发牌", 1200);
        resetRound();
        return;
    }
    _bid_turn = (who + 1) % 3;
    if (!isHuman(_bid_turn)) _ai_wait_until = GetHAL().millis() + 700;
}

void AppDoudizhu::beginPlay()
{
    _phase     = PH_PLAY;
    _cur       = _landlord;
    _leader    = -1;
    _last_combo = Combo();
    _last_n    = 0;
    _pass_count = 0;
    _ai_wait_until = GetHAL().millis() + 800;
    _cursor    = 0;
    if (_cur == 1) _p2_deadline = GetHAL().millis() + 7000;
    for (int i = 0; i < MAXHAND; i++) _sel[i] = false;
}

// ================= 出牌流程 =================

void AppDoudizhu::nextTurn()
{
    _cur = (_cur + 1) % 3;
    if (_cur == 1) _p2_deadline = GetHAL().millis() + 7000;
    if (!isHuman(_cur)) _ai_wait_until = GetHAL().millis() + 650;
}

void AppDoudizhu::playCards(int who, const int* cards, int n)
{
    Combo c;
    if (!buildCombo(cards, n, c)) return;
    if (!comboBeats(c, _last_combo)) return;

    if (!removeCards(who, cards, n)) return;

    _last_combo = c;
    _last_n     = n;
    for (int i = 0; i < n; i++) _last_cards[i] = cards[i];
    _last_player = who;
    _leader      = who;
    _pass_count  = 0;

    if (who != 0 && (esp_random() % 100) < 55) showTaunt(0);
    if (who == 0 && _hand_n[0] <= 3) showTaunt(2);
    if (who != 0 && _hand_n[who] <= 3) showTaunt(3);

    if (_hand_n[who] <= 0) {
        finishGame(who);
        return;
    }
    nextTurn();
}

void AppDoudizhu::passTurn(int who)
{
    if (_phase != PH_PLAY) return;
    (void)who;
    if (who == 0 && (esp_random() % 100) < 45) showTaunt(1);
    _pass_count++;
    if (_pass_count >= 2) {
        // 两家不要 → 最后出牌者重新自由出
        _cur        = (_leader >= 0) ? _leader : ((_cur + 1) % 3);
        _last_combo = Combo();
        _last_n     = 0;
        _pass_count = 0;
        if (_cur == 1) _p2_deadline = GetHAL().millis() + 7000;
        if (!isHuman(_cur)) _ai_wait_until = GetHAL().millis() + 650;
    } else {
        nextTurn();
    }
}

void AppDoudizhu::finishGame(int winner)
{
    _phase = PH_OVER;
    _winner = winner;
    bool landlord_win = (winner == _landlord);
    if (landlord_win) {
        _score[_landlord] += 2;
        for (int i = 0; i < 3; i++)
            if (i != _landlord) _score[i] -= 1;
    } else {
        _score[_landlord] -= 2;
        for (int i = 0; i < 3; i++)
            if (i != _landlord) _score[i] += 1;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%s 出完！%s", kPlayerName[winner], landlord_win ? "地主胜" : "农民胜");
    showMsg(buf, 4000);
    showTaunt(winner == 0 ? 4 : 5);

    // 战绩落 NVS(实例销毁后还能保留)
    {
        auto& st = GetHAL().getSettings();
        st.SetInt("ddz_s0", _score[0]);
        st.SetInt("ddz_s1", _score[1]);
        st.SetInt("ddz_s2", _score[2]);
        st.SetInt("ddz_round", _round);
        st.Sync();
    }
}

// ---- AI ----

void AppDoudizhu::aiBid()
{
    int who = _bid_turn;
    int cnt[15] = {0};
    for (int i = 0; i < _hand_n[who]; i++) cnt[cardRank(_hand[who][i])]++;

    int strength = cnt[14] * 4 + cnt[13] * 3 + cnt[12] * 2;
    for (int r = 0; r <= 12; r++)
        if (cnt[r] == 4) strength += 5;

    // 前两家有人叫过就不抢（简化：只叫一轮）
    bool call = strength >= 6;
    bidAction(who, call);
}

void AppDoudizhu::aiAct(bool force)
{
    if (_phase != PH_PLAY) return;
    if (isHuman(_cur) && !force) return;

    int out[MAXHAND];
    int n = 0;
    int before = _cur;

    // 出牌失败兜底：不能让 AI 停在自己的回合上（否则整局卡死）
    auto safe_play = [&]() {
        if (n <= 0) {
            passTurn(_cur);
            return;
        }
        playCards(_cur, out, n);
        if (_phase == PH_PLAY && _cur == before) passTurn(_cur);
    };

    // 上一手是队友出的，且不是自由出牌 → 让牌（除非能一把走完）
    if (_last_combo.type != CT_NONE && _leader >= 0 && _leader != _cur &&
        isTeammate(_cur, _leader)) {
        if (!canFinishInOne(_cur, out, n)) {
            passTurn(_cur);
            return;
        }
        safe_play();
        return;
    }

    if (_last_combo.type == CT_NONE) {
        if (canFinishInOne(_cur, out, n)) {
            safe_play();
            return;
        }
        if (!findLead(_cur, out, n)) {
            passTurn(_cur);
            return;
        }
        safe_play();
        return;
    }

    if (findBeat(_cur, _last_combo, out, n)) {
        safe_play();
    } else {
        passTurn(_cur);
    }
}

bool AppDoudizhu::canFinishInOne(int who, int* out, int& out_n) const
{
    Combo c;
    if (!buildCombo(_hand[who], _hand_n[who], c)) return false;
    // 自由出牌或能压过才算出完
    if (_last_combo.type != CT_NONE && !comboBeats(c, _last_combo)) return false;
    out_n = _hand_n[who];
    for (int i = 0; i < out_n; i++) out[i] = _hand[who][i];
    return true;
}

bool AppDoudizhu::isHuman(int who) const
{
    if (who == 0) return true;
    if (who == 1) {
        if (!glink_is_linked()) return false;
        /* 双人对战: 连着手柄就把 P2 交给真人(不等它先动);
         * 三人局仍按"手柄真的操作过"才接管。 */
        if (_two_human) return (glink_remote_flags() & 1) != 0;
        return (glink_remote_flags() & 1) && _p2_manual;
    }
    return false;
}

bool AppDoudizhu::isTeammate(int a, int b) const
{
    if (a == _landlord || b == _landlord) return a == b;
    return true;
}

// ================= 牌型判定 =================

int AppDoudizhu::cardRank(int c)
{
    if (c == 52) return 13;
    if (c == 53) return 14;
    return c % 13;
}

int AppDoudizhu::cardSuit(int c)
{
    return c >= 52 ? 4 : c / 13;
}

const char* AppDoudizhu::rankName(int r)
{
    static const char* n[15] = { "3", "4", "5", "6", "7", "8", "9", "10",
                                 "J", "Q", "K", "A", "2", "小王", "大王" };
    if (r < 0 || r > 14) return "?";
    return n[r];
}

const char* AppDoudizhu::suitName(int c)
{
    static const char* s[5] = { "S", "H", "C", "D", "J" };
    return s[cardSuit(c)];
}

void AppDoudizhu::sortHand(int* h, int n)
{
    for (int i = 1; i < n; i++) {
        int v = h[i];
        int rv = cardRank(v);
        int j = i - 1;
        while (j >= 0 && (cardRank(h[j]) > rv || (cardRank(h[j]) == rv && h[j] > v))) {
            h[j + 1] = h[j];
            j--;
        }
        h[j + 1] = v;
    }
}

bool AppDoudizhu::removeCards(int who, const int* cards, int n)
{
    bool used[MAXHAND] = { false };
    int hn = _hand_n[who];
    for (int i = 0; i < n; i++) {
        bool found = false;
        for (int j = 0; j < hn; j++) {
            if (!used[j] && _hand[who][j] == cards[i]) {
                used[j] = true;
                found   = true;
                break;
            }
        }
        if (!found) return false;
    }
    int w = 0;
    for (int j = 0; j < hn; j++) {
        if (!used[j]) _hand[who][w++] = _hand[who][j];
    }
    _hand_n[who] = w;
    return true;
}

bool AppDoudizhu::buildCombo(const int* cards, int n, Combo& out)
{
    out = Combo();
    if (n <= 0 || n > MAXHAND) return false;

    int cnt[15] = {0};
    for (int i = 0; i < n; i++) {
        int r = cardRank(cards[i]);
        if (r < 0 || r > 14) return false;
        cnt[r]++;
    }

    if (n == 1) {
        out.type = CT_SINGLE;
        out.rank = cardRank(cards[0]);
        out.n    = 1;
        return true;
    }
    if (n == 2) {
        if (cnt[13] == 1 && cnt[14] == 1) {
            out.type = CT_ROCKET;
            out.rank = 15;
            out.n    = 2;
            return true;
        }
        for (int r = 0; r <= 12; r++) {
            if (cnt[r] == 2) {
                out.type = CT_PAIR;
                out.rank = r;
                out.n    = 2;
                return true;
            }
        }
        return false;
    }
    if (n == 3) {
        for (int r = 0; r <= 12; r++) {
            if (cnt[r] == 3) {
                out.type = CT_TRIO;
                out.rank = r;
                out.n    = 3;
                return true;
            }
        }
        return false;
    }
    if (n == 4) {
        for (int r = 0; r <= 12; r++) {
            if (cnt[r] == 4) {
                out.type = CT_BOMB;
                out.rank = r;
                out.n    = 4;
                return true;
            }
            if (cnt[r] == 3) {
                out.type = CT_TRIO1;
                out.rank = r;
                out.n    = 4;
                return true;
            }
        }
        return false;
    }
    if (n == 5) {
        for (int r = 0; r <= 12; r++) {
            if (cnt[r] != 3) continue;
            for (int p = 0; p <= 12; p++) {
                if (p != r && cnt[p] == 2) {
                    out.type = CT_TRIO2;
                    out.rank = r;
                    out.n    = 5;
                    return true;
                }
            }
        }
    }

    // 顺子（≥5 张单牌连续，最大到 A）
    if (n >= 5) {
        bool allOne = true;
        int total = 0, lo = -1, hi = -1;
        for (int r = 0; r < 15; r++) {
            if (!cnt[r]) continue;
            total += cnt[r];
            if (cnt[r] != 1 || r > 11) allOne = false;
            if (lo < 0) lo = r;
            hi = r;
        }
        if (allOne && total == n && hi - lo + 1 == n) {
            out.type = CT_STRAIGHT;
            out.rank = hi;
            out.len  = n;
            out.n    = n;
            return true;
        }
    }

    // 连对（≥3 连对）
    if (n >= 6 && n % 2 == 0) {
        int pairs = 0, lo = -1, hi = -1;
        bool ok = true;
        for (int r = 0; r < 15; r++) {
            if (!cnt[r]) continue;
            if (cnt[r] != 2 || r > 11) {
                ok = false;
            } else {
                pairs++;
                if (lo < 0) lo = r;
                hi = r;
            }
        }
        if (ok && pairs * 2 == n && hi - lo + 1 == pairs) {
            out.type = CT_PAIRS;
            out.rank = hi;
            out.len  = pairs;
            out.n    = n;
            return true;
        }
    }

    // 纯飞机（连续三张，≥2 组，不带牌）
    if (n >= 6 && n % 3 == 0) {
        int k = n / 3;
        for (int s = 0; s + k - 1 <= 11; s++) {
            bool ok = true;
            for (int r = s; r < s + k; r++)
                if (cnt[r] != 3) ok = false;
            if (!ok) continue;
            bool extra = false;
            for (int r = 0; r < 15; r++)
                if ((r < s || r >= s + k) && cnt[r]) extra = true;
            if (!extra) {
                out.type = CT_PLANE;
                out.rank = s + k - 1;
                out.len  = k;
                out.n    = n;
                return true;
            }
        }
    }

    // 飞机带单 / 带对
    for (int k = 2; k <= 6; k++) {
        bool wing1 = (n == k * 4);
        bool wing2 = (n == k * 5);
        if (!wing1 && !wing2) continue;

        for (int s = 0; s + k - 1 <= 11; s++) {
            bool ok = true;
            for (int r = s; r < s + k; r++)
                if (cnt[r] != 3) ok = false;
            if (!ok) continue;

            int rest[15];
            memcpy(rest, cnt, sizeof(rest));
            for (int r = s; r < s + k; r++) rest[r] = 0;

            if (wing1) {
                int rc = 0;
                bool good = true;
                for (int r = 0; r < 15; r++) {
                    rc += rest[r];
                    if (rest[r] > 2) good = false;
                }
                if (good && rc == k) {
                    out.type = CT_PLANE1;
                    out.rank = s + k - 1;
                    out.len  = k;
                    out.n    = n;
                    return true;
                }
            } else {
                int pairs = 0;
                bool good = true;
                for (int r = 0; r < 15; r++) {
                    if (!rest[r]) continue;
                    if (rest[r] == 2) pairs++;
                    else good = false;
                }
                if (good && pairs == k) {
                    out.type = CT_PLANE2;
                    out.rank = s + k - 1;
                    out.len  = k;
                    out.n    = n;
                    return true;
                }
            }
        }
    }

    // 四带二 / 四带两对
    if (n == 6 || n == 8) {
        for (int r = 0; r <= 12; r++) {
            if (cnt[r] != 4) continue;
            int rest[15];
            memcpy(rest, cnt, sizeof(rest));
            rest[r] = 0;
            if (n == 6) {
                int rc = 0;
                bool good = true;
                for (int p = 0; p < 15; p++) {
                    rc += rest[p];
                    if (rest[p] > 2) good = false;
                }
                if (good && rc == 2) {
                    out.type = CT_FOUR2;
                    out.rank = r;
                    out.n    = 6;
                    return true;
                }
            } else {
                int pairs = 0;
                bool good = true;
                for (int p = 0; p < 15; p++) {
                    if (!rest[p]) continue;
                    if (rest[p] == 2) pairs++;
                    else good = false;
                }
                if (good && pairs == 2) {
                    out.type = CT_FOUR22;
                    out.rank = r;
                    out.n    = 8;
                    return true;
                }
            }
        }
    }

    return false;
}

bool AppDoudizhu::comboBeats(const Combo& a, const Combo& b)
{
    if (a.type == CT_NONE) return false;
    if (b.type == CT_NONE) return true;
    if (a.type == CT_ROCKET) return true;
    if (b.type == CT_ROCKET) return false;
    if (a.type == CT_BOMB && b.type != CT_BOMB) return true;
    if (b.type == CT_BOMB && a.type != CT_BOMB) return false;

    if (a.type != b.type) return false;
    if (a.n != b.n) return false;
    if (a.type == CT_STRAIGHT || a.type == CT_PAIRS || a.type == CT_PLANE ||
        a.type == CT_PLANE1 || a.type == CT_PLANE2) {
        if (a.len != b.len) return false;
    }
    return a.rank > b.rank;
}

// ---- AI 找牌 ----

bool AppDoudizhu::findBeat(int who, const Combo& last, int* out, int& out_n) const
{
    out_n = 0;
    if (last.type == CT_NONE || last.type == CT_ROCKET) return false;

    int h[MAXHAND];
    int hn = _hand_n[who];
    memcpy(h, _hand[who], sizeof(int) * hn);

    int cnt[15] = {0};
    for (int i = 0; i < hn; i++) cnt[cardRank(h[i])]++;

    int picked = 0;
    bool used[MAXHAND] = { false };

    auto pick = [&](int rank, int k) -> bool {
        int got = 0;
        for (int i = 0; i < hn && got < k; i++) {
            if (!used[i] && cardRank(h[i]) == rank) {
                used[i] = true;
                out[picked++] = h[i];
                got++;
            }
        }
        return got == k;
    };

    switch (last.type) {
        case CT_SINGLE:
            for (int r = last.rank + 1; r < 15; r++) {
                if (cnt[r]) {
                    pick(r, 1);
                    out_n = picked;
                    return true;
                }
            }
            break;
        case CT_PAIR:
            for (int r = last.rank + 1; r <= 12; r++) {
                if (cnt[r] >= 2) {
                    pick(r, 2);
                    out_n = picked;
                    return true;
                }
            }
            break;
        case CT_TRIO:
        case CT_TRIO1:
        case CT_TRIO2: {
            int need = (last.type == CT_TRIO) ? 0 : (last.type == CT_TRIO1 ? 1 : 2);
            for (int r = last.rank + 1; r <= 12; r++) {
                if (cnt[r] < 3) continue;
                int ar = -1;
                if (need == 1) {
                    for (int q = 0; q <= 14; q++)
                        if (q != r && cnt[q] >= 1) {
                            ar = q;
                            break;
                        }
                } else if (need == 2) {
                    for (int q = 0; q <= 12; q++)
                        if (q != r && cnt[q] >= 2) {
                            ar = q;
                            break;
                        }
                }
                if (need && ar < 0) continue;
                pick(r, 3);
                if (need == 1) pick(ar, 1);
                if (need == 2) pick(ar, 2);
                out_n = picked;
                return true;
            }
        } break;
        case CT_STRAIGHT: {
            int len = last.len;
            for (int s = last.rank - len + 2; s + len - 1 <= 11; s++) {
                if (s < 0) continue;
                bool ok = true;
                for (int r = s; r < s + len; r++)
                    if (!cnt[r]) ok = false;
                if (!ok) continue;
                for (int r = s; r < s + len; r++) pick(r, 1);
                out_n = picked;
                return true;
            }
        } break;
        case CT_PAIRS: {
            int len = last.len;
            for (int s = last.rank - len + 2; s + len - 1 <= 11; s++) {
                if (s < 0) continue;
                bool ok = true;
                for (int r = s; r < s + len; r++)
                    if (cnt[r] < 2) ok = false;
                if (!ok) continue;
                for (int r = s; r < s + len; r++) pick(r, 2);
                out_n = picked;
                return true;
            }
        } break;
        case CT_PLANE:
        case CT_PLANE1:
        case CT_PLANE2: {
            int k = last.len;
            for (int s = last.rank - k + 2; s + k - 1 <= 11; s++) {
                if (s < 0) continue;
                bool ok = true;
                for (int r = s; r < s + k; r++)
                    if (cnt[r] < 3) ok = false;
                if (!ok) continue;

                // 先确认翅膀够不够（真正取牌在下面统一做）
                if (last.type == CT_PLANE1) {
                    int avail = 0;
                    for (int q = 0; q <= 14; q++) {
                        if (q >= s && q < s + k) continue;
                        avail += cnt[q];
                    }
                    if (avail < k) continue;
                } else if (last.type == CT_PLANE2) {
                    int pairs = 0;
                    for (int q = 0; q <= 12; q++) {
                        if (q >= s && q < s + k) continue;
                        if (cnt[q] >= 2) pairs++;
                    }
                    if (pairs < k) continue;
                }

                for (int r = s; r < s + k; r++) pick(r, 3);

                if (last.type == CT_PLANE1) {
                    int need = k;
                    for (int q = 0; q <= 14 && need > 0; q++) {
                        if (q >= s && q < s + k) continue;
                        while (cnt[q] > 0 && need > 0) {
                            if (!pick(q, 1)) break;
                            need--;
                        }
                    }
                } else if (last.type == CT_PLANE2) {
                    int need = k;
                    for (int q = 0; q <= 12 && need > 0; q++) {
                        if (q >= s && q < s + k) continue;
                        while (cnt[q] > 0 && need > 0) {
                            if (!pick(q, 2)) break;
                            need--;
                        }
                    }
                }
                out_n = picked;
                return true;
            }
        } break;
        case CT_FOUR2:
        case CT_FOUR22: {
            for (int r = last.rank + 1; r <= 12; r++) {
                if (cnt[r] != 4) continue;

                if (last.type == CT_FOUR2) {
                    int extra = 0;
                    for (int q = 0; q <= 14; q++) {
                        if (q == r) continue;
                        extra += cnt[q];
                    }
                    if (extra < 2) continue;
                    pick(r, 4);
                    int need = 2;
                    for (int q = 0; q <= 14 && need > 0; q++) {
                        if (q == r) continue;
                        while (cnt[q] > 0 && need > 0) {
                            if (!pick(q, 1)) break;
                            need--;
                        }
                    }
                } else {
                    // 四带两对：必须真的找两对不同点数
                    int pairs = 0;
                    for (int q = 0; q <= 12; q++) {
                        if (q != r && cnt[q] >= 2) pairs++;
                    }
                    if (pairs < 2) continue;
                    pick(r, 4);
                    int need = 2;
                    for (int q = 0; q <= 12 && need > 0; q++) {
                        if (q == r || cnt[q] < 2) continue;
                        if (!pick(q, 2)) continue;
                        need--;
                    }
                }
                out_n = picked;
                return true;
            }
        } break;
        case CT_BOMB: {
            for (int r = last.rank + 1; r <= 12; r++) {
                if (cnt[r] == 4) {
                    pick(r, 4);
                    out_n = picked;
                    return true;
                }
            }
        } break;
        default:
            break;
    }

    // 兜底：炸弹 / 王炸（对非炸弹牌型）
    if (last.type != CT_BOMB) {
        for (int r = 0; r <= 12; r++) {
            if (cnt[r] == 4) {
                pick(r, 4);
                out_n = picked;
                return true;
            }
        }
    }
    if (cnt[13] && cnt[14]) {
        pick(13, 1);
        pick(14, 1);
        out_n = picked;
        return true;
    }

    out_n = 0;
    return false;
}

bool AppDoudizhu::findLead(int who, int* out, int& out_n) const
{
    out_n = 0;
    int h[MAXHAND];
    int hn = _hand_n[who];
    memcpy(h, _hand[who], sizeof(int) * hn);

    int cnt[15] = {0};
    for (int i = 0; i < hn; i++) cnt[cardRank(h[i])]++;

    int picked = 0;
    auto pick = [&](int rank, int k) -> bool {
        int got = 0;
        for (int i = 0; i < hn && got < k; i++) {
            if (cardRank(h[i]) == rank) {
                out[picked++] = h[i];
                got++;
            }
        }
        return got == k;
    };

    // 手牌少时优先走干净
    if (hn <= 4) {
        Combo c;
        if (buildCombo(_hand[who], hn, c)) {
            out_n = hn;
            for (int i = 0; i < hn; i++) out[i] = _hand[who][i];
            return true;
        }
    }

    // 顺子（从低到高找最短的一手，至少 5 张）
    for (int len = 5; len <= 12 && len <= hn; len++) {
        for (int s = 0; s + len - 1 <= 11; s++) {
            bool ok = true;
            for (int r = s; r < s + len; r++)
                if (cnt[r] < 1) ok = false;
            if (!ok) continue;
            for (int r = s; r < s + len; r++) pick(r, 1);
            out_n = picked;
            return true;
        }
    }

    // 三带一
    for (int r = 0; r <= 12; r++) {
        if (cnt[r] < 3) continue;
        int ar = -1;
        for (int q = 0; q <= 14; q++) {
            if (q != r && cnt[q] >= 1) {
                ar = q;
                break;
            }
        }
        if (ar < 0) continue;
        pick(r, 3);
        pick(ar, 1);
        out_n = picked;
        return true;
    }

    // 对子
    for (int r = 0; r <= 12; r++) {
        if (cnt[r] >= 2) {
            pick(r, 2);
            out_n = picked;
            return true;
        }
    }

    // 单张（最小的）
    for (int r = 0; r <= 14; r++) {
        if (cnt[r] > 0) {
            pick(r, 1);
            out_n = picked;
            return true;
        }
    }

    out_n = 0;
    return false;
}

// ================= 作弊 =================

void AppDoudizhu::cheatReveal()
{
    _reveal  = true;
    _cheat_used = true;
    addSuspicion(12);
    showMsg("透视：对手手牌已显示");
    showTaunt(DZT_CHEAT);
}

void AppDoudizhu::cheatSwap()
{
    // 把自己最小的一张换成对手手上最大的一张（真正的“偷牌”）
    int best_who = -1, best_card = -1;
    int best_rank = -1;
    for (int p = 1; p < 3; p++) {
        for (int i = 0; i < _hand_n[p]; i++) {
            int r = cardRank(_hand[p][i]);
            if (r > best_rank) {
                best_rank = r;
                best_card = _hand[p][i];
                best_who  = p;
            }
        }
    }
    if (best_who < 0 || _hand_n[0] <= 0) {
        showMsg("没有可换的牌");
        return;
    }
    int worst_i = 0;
    for (int i = 1; i < _hand_n[0]; i++) {
        if (cardRank(_hand[0][i]) < cardRank(_hand[0][worst_i])) worst_i = i;
    }
    int my_card = _hand[0][worst_i];

    // 从对手手牌里移除 best_card，塞回我的烂牌
    int w = 0;
    for (int i = 0; i < _hand_n[best_who]; i++) {
        if (_hand[best_who][i] != best_card) _hand[best_who][w++] = _hand[best_who][i];
    }
    _hand_n[best_who] = w;
    _hand[best_who][_hand_n[best_who]++] = my_card;
    sortHand(_hand[best_who], _hand_n[best_who]);

    _hand[0][worst_i] = best_card;
    sortHand(_hand[0], _hand_n[0]);

    _cheat_used = true;
    addSuspicion(25);
    showMsg("换牌：偷来一张大牌");
    showTaunt(DZT_CHEAT);
}

void AppDoudizhu::cheatForcePass()
{
    // 让下两家强制要不起一次：把回合直接跳过
    if (_phase != PH_PLAY) {
        showMsg("现在不能强制要不起");
        return;
    }
    if (_last_combo.type == CT_NONE) {
        showMsg("自由出牌阶段无效");
        return;
    }
    _cheat_used = true;
    addSuspicion(20);
    _pass_count = 2;
    passTurn(_cur);
    showMsg("强制要不起");
    showTaunt(DZT_CHEAT);
}

void AppDoudizhu::addSuspicion(int v)
{
    _suspicion += v;
    if (_suspicion >= 40) triggerAccuse();
}

void AppDoudizhu::triggerAccuse()
{
    if (_phase == PH_ACCUSE) return;
    _phase     = PH_ACCUSE;
    _accuse_yes = false;
}

void AppDoudizhu::resolveAccuse(bool admit)
{
    _phase = PH_PLAY;
    if (admit) {
        _score[0] -= 1;
        _suspicion = 0;
        showMsg("AI：认了就行，下不为例", 2000);
    } else {
        _suspicion = 0;
        _ai_angry  = true;
        showMsg("AI：嘴硬？那我认真了", 2000);
    }
}

// ================= 输入 =================

void AppDoudizhu::processKeys()
{
    Keyboard::KeyEvent_t e;
    while (dzq_pop(e)) {
        if (e.isModifier) continue;
        if (e.state) handleKey(e.keyCode, e.keyName);
    }
}

void AppDoudizhu::handleKey(int keyCode, const char* keyName)
{
    if (keyCode == KEY_ESC) {
        if (_cheat_menu) {
            _cheat_menu = 0;
            return;
        }
        if (_phase == PH_OVER) {
            /* 结算面板写的是 "ESC 退出", 这里以前却 resetRound() —— 玩家想退出
             * 结果直接开下一局。与文案保持一致: ESC = 退出游戏。 */
            close();
            return;
        }
        close();
        return;
    }

    // 双人 / 三人模式切换(下一次轮到 P2 就生效)
    if (keyIs(keyName, 'm')) {
        _two_human = !_two_human;
        showMsg(_two_human ? "双人对战: 本机 + 手柄" : "三人局: 本机 + 手柄 + AI");
        return;
    }

    if (_phase == PH_ACCUSE) {
        if (keyCode == KEY_LEFT || keyCode == KEY_RIGHT) {
            _accuse_yes = !_accuse_yes;
        } else if (keyCode == KEY_ENTER || keyCode == KEY_Y) {
            resolveAccuse(_accuse_yes || keyCode == KEY_Y);
        } else if (keyCode == KEY_N) {
            resolveAccuse(false);
        }
        return;
    }

    if (_phase == PH_OVER) {
        if (keyCode == KEY_ENTER) resetRound();
        return;
    }

    if (_phase == PH_BID) {
        if (_bid_turn == 0) {
            /* 屏幕上写 Y/N, 但大多数人第一反应是按 Enter。Enter 按"叫地主"处理,
             * 不叫仍用 N, 避免按 Enter 毫无反应。 */
            if (keyCode == KEY_Y || keyCode == KEY_ENTER) bidAction(0, true);
            else if (keyCode == KEY_N) bidAction(0, false);
        }
        return;
    }

    if (_phase != PH_PLAY) return;

    // 作弊菜单
    if (_cheat_menu) {
        if (keyIs(keyName, '1')) {
            _cheat_menu = 0;
            cheatReveal();
        } else if (keyIs(keyName, '2')) {
            _cheat_menu = 0;
            cheatSwap();
        } else if (keyIs(keyName, '3')) {
            _cheat_menu = 0;
            cheatForcePass();
        } else if (keyIs(keyName, '4') || keyIs(keyName, 'c')) {
            _cheat_menu = 0;
        }
        return;
    }

    if (keyCode == KEY_C) {
        _cheat_menu = 1;
        return;
    }

    /* 没轮到自己时按键以前是"静默丢弃", 玩家会以为按键坏了/游戏卡了。
     * 只对出牌相关键给一句短提示, 避免乱按时刷屏。 */
    if (_cur != 0) {
        if (keyCode == KEY_ENTER || keyCode == KEY_SPACE ||
            keyCode == KEY_LEFT || keyCode == KEY_RIGHT ||
            keyIs(keyName, 'a') || keyIs(keyName, 'd')) {
            /* 700ms 一闪而过, 玩家基本看不到, 会以为按键坏了 */
            showMsg(isHuman(_cur) ? "还没轮到你，等 P2 出牌" : "还没轮到你，等 AI 出牌", 1500);
        }
        return;
    }

    if (keyCode == KEY_LEFT || keyIs(keyName, 'a') || keyIs(keyName, ',')) {
        moveCursor(-1);
    } else if (keyCode == KEY_RIGHT || keyIs(keyName, 'd') || keyIs(keyName, ';')) {
        moveCursor(1);
    } else if (keyCode == KEY_SPACE) {
        toggleSelect();
    } else if (keyCode == KEY_ENTER) {
        int out[MAXHAND];
        int n = 0;
        for (int i = 0; i < _hand_n[0]; i++)
            if (_sel[i]) out[n++] = _hand[0][i];
        if (n == 0) {
            showMsg("先选牌");
            return;
        }
        Combo c;
        if (!buildCombo(out, n, c)) {
            showMsg("不是合法牌型");
            return;
        }
        if (!comboBeats(c, _last_combo)) {
            showMsg("管不上");
            return;
        }
        for (int i = 0; i < MAXHAND; i++) _sel[i] = false;
        _cursor = 0;
        playCards(0, out, n);
    } else if (keyIs(keyName, 'b')) {
        if (_last_combo.type == CT_NONE) {
            showMsg("你是先手，必须出牌");
            return;
        }
        for (int i = 0; i < MAXHAND; i++) _sel[i] = false;
        passTurn(0);
    }
}

void AppDoudizhu::remoteInput()
{
    // 手柄按键序号一变，就把 P2 交给玩家；静默 7 秒后自动托管，避免卡住整局
    static uint32_t last_seq = 0;
    uint32_t seq = glink_btn_seq();
    if (seq != last_seq) {
        last_seq = seq;
        if (glink_is_linked() && (glink_remote_flags() & 1)) {
            _p2_manual  = true;
            _p2_deadline = GetHAL().millis() + 7000;
        }
    }

    if (!isHuman(1)) return;

    uint16_t b = glink_buttons();
    static uint16_t last_b = 0;
    uint16_t press = (uint16_t)(b & ~last_b);
    last_b = b;
    if (!press) return;

    if (_phase == PH_BID && _bid_turn == 1) {
        if (press & (1 << 4)) bidAction(1, true);    // A 叫
        if (press & (1 << 5)) bidAction(1, false);   // B 不叫
        return;
    }
    if (_phase != PH_PLAY || _cur != 1) return;

    if (press & (1 << 2)) moveCursor(-1);
    if (press & (1 << 3)) moveCursor(1);
    if (press & (1 << 4)) {
        toggleSelect();
    }
    if (press & (1 << 6)) {  // X 出牌
        int out[MAXHAND];
        int n = 0;
        for (int i = 0; i < _hand_n[1]; i++)
            if (_sel[i]) out[n++] = _hand[1][i];
        if (n > 0) {
            Combo c;
            if (buildCombo(out, n, c) && comboBeats(c, _last_combo)) {
                for (int i = 0; i < MAXHAND; i++) _sel[i] = false;
                _cursor = 0;
                playCards(1, out, n);
            }
        }
    }
    if (press & (1 << 5)) {  // B 不要
        for (int i = 0; i < MAXHAND; i++) _sel[i] = false;
        passTurn(1);
    }
}

void AppDoudizhu::moveCursor(int d)
{
    int n = _hand_n[0];
    if (n <= 0) return;
    _cursor = (_cursor + d + n) % n;
}

void AppDoudizhu::toggleSelect()
{
    if (_cursor < 0 || _cursor >= _hand_n[0]) return;
    _sel[_cursor] = !_sel[_cursor];
}

// ================= 绘制 =================

void AppDoudizhu::showMsg(const char* text, uint32_t ms)
{
    snprintf(_msg, sizeof(_msg), "%s", text);
    _msg_until = GetHAL().millis() + ms;
}

void AppDoudizhu::drawCard(int x, int y, int w, int h, int card, bool selected, bool faceDown)
{
    if (faceDown) {
        _canvas->fillRoundRect(x, y, w, h, 2, 0x0010);
        _canvas->drawRoundRect(x, y, w, h, 2, 0x7BEF);
        _canvas->drawFastHLine(x + 2, y + h / 2, w - 4, 0x001F);
        return;
    }
    uint16_t bg = selected ? 0xFFE0 : TFT_WHITE;
    _canvas->fillRoundRect(x, y, w, h, 2, bg);
    _canvas->drawRoundRect(x, y, w, h, 2, TFT_BLACK);

    int r = cardRank(card);
    bool red = (cardSuit(card) == 1 || cardSuit(card) == 3 || r >= 13);
    uint16_t fg = red ? TFT_RED : TFT_BLACK;

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(fg);
    if (r >= 13) {
        _canvas->setCursor(x + 2, y + 2);
        _canvas->print(r == 13 ? "小" : "大");
        _canvas->setCursor(x + 2, y + h - 10);
        _canvas->print("王");
    } else {
        const char* nm = rankName(r);
        _canvas->setCursor(x + 1, y + 2);
        _canvas->print(nm);
        _canvas->setCursor(x + 2, y + h - 10);
        _canvas->print(suitName(card));
    }
}

void AppDoudizhu::drawHand()
{
    int n = _hand_n[0];
    if (n <= 0) return;

    /* 手牌高度必须让开底部提示条(y=104 起): 原来牌高 30、底边到 118,
     * 提示条直接盖掉每张牌下半截的花色 —— 玩家只能看到点数, 分不清花色。
     * 改成 24 高、底边 101, 与提示条留出空隙; 选中抬起 6px 后仍不压桌面出牌区。 */
    int w = 20, h = 24;
    int step = (n > 1) ? (222 - w) / (n - 1) : w;
    if (step > w) step = w;
    int total = w + step * (n - 1);
    int x0 = (240 - total) / 2;

    int baseY = 77;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * step;
        int y = baseY - (_sel[i] ? 6 : 0);
        drawCard(x, y, w, h, _hand[0][i], _sel[i], false);
    }

    /* 光标框必须在所有牌画完后单独画: 以前是每张牌画完就画自己的绿框,
     * 下一张牌(step=10, 只错开半张)直接把右半边盖住, 玩家基本看不出焦点在哪。
     * 改成"上沿+下沿+左沿"三条亮黄粗线, 无论右边怎么叠都露在外面。 */
    if (_cursor >= 0 && _cursor < n) {
        int x = x0 + _cursor * step;
        int y = baseY - (_sel[_cursor] ? 6 : 0);
        _canvas->fillRect(x - 1, y - 3, w + 2, 2, TFT_YELLOW);
        _canvas->fillRect(x - 1, y + h + 1, w + 2, 2, TFT_YELLOW);
        _canvas->fillRect(x - 1, y, 2, h, TFT_YELLOW);
    }
}

void AppDoudizhu::drawTable()
{
    // 桌面出的牌
    if (_last_n > 0 && _last_combo.type != CT_NONE) {
        int n = _last_n;
        int w = 18, h = 24;
        int step = (n > 1) ? 14 : w;
        int total = w + step * (n - 1);
        if (total > 220) {
            step = (220 - w) / (n - 1);
            total = w + step * (n - 1);
        }
        int x0 = (240 - total) / 2;
        for (int i = 0; i < n; i++) {
            drawCard(x0 + i * step, 47, w, h, _last_cards[i], false, false);
        }
    } else if (_phase == PH_PLAY) {
        /* 原来用 0x7BEF 浅灰绿写在绿桌上, 实机几乎看不见 */
        _canvas->fillRect(92, 48, 56, 14, TFT_BLACK);
        _canvas->drawRect(92, 48, 56, 14, 0x7BEF);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setTextDatum(middle_center);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->drawString("自由出牌", 120, 55);
        _canvas->setTextDatum(top_left);
    }

    /* 自己的身份: 叫地主只闪一条 1.2 秒横幅, 之后整局都看不出自己是地主还是农民 */
    if (_phase == PH_PLAY && _landlord >= 0) {
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(_landlord == 0 ? TFT_YELLOW : TFT_LIGHTGREY);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString(_landlord == 0 ? "你是地主" : "你是农民", 120, 19);
        _canvas->setTextDatum(top_left);
    }

    // 对手信息
    _canvas->setFont(&fonts::efontCN_10);
    for (int p = 1; p < 3; p++) {
        int x = (p == 1) ? 4 : 176;
        int y = 15;
        _canvas->setTextColor(kPlayerColor[p]);
        _canvas->setCursor(x, y);
        _canvas->printf("%s %d", kPlayerName[p], _hand_n[p]);
        if (p == _landlord) {
            _canvas->setTextColor(TFT_RED);
            _canvas->print(" 地主");
        }
        if (_cur == p && _phase == PH_PLAY) {
            _canvas->setTextColor(TFT_GREEN);
            _canvas->print(" <<");
        }
    }

    // 作弊透视：两个对手各占一行(青色=P2, 黄色=AI, 与上方名字同色),
    // 不写前缀省宽度, 17 张牌才不会被右侧裁掉
    if (_reveal) {
        _canvas->setFont(&fonts::efontCN_10);
        for (int p = 1; p < 3; p++) {
            int y = (p == 1) ? 24 : 35;
            _canvas->setTextColor(kPlayerColor[p]);
            _canvas->setCursor(3, y);
            for (int i = 0; i < _hand_n[p]; i++) {
                _canvas->printf("%s", rankName(cardRank(_hand[p][i])));
                if (i + 1 < _hand_n[p]) _canvas->print(" ");
            }
        }
    }
}

void AppDoudizhu::drawHud()
{
    _canvas->fillRect(0, 0, 240, 13, 0x0841);

    // 左上角作弊标记
    if (_cheat_used) {
        _canvas->fillRect(0, 0, 44, 13, TFT_RED);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setCursor(3, 3);
        _canvas->print("CHEAT");
    }

    _canvas->setFont(&fonts::efontCN_10);
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(60, 3);
    _canvas->printf("DOUDIZHU R%d", _round);

    _canvas->setTextColor(_two_human ? TFT_CYAN : 0x7BEF);
    _canvas->setCursor(128, 3);
    _canvas->print(_two_human ? "2P" : "3P");

    _canvas->setTextColor(0x7BEF);
    _canvas->setCursor(160, 3);
    _canvas->printf("%d/%d/%d", _score[0], _score[1], _score[2]);
}

void AppDoudizhu::drawOverlay()
{
    uint32_t now = GetHAL().millis();

    // 我的操作提示
    if (_phase == PH_PLAY && _cur == 0) {
        _canvas->fillRect(0, 104, 240, 17, 0x0841);
        /* 保持 12px(10px 的"选/牌"会糊成一团), 只把两处动作说清楚, 实测整行 223px 放得下 */
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setTextColor(TFT_GREEN);
        _canvas->setCursor(4, 106);
        _canvas->print("A/D选牌 SPACE选中 ENTER出牌 B过 C作弊");
    } else if (_phase == PH_BID && _bid_turn == 0) {
        /* 手牌上移到 y=77 后, 叫地主面板底边必须让开, 否则压住牌顶的点数 */
        _canvas->fillRect(18, 45, 204, 30, 0x1082);
        _canvas->drawRect(18, 45, 204, 30, TFT_YELLOW);
        _canvas->setFont(&fonts::efontCN_12);   // 粗体笔画会粘在一起, 常规体更清楚
        _canvas->setTextDatum(middle_center);
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->drawString("轮到你叫地主", 120, 52);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(TFT_GREEN);
        _canvas->drawString("Enter/Y 叫地主   N 不叫", 120, 66);
        _canvas->setTextDatum(top_left);
    } else if (_phase == PH_BID) {
        _canvas->fillRect(0, 105, 240, 16, 0x0841);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(0x7BEF);
        _canvas->setCursor(4, 107);
        _canvas->printf("等待 %s 叫地主...", kPlayerName[_bid_turn]);
    } else if (_phase == PH_PLAY) {
        _canvas->fillRect(0, 105, 240, 16, 0x0841);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(0x7BEF);
        _canvas->setCursor(4, 107);
        _canvas->printf("等待 %s 出牌...", kPlayerName[_cur]);
    }

    // 消息条
    if (_msg[0] && (int32_t)(_msg_until - now) > 0) {
        int w = 200;
        int x = (240 - w) / 2;
        /* 手牌顶边在 77, 消息条整体上移到 62~75, 不再压住牌顶点数 */
        _canvas->fillRect(x, 62, w, 13, 0x1082);
        _canvas->drawRect(x, 62, w, 13, 0x7BEF);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString(_msg, 120, 68);
        _canvas->setTextDatum(top_left);
    }

    // 作弊菜单
    if (_cheat_menu) {
        int x = 34, y = 20, w = 172, h = 68;
        _canvas->fillRect(x, y, w, h, 0x1082);
        _canvas->drawRect(x, y, w, h, TFT_YELLOW);
        _canvas->setTextColor(TFT_YELLOW);
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setCursor(x + 8, y + 6);
        _canvas->print("1 透视对手手牌");
        _canvas->setCursor(x + 8, y + 21);
        _canvas->print("2 换一张大牌");
        _canvas->setCursor(x + 8, y + 36);
        _canvas->print("3 强制要不起");
        _canvas->setCursor(x + 8, y + 51);
        _canvas->print("4 关闭");
    }

    // 问责弹窗
    if (_phase == PH_ACCUSE) {
        int x = 18, y = 32, w = 204, h = 58;
        _canvas->fillRect(x, y, w, h, 0x1082);
        _canvas->drawRect(x, y, w, h, TFT_RED);
        _canvas->setTextColor(TFT_RED);
        _canvas->setFont(&fonts::efontCN_12);
        _canvas->setCursor(x + 6, y + 5);
        _canvas->print("AI 察觉异常：你是不是出千？");
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(x + 6, y + 22);
        _canvas->print(_accuse_yes ? "  > 承认" : "  > 嘴硬");
        _canvas->setCursor(x + 104, y + 22);
        _canvas->print(_accuse_yes ? "   嘴硬" : "   承认");
        _canvas->setCursor(x + 6, y + 40);
        _canvas->print("< > 切换  ENTER 确认  Y/N");
    }

    // 结算
    if (_phase == PH_OVER) {
        int x = 40, y = 34, w = 160, h = 48;
        _canvas->fillRect(x, y, w, h, 0x1082);
        _canvas->drawRect(x, y, w, h, TFT_GREEN);
        _canvas->setTextDatum(middle_center);
        /* 以前只写"本局结束", 谁赢只靠一条 4 秒的横幅闪过, 错过就再也看不到;
         * 现在把"谁出完 + 地主胜/农民胜"常驻在结算面板里。 */
        bool landlord_win = (_winner >= 0 && _winner == _landlord);
        char line[40];
        snprintf(line, sizeof(line), "%s 出完", (_winner >= 0 && _winner < 3) ? kPlayerName[_winner] : "?");
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->drawString(line, 120, y + 10);
        _canvas->setFont(&fonts::efontCN_12_b);
        _canvas->setTextColor(landlord_win ? TFT_ORANGE : TFT_GREEN);
        _canvas->drawString(landlord_win ? "地主胜" : "农民胜", 120, y + 26);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(TFT_LIGHTGREY);
        _canvas->drawString("ENTER 再来一局  ESC 退出", 120, y + 38);
        _canvas->setTextDatum(top_left);
    }
}

// ================= 嘴炮 =================
struct DzTauntLine { int kind; const char* text; };
static const DzTauntLine kDzTaunts[] = {
    { DZT_AI_PLAY,  "出得不错，可惜没用" }, { DZT_AI_PLAY, "菜就多练" },
    { DZT_AI_PLAY,  "看好了，学着点" },     { DZT_AI_PLAY, "跟不上了吧" },
    { DZT_AI_PLAY,  "就这牌也敢跟？" },     { DZT_AI_PLAY, "别磨蹭，轮你了" },
    { DZT_AI_PLAY,  "这手够你喝一壶" },     { DZT_AI_PLAY, "认命吧" },
    { DZT_PASS,     "不敢出？怂了" },       { DZT_PASS, "牌真烂啊" },
    { DZT_PASS,     "过吧过吧，憋死你" },   { DZT_PASS, "没牌了吧" },
    { DZT_PASS,     "连个对子都凑不齐" },   { DZT_PASS, "趁早认输" },
    { DZT_NEAR_WIN, "你要赢了？想得美" },   { DZT_NEAR_WIN, "不可能！我认真了" },
    { DZT_NEAR_WIN, "别得意，看招" },       { DZT_NEAR_WIN, "有点东西，再来" },
    { DZT_AI_NEAR,  "看到没，下一手完" },   { DZT_AI_NEAR, "来不及了" },
    { DZT_AI_NEAR,  "准备输吧" },           { DZT_AI_NEAR, "挡得住？" },
    { DZT_HUMAN_WIN,"算你狠" },             { DZT_HUMAN_WIN,"运气而已" },
    { DZT_HUMAN_WIN,"你作弊吧" },           { DZT_HUMAN_WIN,"我不服，再来" },
    { DZT_AI_WIN,   "就这水平" },           { DZT_AI_WIN, "回家练十年" },
    { DZT_AI_WIN,   "菜是原罪" },           { DZT_AI_WIN, "输麻了吧" },
    { DZT_AI_WIN,   "废物一个" },           { DZT_AI_WIN, "建议卸载" },
    { DZT_CHEAT,    "又偷看？手脚不干净" }, { DZT_CHEAT, "要不要脸啊" },
    { DZT_CHEAT,    "当我瞎吗" },           { DZT_CHEAT, "作弊狗，呸" },
    { DZT_CHEAT,    "棋盘长了手？" },       { DZT_CHEAT, "偷得挺熟练" },
};

void AppDoudizhu::showTaunt(int kind)
{
    int pick[24];
    int n = 0;
    for (size_t i = 0; i < sizeof(kDzTaunts) / sizeof(kDzTaunts[0]); i++) {
        if (kDzTaunts[i].kind == kind && n < 24) pick[n++] = (int)i;
    }
    if (!n) return;
    snprintf(_taunt, sizeof(_taunt), "%s", kDzTaunts[pick[esp_random() % n]].text);
    /* 2.6s 对 12px 中文偏短, 真人还没读完就消失; 4s 也更方便串口取证(截图逐行读很慢) */
    _taunt_until = GetHAL().millis() + 4000;
    ESP_LOGI("DOUDIZHU", "taunt kind=%d text=%s", kind, _taunt);
}

void AppDoudizhu::drawTaunt()
{
    if (!_taunt[0] || (int32_t)(GetHAL().millis() - _taunt_until) >= 0) return;

    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextDatum(middle_center);
    /* 透视模式会在牌桌上铺满对手手牌, 只描边不够读; 先垫一层 1px 黑边底,
     * 仍是红字为主, 但和五子棋的右栏竖排布局明显不同。 */
    int tw = _canvas->textWidth(_taunt);
    _canvas->fillRect(120 - tw / 2 - 4, 36 - 8, tw + 8, 17, TFT_BLACK);
    _canvas->drawRect(120 - tw / 2 - 4, 36 - 8, tw + 8, 17, TFT_RED);
    _canvas->setTextColor(TFT_BLACK);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx || dy) _canvas->drawString(_taunt, 120 + dx, 36 + dy);
        }
    }
    _canvas->setTextColor(TFT_RED);
    _canvas->drawString(_taunt, 120, 36);
    _canvas->setTextDatum(top_left);
}

void AppDoudizhu::render()
{
    if (!_canvas) return;

    _canvas->fillScreen(0x0C60);  // 深绿桌面
    _canvas->drawRect(2, 14, 236, 72, 0x1A80);

    drawHud();
    drawTable();
    drawHand();
    drawOverlay();
    drawTaunt();  // 最后画: 透视/结算这类覆盖层不能把嘴炮盖掉

    appCanvasPush(_canvas);
    pushMirror();
}

void AppDoudizhu::pushMirror()
{
    if (!glink_is_linked()) return;
    uint8_t p[12] = {0};
    p[0] = 'G';
    p[1] = 'M';
    p[2] = 0x2F;
    p[3] = 0;
    p[4] = (uint8_t)_hand_n[0];
    p[5] = (uint8_t)_hand_n[1];
    p[6] = (uint8_t)_hand_n[2];
    p[7] = (uint8_t)(_phase == PH_PLAY ? _cur : 0);
    p[8] = (uint8_t)(_landlord < 0 ? 0 : _landlord + 1);
    p[9] = _cheat_used ? 1 : 0;
    glink_send_state(p, sizeof(p));
}

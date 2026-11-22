/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps.h>
#include <hal.h>
#include <apps/app_extras/app_extras.h>
#include "apps/app_relay_test/app_relay_test.h"
#include "apps/app_multimeter/app_multimeter.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <nvs_flash.h>
#include <nvs.h>
#include "crash_handler.h" // Added
#include "security/security_manager.h" // Added
#include "apps/app_registry.h"
#include "serial_pump.h"
#include <apps/utils/toast/toast.h>
#include <apps/utils/ui_notify/ui_notify.h>
#include <apps/utils/ui_sysbar/ui_sysbar.h>
#include <apps/utils/theme.h>
#include <apps/utils/ztrace/ztrace.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/glink/zdlink.h>
#include "hal/utils/perf.h"

/* 赛车自测钩子(app_race 提供) */
void race_set_test_tilt(float v);
void race_set_debug(bool on);

using namespace mooncake;
using namespace smooth_ui_toolkit;

/* ------------------------------------------------------------------ */
/*  P3 按 app 堆占用 / 泄漏自测                                        */
/*  串口发送 "HEAPTEST" 触发；逐个 app 装载→打开→关闭→卸载,            */
/*  分别记录打开前后与卸载后的可用堆, 净差值即该 app 的残留(泄漏)。    */
/*  测前临时关掉 WiFi/ESP-NOW: 它们的动态 RX 缓冲会让读数忽大忽小       */
/*  (开一个 app 差 18KB), 关掉后才是 app 自己的真实占用。               */
/*  跑两轮: 第一轮含一次性初始化, 第二轮起才是每次开关的真实泄漏。      */
/* ------------------------------------------------------------------ */
static void run_heap_test()
{
    auto& reg = GetAppRegistry();
    auto& st  = GetHAL().getSettings();
    bool wifi_was = st.GetBool("wifi_on", true);

    glink_set_enabled(false);
    if (wifi_was) GetHAL().wifiDeinit();
    vTaskDelay(pdMS_TO_TICKS(600));

    ESP_LOGI("HT", "===== HEAPTEST BEGIN apps=%d wifi_was=%d free=%uKB largest=%uKB =====",
             (int)reg.size(), (int)wifi_was, (unsigned)perf_free_kb(), (unsigned)perf_largest_kb());

    uint32_t baseline = perf_free_kb();

    for (size_t i = 0; i < reg.size(); i++) {
        const std::string name = reg[i].name;

        for (int cyc = 0; cyc < 2; cyc++) {
            uint32_t h0 = perf_free_kb(), l0 = perf_largest_kb();
            int mid = InstallFromRegistry(i);
            if (mid < 0) {
                ESP_LOGE("HT", "%-12s INSTALL_FAIL", name.c_str());
                break;
            }

            GetMooncake().openApp(mid);
            vTaskDelay(pdMS_TO_TICKS(700));
            uint32_t h1 = perf_free_kb(), l1 = perf_largest_kb();

            GetMooncake().closeApp(mid);
            vTaskDelay(pdMS_TO_TICKS(200));
            UninstallFromRegistry(i);
            vTaskDelay(pdMS_TO_TICKS(400));
            uint32_t h2 = perf_free_kb(), l2 = perf_largest_kb();

            // cyc0 = 冷启动(含该 app 的一次性初始化); cyc1 = 稳态, 差值才是真实泄漏
            ESP_LOGI("HT", "c%d %-12s open=%dKB hold=%uKB after=%dKB largest=%u/%u/%u",
                     cyc, name.c_str(), (int)h0 - (int)h1, (unsigned)h1,
                     (int)h0 - (int)h2,
                     (unsigned)l0, (unsigned)l1, (unsigned)l2);
        }
    }

    ESP_LOGI("HT", "===== HEAPTEST END free=%uKB (baseline %uKB, residual %dKB) largest=%uKB =====",
             (unsigned)perf_free_kb(), (unsigned)baseline,
             (int)baseline - (int)perf_free_kb(),
             (unsigned)perf_largest_kb());

    // 恢复无线广播
    glink_set_enabled(st.GetBool("glink_on", true));
    if (wifi_was) {
        glink_acquire("Launcher", GLINK_GID_LAUNCHER, GLINK_ICON_GENERIC);
        vTaskDelay(pdMS_TO_TICKS(1200));
        ESP_LOGI("HT", "restored(wifi on) free=%uKB largest=%uKB",
                 (unsigned)perf_free_kb(), (unsigned)perf_largest_kb());
    }
}

/* ------------------------------------------------------------------ */
/*  P3 长时间稳定性压测: 串口 "SOAK" 触发                              */
/*  3 轮遍历全部 app(打开→关闭→卸载), 每轮结束打印堆水位。             */
/*  判据: 每轮 free/largest 不回退 = 无累积泄漏。                       */
/* ------------------------------------------------------------------ */
static void run_soak_test()
{
    auto& reg = GetAppRegistry();
    ESP_LOGI("SOAK", "===== SOAK BEGIN apps=%d free=%uKB largest=%uKB =====",
             (int)reg.size(), (unsigned)perf_free_kb(), (unsigned)perf_largest_kb());

    for (int round = 1; round <= 3; round++) {
        for (size_t i = 0; i < reg.size(); i++) {
            int mid = InstallFromRegistry(i);
            if (mid < 0) continue;
            GetMooncake().openApp(mid);
            vTaskDelay(pdMS_TO_TICKS(600));
            GetMooncake().closeApp(mid);
            vTaskDelay(pdMS_TO_TICKS(200));
            UninstallFromRegistry(i);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ESP_LOGI("SOAK", "round %d free=%uKB largest=%uKB min=%uKB",
                 round, (unsigned)perf_free_kb(), (unsigned)perf_largest_kb(),
                 (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    }

    ESP_LOGI("SOAK", "===== SOAK END free=%uKB largest=%uKB min=%uKB =====",
             (unsigned)perf_free_kb(), (unsigned)perf_largest_kb(),
             (unsigned)(esp_get_minimum_free_heap_size() / 1024));
}

/* 串口按键注入: 先排队, 由 app_task 在应用上下文里 emit(见 app_task 顶部)。
 * 直接在 desktop_task emit 会和 app 的关闭/析构抢同一个回调链表,
 * 实测会在 Breakout 这类应用上跳到已释放的函数指针(InstructionFetchError)。 */
static volatile int s_inj_code = 0;
static volatile int s_inj_down = 0;
static char s_inj_name[16] = "";
static void zd_inject_key(KeScanCode_t code, const char* name, bool down);

// FreeRTOS task handles
TaskHandle_t desktop_task_handle = NULL;
TaskHandle_t app_task_handle = NULL;

// Task stack sizes
#define DESKTOP_TASK_STACK_SIZE 8192
#define APP_TASK_STACK_SIZE 16384

// Task priorities
#define DESKTOP_TASK_PRIORITY 5
#define APP_TASK_PRIORITY 4

// Global flag to indicate if app task should be running
bool app_task_running = false;
int current_app_id = -1;

static bool open_zdmsg_app();
extern bool zdmsg_is_open();
extern bool zdmsg_list_contains(const char* needle);

/* 当前前台 app 名(不含 LoRa 面板自身与桌面); 只用于日志/自测 */
const char* zd_fg_app()
{
    static char buf[24];
    auto list = GetMooncake().getAppAbilityManager()->getAllAbilityInstance();
    for (auto& raw : list) {
        auto* app = static_cast<AppAbility*>(raw);
        if (app->getId() == 0) continue;
        if (GetMooncake().getAppCurrentState(app->getId()) == AppAbility::StateRunning) {
            snprintf(buf, sizeof(buf), "%s", app->getAppInfo().name.c_str());
            return buf;
        }
    }
    return "Launcher";
}

/* 关掉一个正在跑的 app(按名字), 用于自测前清场 */
static void close_app_named(const char* name)
{
    auto list = GetMooncake().getAppAbilityManager()->getAllAbilityInstance();
    for (auto& raw : list) {
        auto* app = static_cast<AppAbility*>(raw);
        if (GetMooncake().getAppCurrentState(app->getId()) != AppAbility::StateRunning) continue;
        if (app->getAppInfo().name == name) {
            ZT("flow", "step=prep close \"%s\"", name);
            app->close();
            return;
        }
    }
}

/* 切前台: 关掉其它正在跑的 app。mooncake 的 openApp 不会自己收掉前一个,
 * 两个 app 同时 onRunning 会互相覆盖共享画布(实测: 万用表还在画, 面板被盖掉)。
 * id 0 = 桌面, 必须留着。 */
static void close_other_apps(int keep_id)
{
    auto list = GetMooncake().getAppAbilityManager()->getAllAbilityInstance();
    for (auto& raw : list) {
        auto* app = static_cast<AppAbility*>(raw);
        int id = app->getId();
        if (id == keep_id || id == 0) continue;
        if (GetMooncake().getAppCurrentState(id) == AppAbility::StateRunning) {
            ZT("flow", "close \"%s\" before open", app->getAppInfo().name.c_str());
            app->close();
        }
    }
}

/* 打开一个 app(按名字); 与桌面菜单走同一条 InstallFromRegistry + openApp */
static bool open_app_named(const char* name)
{
    auto& reg = GetAppRegistry();
    for (size_t i = 0; i < reg.size(); i++) {
        if (reg[i].name == name) {
            int mid = InstallFromRegistry(i);
            if (mid < 0) return false;
            /* 已在运行: 直接复用, 绝不再 openApp。
             * 重复 open 会让同一个实例再跑一遍 onOpen(键盘回调重复连接),
             * 一个按键被吃两次(实测按 6 次 L 难度跳了两档)。 */
            if (GetMooncake().getAppCurrentState(mid) == AppAbility::StateRunning) {
                /* 已在跑: 只清掉别的残留 app, 绝不再 openApp */
                close_other_apps(mid);
                ESP_LOGI("APP", "open skip: %s already running (id=%d)", name, mid);
                ZT("flow", "open skip already-running \"%s\" id=%d", name, mid);
                return true;
            }
            close_other_apps(mid);
            GetMooncake().openApp(mid);
            return true;
        }
    }
    return false;
}


// App task function
void app_task(void *pvParameters)
{
    mclog::tagInfo("Task", "App task started");
    
    while (1) {
        if (app_task_running) {
            /* 自测注入的按键在 app 任务里落: 与 app 的关闭/析构同线程, 避免回调竞态 */
            if (s_inj_code) {
                const int code = s_inj_code;
                const bool down = (s_inj_down != 0);
                s_inj_code = 0;
                Keyboard::KeyEvent_t e;
                e.state = down;
                e.isModifier = false;
                e.keyCode = (KeScanCode_t)code;
                e.keyName = s_inj_name;
                GetHAL().keyboard.onKeyEvent.emit(e);
            }
            // Run the app
            GetMooncake().update();
            // 全局提示条(静音/有声): 必须在本任务、且在 app 推屏之后叠加,
            // 绝不能在键盘回调(desktop_task)里画
            ui_toast::update();
            // 全局消息通知条: 同上, 只在本任务绘制; '=' 跳转请求也在这里落库
            if (ui_notify::take_open_request()) {
                char before[24];
                snprintf(before, sizeof(before), "%s", zd_fg_app());
                uint32_t t0 = zdlink_now_ms();
                bool ok = open_zdmsg_app();
                ZT("jump", "key='=' fg_before=%s opened=%d dt=%ums", before, (int)ok,
                   (unsigned)(zdlink_now_ms() - t0));
            }
            ui_notify::set_suppressed(zdmsg_is_open());
            ui_notify::update();
            // 全屏游戏: 顶部 kInlineH 是 app 画布之外的内联状态条, 由 ui_sysbar 直接画,
            // 不再每帧推 26px 系统栏盖住游戏内容(那正是之前"重叠两份"的根因)。
            ui_sysbar::update();
        } else {
            // If no app is running, yield to other tasks
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        
        GetHAL().feedTheDog();
    }
}

/* ------------------------------------------------------------------ */
/*  实机取证: 顶栏(系统栏画布) + 内容画布 拼成整屏 240x135 -> 串口       */
/*  主机脚本按 "SHOT BEGIN w h n" / "SHOT i/n <b64>" / "SHOT END" 还原  */
/*  成 PNG。只读画布, 不依赖屏幕读回, 也不改绘制路径。                  */
/* ------------------------------------------------------------------ */
static void zd_shot_dump()
{
    extern const char* zd_fg_app();
    auto& shared = GetHAL().canvas();
    auto& bar = GetHAL().canvasSystemBar();
    /* 有的全屏 app 用自己的 sprite、共享画布是 0x0, 这时从 ui_sysbar 的登记里取,
     * 否则取证会直接失败(以前 Breakout/Flappy/Minesweeper 就截不到图)。 */
    LGFX_Sprite* cvp = &shared;
    if (!shared.getBuffer() || shared.width() <= 0) {
        const void* reg = ui_sysbar::inlineMode() ? ui_sysbar::appCanvasSprite() : nullptr;
        if (reg) cvp = (LGFX_Sprite*)reg;
    }
    auto& cv = *cvp;
    const int w = (int)cv.width();
    const int ch = (int)cv.height();
    ESP_LOGI("ZD", "shot canvas=%p shared=%p reg=%p %dx%d fg=%s inline=%d",
             (const void*)cvp, (const void*)&shared,
             (const void*)ui_sysbar::appCanvasSprite(), w, ch,
             zd_fg_app(), (int)ui_sysbar::inlineMode());
    if (w <= 0 || ch <= 0 || w > 400 || !cv.getBuffer()) {
        ESP_LOGE("ZD", "shot: bad canvas %dx%d", w, ch);
        return;
    }

    /* 顶栏画布前 26px / 全屏 app 的前 kInlineH 行; 只有宽度一致才拼 */
    int bh = 0;
    if ((int)bar.width() == w && bar.height() > 0 && bar.getBuffer()) {
        bh = (int)bar.height();
    }

    const int dh = (int)GetHAL().display().height();

    /* 全屏 app: 内容画布 121 高, 屏幕上方 kInlineH 是内联状态条, 下面 121 是内容。
     * 按真实屏幕排版拼回 240x135, 不做拉伸。 */
    const bool inline_mode = (bh > 0) && (ch + ui_sysbar::kInlineH <= dh + 1) &&
                             (ch >= dh - ui_sysbar::kInlineH);

    /* 老的全屏画布(135 高, 系统栏压在内容之上): 取同坐标的顶栏像素叠加 */
    const bool overlay = (bh > 0) && (ch >= dh) && !inline_mode;
    const int h = inline_mode ? dh : (overlay ? ch : (ch + bh));

    if (inline_mode && !ui_notify::active()) ui_sysbar::drawInline();

    const size_t rowb = (size_t)w * 2;
    const size_t total = rowb * (size_t)h;
    const size_t chunk = 144;  /* 144B -> 192 个 base64 字符, 不产生中间补位 */
    const size_t lines = (total + chunk - 1) / chunk;
    static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static uint8_t tmp[160];
    static uint8_t rowbuf[800];      /* 一行 RGB565, 高字节在前(与主机脚本一致) */
    static RGBColor rgbrow[400];
    int row_y = -1;

    printf("SHOT BEGIN %d %d %u\n", w, h, (unsigned)lines);
    for (size_t off = 0, idx = 0; off < total; off += chunk, idx++) {
        size_t n = (total - off) < chunk ? (total - off) : chunk;
        for (size_t i = 0; i < n; i++) {
            size_t o = off + i;
            int y = (int)(o / rowb);
            size_t xb = o % rowb;
            if (y != row_y) {
                row_y = y;
                if (inline_mode) {
                    if (y < ui_sysbar::kInlineH) {
                        /* 屏幕上方 kInlineH 行就是内联状态条, 原样拷 */
                        memcpy(rowbuf, (const uint8_t*)bar.getBuffer() + (size_t)y * rowb, rowb);
                    } else {
                        /* 取证期间 app 可能刚好被关掉(异步 close), 指针会失效:
                         * 每行读之前确认登记的仍是同一块画布, 变了就中止, 别去读已释放内存 */
                        if (cvp != &shared &&
                            (!ui_sysbar::inlineMode() || ui_sysbar::appCanvasSprite() != cvp)) {
                            printf("SHOT ABORT canvas-changed\n");
                            return;
                        }
                        cv.readRectRGB(0, y - ui_sysbar::kInlineH, w, 1, rgbrow);
                        for (int x = 0; x < w; x++) {
                            uint16_t v = (uint16_t)(((rgbrow[x].r & 0xF8) << 8) |
                                                    ((rgbrow[x].g & 0xFC) << 3) | (rgbrow[x].b >> 3));
                            rowbuf[2 * x]      = (uint8_t)(v >> 8);
                            rowbuf[2 * x + 1]  = (uint8_t)(v & 0xFF);
                        }
                    }
                } else if (y < bh) {
                    /* 顶栏是 16bpp, 字节序就是推屏时的顺序, 原样拷 */
                    memcpy(rowbuf, (const uint8_t*)bar.getBuffer() + (size_t)y * rowb, rowb);
                } else {
                    int sy = overlay ? y : (y - bh);
                    /* 内容画布可能是 8bpp(游戏全屏), 交给 readRectRGB 统一转 RGB565 */
                    cv.readRectRGB(0, sy, w, 1, rgbrow);
                    for (int x = 0; x < w; x++) {
                        uint16_t v = (uint16_t)(((rgbrow[x].r & 0xF8) << 8) |
                                                ((rgbrow[x].g & 0xFC) << 3) | (rgbrow[x].b >> 3));
                        rowbuf[2 * x]      = (uint8_t)(v >> 8);
                        rowbuf[2 * x + 1]  = (uint8_t)(v & 0xFF);
                    }
                }
            }
            tmp[i] = rowbuf[xb];
        }

        char b64[208];
        size_t o = 0;
        for (size_t i = 0; i < n; i += 3) {
            int rem = (int)(n - i);
            uint32_t v = (uint32_t)tmp[i] << 16;
            if (rem > 1) v |= (uint32_t)tmp[i + 1] << 8;
            if (rem > 2) v |= (uint32_t)tmp[i + 2];
            b64[o++] = tb[(v >> 18) & 63];
            b64[o++] = tb[(v >> 12) & 63];
            b64[o++] = (rem > 1) ? tb[(v >> 6) & 63] : '=';
            b64[o++] = (rem > 2) ? tb[v & 63] : '=';
        }
        b64[o] = 0;
        printf("SHOT %u/%u %s\n", (unsigned)(idx + 1), (unsigned)lines, b64);
    }
    printf("SHOT END\n");
    fflush(stdout);
    ESP_LOGI("ZD", "shot: %dx%d (bar %d) %uB dumped", w, h, bh, (unsigned)total);
}

/* 字号对比页由面板自己画(见 apps/app_zdmsg) */
extern void zdmsg_font_demo(bool on);
extern bool zdmsg_is_open();

/* 打开 天链面板: 串口自测与全局通知的 '=' 跳转共用 */
static bool open_zdmsg_app()
{
    auto& reg = GetAppRegistry();
    for (size_t i = 0; i < reg.size(); i++) {
        if (reg[i].name == "天链") {
            int mid = InstallFromRegistry(i);
            if (mid < 0) return false;

            /* openApp 不会关掉当前 app, 两个 app 同时 onRunning 会互相覆盖
             * 共享画布(实测: 万用表还在往 240x109 画, 面板内容被盖掉)。
             * 切前台前先把别的 app 收掉, 与从桌面进入的行为一致。 */
            close_other_apps(mid);

            GetMooncake().openApp(mid);
            return true;
        }
    }
    return false;
}

/* ---------------- 全链路自测 ZDFLOW ----------------
 * 一条命令跑完: 任意 app 在前台 → 收到消息 → 通知条 → 按 = 跳转并定位 →
 * 回复 → 收到 ACK → 降级路径(堆紧张只显示一行)。每步自己打 PASS/FAIL 与耗时。 */
static int8_t s_flow_stage = -1;
static uint32_t s_flow_ts = 0;
static uint32_t s_flow_t0 = 0;
static char s_flow_text[72] = {0};
static bool s_flow_ok[4];
static uint32_t s_flow_dt[4];
static uint32_t s_flow_ack0 = 0;

static void flow_key(KeScanCode_t c, const char* n)
{
    Keyboard::KeyEvent_t e;
    e.state = true;
    e.isModifier = false;
    e.keyCode = c;
    e.keyName = n;
    GetHAL().keyboard.onKeyEvent.emit(e);
}

static void flow_begin()
{
    memset(s_flow_ok, 0, sizeof(s_flow_ok));
    memset(s_flow_dt, 0, sizeof(s_flow_dt));
    snprintf(s_flow_text, sizeof(s_flow_text), "FLOW%03u 全链路自测消息",
             (unsigned)(zdlink_now_ms() % 1000));
    s_flow_t0 = zdlink_now_ms();
    zdlink_stats(nullptr, nullptr, nullptr, &s_flow_ack0);
    ZT("flow", "step=0/start fg=%s heap=%u text=\"%s\"", zd_fg_app(),
       (unsigned)esp_get_free_heap_size(), s_flow_text);
    s_flow_stage = 0;
    s_flow_ts = s_flow_t0;
}

static void flow_tick()
{
    if (s_flow_stage < 0) return;
    uint32_t now = zdlink_now_ms();
    if ((int32_t)(now - s_flow_ts) < 0) return;

    switch (s_flow_stage) {
        case 0: /* 清场: 面板/万用表先关掉, 否则通知会被面板让位规则跳过 */
            close_app_named("天链");
            close_app_named("Multimeter");
            s_flow_stage = 1;
            s_flow_ts = now + 500;
            break;

        case 1: { /* 前台放一个普通 app */
            bool ok = open_app_named("Multimeter");
            ZT("flow", "step=1/app name=Multimeter open=%d fg=%s result=%s", (int)ok,
               zd_fg_app(), ok ? "PASS" : "FAIL");
            s_flow_stage = 2;
            s_flow_ts = now + 800;
            break;
        }

        case 2: /* 注入一条消息(走正式解析路径) */
            ZT("flow", "step=2/rx inject from=7DF43F68 via=E text=\"%s\"", s_flow_text);
            zdlink_selftest_rx_text(0x7DF43F68u, s_flow_text);
            s_flow_stage = 3;
            s_flow_ts = now + 260;
            break;

        case 3: { /* 通知条是否出现 */
            bool ok = ui_notify::active() && strstr(ui_notify::current_text(), "FLOW");
            s_flow_ok[0] = ok;
            s_flow_dt[0] = now - s_flow_t0;
            ZT("flow", "step=3/notify shown=%d mode=%s unread=%u dt=%ums result=%s", (int)ok,
               ui_notify::simple_mode() ? "simple" : "full", (unsigned)ui_notify::unread(),
               (unsigned)s_flow_dt[0], ok ? "PASS" : "FAIL");
            if (ok) flow_key(KEY_EQUAL, "=");
            s_flow_stage = 4;
            s_flow_ts = now + 1000;
            break;
        }

        case 4: { /* 按 = 后: 面板打开且那条消息在列表里 */
            bool open = zdmsg_is_open();
            bool located = open && zdmsg_list_contains("FLOW");
            s_flow_ok[1] = located;
            s_flow_dt[1] = now - s_flow_t0;
            ZT("flow", "step=4/jump open=%d located=%d fg=%s dt=%ums result=%s", (int)open,
               (int)located, zd_fg_app(), (unsigned)s_flow_dt[1], located ? "PASS" : "FAIL");
            if (located) {
                /* 列表页打字即写: o k → Enter 发送(目标默认 E 直连) */
                flow_key(KEY_O, "o");
                flow_key(KEY_K, "k");
                flow_key(KEY_ENTER, "e");
            }
            s_flow_stage = 5;
            s_flow_ts = now + 1800;
            break;
        }

        case 5: { /* 回复是否拿到 ACK */
            uint32_t ack = 0;
            zdlink_stats(nullptr, nullptr, nullptr, &ack);
            bool ok = ack > s_flow_ack0;
            s_flow_ok[2] = ok;
            s_flow_dt[2] = now - s_flow_t0;
            ZT("flow", "step=5/reply_ack ack_before=%u ack_now=%u delta=%u dt=%ums result=%s",
               (unsigned)s_flow_ack0, (unsigned)ack, (unsigned)(ack - s_flow_ack0),
               (unsigned)s_flow_dt[2], ok ? "PASS" : "FAIL");
            /* 降级路径: 先退出面板, 让通知条能显示 */
            close_app_named("天链");
            s_flow_stage = 6;
            s_flow_ts = now + 600;
            break;
        }

        case 6: /* 堆紧张(阈值拉到 200KB)再收一条 */
            ui_notify::set_low_heap(200000);
            zdlink_selftest_rx_text(0x7DF331D0u, "FLOW-low 堆紧张时只显示一行");
            s_flow_stage = 7;
            s_flow_ts = now + 300;
            break;

        case 7: { /* 降级校验 */
            bool ok = ui_notify::active() && ui_notify::simple_mode();
            s_flow_ok[3] = ok;
            s_flow_dt[3] = now - s_flow_t0;
            ui_notify::set_low_heap(0);
            ZT("flow", "step=6/degrade simple=%d shown=%d dt=%ums result=%s",
               (int)ui_notify::simple_mode(), (int)ui_notify::active(),
               (unsigned)s_flow_dt[3], ok ? "PASS" : "FAIL");

            int pass = 0;
            for (int i = 0; i < 4; i++) {
                if (s_flow_ok[i]) pass++;
            }
            ZT("flow",
               "summary result=%s steps=%d/4 dt_notify=%u dt_jump=%u dt_ack=%u dt_low=%u "
               "total=%ums",
               pass == 4 ? "PASS" : "FAIL", pass, (unsigned)s_flow_dt[0],
               (unsigned)s_flow_dt[1], (unsigned)s_flow_dt[2], (unsigned)s_flow_dt[3],
               (unsigned)(now - s_flow_t0));
            s_flow_stage = -1;
            break;
        }
    }
}

/* ---------------- 全局状态栏自测 ZDBAR ----------------
 * 一条命令跑完: 打开全屏游戏 → 校验内容画布 = 屏高-内联头(没被压扁/裁掉)、
 * 系统栏不再每帧覆盖(pushes 不增长)、内联头确实画了
 * → 强制 ESP-NOW(青) → 强制 LoRa(橙) → 清掉强制, 用真实空口帧验证 L 标记。 */
static int8_t s_bar_stage = -1;
static uint32_t s_bar_ts = 0;
static uint32_t s_bar_push0 = 0;
static uint32_t s_bar_inline0 = 0;
static uint32_t s_bar_draw0 = 0;
static bool s_bar_ok[5];
static uint8_t s_bar_real_link = 0;  /* 注入真实 LoRa 帧后立刻采到的来源 */
static int8_t s_bar_real_rssi = 0;

static void bar_begin()
{
    memset(s_bar_ok, 0, sizeof(s_bar_ok));
    s_bar_push0 = ui_sysbar::pushes();
    s_bar_inline0 = ui_sysbar::inlineDraws();
    s_bar_stage = 0;
    s_bar_ts = zdlink_now_ms();
    ZT("bar", "step=0/start fg=%s canvas=%dx%d heap=%u", zd_fg_app(),
       (int)GetHAL().canvas().width(), (int)GetHAL().canvas().height(),
       (unsigned)esp_get_free_heap_size());
}

static void bar_tick()
{
    if (s_bar_stage < 0) return;
    uint32_t now = zdlink_now_ms();
    if ((int32_t)(now - s_bar_ts) < 0) return;

    switch (s_bar_stage) {
        case 0: /* 先清场: 上一个 app 的 close 是异步的, 边关边开会被它顺手收掉(实测) */
            close_other_apps(-1);
            s_bar_stage = 1;
            s_bar_ts = now + 600;
            break;
        case 1: { /* 前台放一个全屏游戏 */
            bool ok = open_app_named("Gomoku");
            s_bar_push0 = ui_sysbar::pushes();
            s_bar_inline0 = ui_sysbar::inlineDraws();
            ZT("bar", "step=1/open name=Gomoku ok=%d fg=%s canvas=%dx%d", (int)ok, zd_fg_app(),
               (int)GetHAL().canvas().width(), (int)GetHAL().canvas().height());
            s_bar_stage = 2;
            s_bar_ts = now + 1200;
            break;
        }
        case 2: { /* 内容没被裁: 画布 = 屏高-内联头; 不再有 26px 系统栏覆盖 */
            int ch = (int)GetHAL().canvas().height();
            int cw = (int)GetHAL().canvas().width();
            int dh = (int)GetHAL().display().height();
            int want = dh - ui_sysbar::kInlineH;
            uint32_t p = ui_sysbar::pushes();
            uint32_t id = ui_sysbar::inlineDraws();
            bool ok = (ch == want) && ui_sysbar::inlineMode() &&
                      (p == s_bar_push0) && (id > s_bar_inline0);
            s_bar_ok[0] = ok;
            ZT("bar",
               "step=2/inline canvas=%dx%d want_h=%d inline=%d pushes=%u(+%u) inline_draws=%u(+%u) "
               "fg=%s result=%s",
               cw, ch, want, (int)ui_sysbar::inlineMode(), (unsigned)p,
               (unsigned)(p - s_bar_push0), (unsigned)id, (unsigned)(id - s_bar_inline0),
               zd_fg_app(), ok ? "PASS" : "FAIL");
            ui_sysbar::force_link(ZD_VIA_E, 15000);
            s_bar_stage = 3;
            s_bar_ts = now + 500;
            break;
        }
        case 3: { /* ESP-NOW 青色 */
            uint8_t l = ui_sysbar::link();
            bool ok = (l == ZD_VIA_E);
            s_bar_ok[1] = ok;
            ZT("bar", "step=3/espnow link=%c rssi=%d color=0x%04X result=%s",
               l == ZD_VIA_E ? 'E' : (l == ZD_VIA_L ? 'L' : '-'), (int)ui_sysbar::rssi(),
               (unsigned)THEME_COLOR_LINK_ESPNOW, ok ? "PASS" : "FAIL");
            ui_sysbar::force_link(ZD_VIA_L, 15000);
            s_bar_stage = 4;
            s_bar_ts = now + 500;
            break;
        }
        case 4: { /* LoRa 橙色 */
            uint8_t l = ui_sysbar::link();
            bool ok = (l == ZD_VIA_L);
            s_bar_ok[2] = ok;
            ZT("bar", "step=4/lora link=%c rssi=%d color=0x%04X result=%s",
               l == ZD_VIA_E ? 'E' : (l == ZD_VIA_L ? 'L' : '-'), (int)ui_sysbar::rssi(),
               (unsigned)THEME_COLOR_LINK_LORA, ok ? "PASS" : "FAIL");
            /* 清掉强制, 用真实 LTEXT(走正式解析路径) 验证 L 标记。
             * 注意: 必须在注入后"立刻"采样 —— 等 700ms 的话对端的 ESP-NOW 帧
             * 会变成更近的一条, 那时显示 E 才是正确行为, 断言必须落在注入瞬间。 */
            ui_sysbar::force_link(0, 0);
            zdlink_selftest_rx_ltext(0x7DF331D0u, 0x7DF43F68u, -71, 16, 1, "BAR 空口自测");
            ui_sysbar::refresh_now();
            uint8_t rl = ui_sysbar::link();
            s_bar_ok[3] = (rl == ZD_VIA_L);
            s_bar_real_link = rl;
            s_bar_real_rssi = ui_sysbar::rssi();
            s_bar_stage = 5;
            s_bar_ts = now + 300;
            break;
        }
        case 5: { /* 真实空口帧也能把来源切到 LoRa(注入瞬间已断言) */
            s_bar_ok[3] = (s_bar_real_link == ZD_VIA_L);
            /* 回桌面: 统一顶栏必须只由 ui_sysbar 画(Launcher 自绘已删除) */
            close_app_named("Gomoku");
            s_bar_push0 = ui_sysbar::pushes();
            s_bar_draw0 = ui_sysbar::statusDraws();
            s_bar_stage = 6;
            s_bar_ts = now + 1200;
            break;
        }
        case 6: { /* 桌面: 连续 6 帧(120ms 间隔)只有 ui_sysbar 一个绘制者 */
            /* 静态桌面栏本该只在脏时推(100ms 节流), 所以先主动触发一次重绘
             * (等价于 Launcher 的 1Hz 看门狗), 再连拍 6 帧:
             * 既验证这次重绘真的落笔(status_draws/pushes 有增长),
             * 也验证随后的 6 帧像素完全一致 —— 没有第二套栏、没有中间态。 */
            /* 前面 case 4 注入了一条 LTEXT 自测帧, 会真的弹 4s 通知;
             * 通知活跃时 overlay 把 26px 栏交给 ui_notify 画(同一绘制者),
             * 这里必须等它收起, 否则测不到"桌面栏重画", 断言会假失败。 */
            for (int i = 0; i < 60 && ui_notify::active(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            /* 置脏后由 Launcher 的 1Hz 看门狗 / app_task 里的 ui_sysbar::update 去落笔。
             * 这里绝不能在 desktop_task 里直接推屏 —— 两个任务同时 push 会死在 SPI 上。 */
            ui_sysbar::invalidate();
            uint32_t p0 = ui_sysbar::pushes();
            uint32_t d0 = ui_sysbar::statusDraws();
            for (int i = 0; i < 30 && ui_sysbar::statusDraws() == d0; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            bool frames_ok = true;
            for (int i = 0; i < 6; i++) {
                vTaskDelay(pdMS_TO_TICKS(120));
                bool desk = (strcmp(zd_fg_app(), "Launcher") == 0);
                uint32_t dh = ui_sysbar::statusDraws();
                ZT("bar", "desktop frame=%d fg=%s owner=ui_sysbar status_draws=%u(+%u) canvas=%dx%d",
                   i, zd_fg_app(), (unsigned)dh, (unsigned)(dh - d0),
                   (int)GetHAL().canvas().width(), (int)GetHAL().canvas().height());
                if (!desk) frames_ok = false;
            }
            uint32_t p = ui_sysbar::pushes();
            uint32_t d = ui_sysbar::statusDraws();
            bool ok = frames_ok && (p > p0 || p > s_bar_push0) && d > s_bar_draw0;
            s_bar_ok[4] = ok;
            ZT("bar",
               "step=6/desktop fg=%s canvas=%dx%d push=+%u status_draws=+%u result=%s",
               zd_fg_app(), (int)GetHAL().canvas().width(), (int)GetHAL().canvas().height(),
               (unsigned)(p - p0), (unsigned)(d - s_bar_draw0), ok ? "PASS" : "FAIL");
            s_bar_stage = 7;
            s_bar_ts = now + 200;
            break;
        }
        case 7: { /* 汇总 */
            int pass = 0;
            for (int i = 0; i < 5; i++) {
                if (s_bar_ok[i]) pass++;
            }
            ZT("bar",
               "step=7/real_lora link=%c rssi=%d result=%s | summary result=%s steps=%d/5 "
               "now_link=%c pushes=%u inline_draws=%u status_draws=%u heap=%u",
               s_bar_real_link == ZD_VIA_E ? 'E' : (s_bar_real_link == ZD_VIA_L ? 'L' : '-'),
               (int)s_bar_real_rssi, s_bar_ok[3] ? "PASS" : "FAIL", pass == 5 ? "PASS" : "FAIL",
               pass, ui_sysbar::link() == ZD_VIA_E ? 'E' : (ui_sysbar::link() == ZD_VIA_L ? 'L' : '-'),
               (unsigned)ui_sysbar::pushes(), (unsigned)ui_sysbar::inlineDraws(),
               (unsigned)ui_sysbar::statusDraws(), (unsigned)esp_get_free_heap_size());
            s_bar_stage = -1;
            break;
        }
    }
}

// Desktop task function
/* ---------------- 通用打开 app 取证: ZDGO:Name ----------------
 * 先关掉前台 app, 等 600ms 再开(close 是异步的, 边关边开会被顺手收掉)。 */
static char s_go_name[32] = {0};
static int8_t s_go_stage = -1;
static uint32_t s_go_ts = 0;

static void go_tick()
{
    std::string want = SerialPump::takePendingPrefix("ZDGO:");
    if (!want.empty()) {
        snprintf(s_go_name, sizeof(s_go_name), "%s", want.c_str());
        close_other_apps(-1);
        s_go_stage = 1;
        s_go_ts = zdlink_now_ms() + 650;
        ZT("go", "stage=0 close others, target=%s", s_go_name);
    }
    if (s_go_stage < 0) return;
    if ((int32_t)(zdlink_now_ms() - s_go_ts) < 0) return;
    if (s_go_stage == 1) {
        bool ok = open_app_named(s_go_name);
        ZT("go", "stage=1 open name=%s ok=%d fg=%s canvas=%dx%d heap=%u", s_go_name, (int)ok,
           zd_fg_app(), (int)GetHAL().canvas().width(), (int)GetHAL().canvas().height(),
           (unsigned)esp_get_free_heap_size());
        s_go_stage = -1;
    }
}

/* ---------------- 全屏 app 逐一复核 ZDALL ----------------
 * 把每个全屏 app 都真的打开一次, 断言三件事(这就是"内容没被裁掉/挡住"的机器判据):
 *   1) 内容画布 = 240 x (屏高 - kInlineH), 不是 109 —— 没被压扁;
 *   2) inlineMode() == 1 —— 顶栏走内联条, 不走 26px 覆盖层;
 *   3) 它跑起来的这段时间里 ui_sysbar::pushes() 不增长 —— 26px 系统栏没有每帧顶回去。
 * 最后一行打 summary PASS x/y, 任何一项不满足就 FAIL 并继续跑到尾。 */
static const char* kAllApps[] = {
    "2048", "Breakout", "Checkers", "Connect4", "Doudizhu", "Flappy", "FPS", "Gomoku",
    "Invaders", "Minesweeper", "Pong", "Racing", "Reversi", "Snake", "Shogi", "Tetris",
    "Tank", "UNO", "DeepSeek", "IMU", "IR Remote", "MicMeter"};
/* 注: 注册表里 AppImu 的注册名是 "IMU"(app 自己的 name 才是 "Level");
 * app_combo(BT Keyboard & Mouse) 因依赖 ble_hid 被 CMakeLists 排除, 不在此列。 */

static int8_t s_all_stage = -1;
static int s_all_idx = 0;
static int s_all_pass = 0;
static int s_all_fail = 0;
static uint32_t s_all_ts = 0;
static uint32_t s_all_push0 = 0;

static void all_begin()
{
    s_all_idx = 0;
    s_all_pass = 0;
    s_all_fail = 0;
    s_all_stage = 0;
    s_all_ts = zdlink_now_ms();
    ZT("all", "start apps=%d want_h=%d", (int)(sizeof(kAllApps) / sizeof(kAllApps[0])),
       (int)GetHAL().display().height() - ui_sysbar::kInlineH);
}

static void all_tick()
{
    if (s_all_stage < 0) return;
    uint32_t now = zdlink_now_ms();
    if ((int32_t)(now - s_all_ts) < 0) return;
    const int n = (int)(sizeof(kAllApps) / sizeof(kAllApps[0]));

    switch (s_all_stage) {
        case 0: /* 先清场: close 是异步的 */
            if (s_all_idx >= n) {
                s_all_stage = -1;
                ZT("all", "summary result=%s pass=%d fail=%d total=%d",
                   (s_all_fail == 0) ? "PASS" : "FAIL", s_all_pass, s_all_fail, n);
                return;
            }
            close_other_apps(-1);
            s_all_stage = 1;
            s_all_ts = now + 900;  /* mooncake 的 close 是异步的, 留够时间再开下一个 */
            break;
        case 1: {
            bool ok = open_app_named(kAllApps[s_all_idx]);
            s_all_push0 = ui_sysbar::pushes();
            ZT("all", "open[%d/%d] name=%s ok=%d", s_all_idx + 1, n, kAllApps[s_all_idx],
               (int)ok);
            s_all_stage = 2;
            s_all_ts = now + 1000;
            break;
        }
        case 2: {
            /* 有的 app 用自己的 sprite(共享画布 0x0), 统一看 ui_sysbar 里的登记 */
            const int ch = ui_sysbar::appCanvasHeight();
            const int cw = (int)GetHAL().display().width();
            const int want = (int)GetHAL().display().height() - ui_sysbar::kInlineH;
            const uint32_t p = ui_sysbar::pushes();
            /* 注册名和 app 自己的 name 不一定一样(如注册 "IMU" / 名为 "Level"),
             * 只要求不是停在前台桌面即可 */
            const bool fg_ok = (strcmp(zd_fg_app(), "Launcher") != 0);
            bool ok = (cw == (int)GetHAL().display().width()) && (ch == want) &&
                      ui_sysbar::inlineMode() && (p == s_all_push0) && fg_ok;
            if (ok) s_all_pass++;
            else s_all_fail++;
            ZT("all",
               "check[%d/%d] name=%s canvas=%dx%d want_h=%d inline=%d pushes=+%u fg=%s "
               "result=%s",
               s_all_idx + 1, n, kAllApps[s_all_idx], cw, ch, want,
               (int)ui_sysbar::inlineMode(), (unsigned)(p - s_all_push0), zd_fg_app(),
               ok ? "PASS" : "FAIL");
            s_all_idx++;
            s_all_stage = 0;
            s_all_ts = now + 200;
            break;
        }
    }
}

/* ---------------- 应用全量自测 ZDAPPTEST ----------------
 * 逐个把注册表里的 app 拉起, 每一项判 5 件事并给 PASS/FAIL:
 *   1) open   能打开(install + openApp 成功, 前台不是桌面);
 *   2) frame  画面不是空白/花屏(采样统计: 颜色数 + 亮度);
 *   3) key    注入方向键后画面有变化(按键有响应);
 *   4) alive  测完仍在运行(没崩、没被看门狗重启);
 *   5) back   G0 侧键/Esc 能回到桌面。
 * 每项 check 行后停留 3.4s, 主机脚本在这段时间发 ZDSHOT 存图。 */
struct ZdFrameSig {
    int w = 0, h = 0;
    uint32_t hash = 0;
    int colors = 0;
    int bright = 0;
    int lit = 0;   /* 亮像素(有内容的证据): 采样点里 luma>30 的个数 */
    bool ok = false;
};

static void zd_inject_key(KeScanCode_t code, const char* name, bool down);

static ZdFrameSig zd_frame_sig()
{
    ZdFrameSig s;
    auto& shared = GetHAL().canvas();
    LGFX_Sprite* cvp = &shared;
    if (!shared.getBuffer() || shared.width() <= 0) {
        /* 只在 inlineMode 成立时读 app 自己的 sprite: 应用关闭后指针可能已释放 */
        if (ui_sysbar::inlineMode()) {
            const void* reg = ui_sysbar::appCanvasSprite();
            if (reg) cvp = (LGFX_Sprite*)reg;
        }
    }
    if (!cvp->getBuffer()) return s;
    const int w = (int)cvp->width();
    const int h = (int)cvp->height();
    if (w <= 0 || h <= 0 || w > 400) return s;

    static RGBColor row[400];
    uint16_t uniq[32];
    int nuniq = 0;
    uint32_t hash = 2166136261u;
    long sum = 0;
    int cnt = 0;
    int lit = 0;
    const int stepY = (h > 16) ? (h / 16) : 1;
    for (int y = 0; y < h; y += stepY) {
        cvp->readRectRGB(0, y, w, 1, row);
        for (int x = 0; x < w; x += 7) {
            const uint16_t v = (uint16_t)(((row[x].r & 0xF8) << 8) |
                                          ((row[x].g & 0xFC) << 3) | (row[x].b >> 3));
            hash = (hash ^ v) * 16777619u;
            sum += (row[x].r + row[x].g + row[x].b) / 3;
            if ((row[x].r + row[x].g + row[x].b) > 90) lit++;
            cnt++;
            bool hit = false;
            for (int i = 0; i < nuniq; i++) {
                if (uniq[i] == v) { hit = true; break; }
            }
            if (!hit && nuniq < 32) uniq[nuniq++] = v;
        }
    }
    s.w = w;
    s.h = h;
    s.hash = hash;
    s.colors = nuniq;
    s.bright = cnt ? (int)(sum / cnt) : 0;
    s.lit = lit;
    s.ok = (cnt > 0);
    return s;
}

static int8_t s_at_stage = -1;
static int s_at_idx = 0;
static int s_at_pass = 0;
static int s_at_fail = 0;
static uint32_t s_at_ts = 0;
static uint32_t s_at_heap0 = 0;
static uint32_t s_at_push0 = 0;
static ZdFrameSig s_at_sig1;
static ZdFrameSig s_at_sig2;
static bool s_at_open = false;
static bool s_at_react = false;
static int s_at_retry = 0;

static int zd_apptest_count()
{
    return (int)GetAppRegistry().size();
}

static const char* zd_apptest_name(int idx)
{
    auto& reg = GetAppRegistry();
    int n = 0;
    for (size_t i = 0; i < reg.size(); i++) {
        if (n == idx) return reg[i].name.c_str();
        n++;
    }
    return "";
}

static void apptest_begin(int start = 0)
{
    s_at_idx = (start < 0) ? 0 : start;
    s_at_pass = 0;
    s_at_fail = 0;
    s_at_stage = 0;
    s_at_ts = zdlink_now_ms();
    ZT("apptest", "start apps=%d", zd_apptest_count());
}

static void apptest_tick()
{
    if (s_at_stage < 0) return;
    uint32_t now = zdlink_now_ms();
    if ((int32_t)(now - s_at_ts) < 0) return;
    const int n = zd_apptest_count();
    const char* name = zd_apptest_name(s_at_idx);

    switch (s_at_stage) {
        case 0: /* 清场(close 异步), 留出退出动画时间 */
            if (s_at_idx >= n) {
                s_at_stage = -1;
                ZT("apptest", "summary result=%s pass=%d fail=%d total=%d",
                   (s_at_fail == 0) ? "PASS" : "FAIL", s_at_pass, s_at_fail, n);
                return;
            }
            close_other_apps(-1);
            s_at_stage = 1;
            s_at_ts = now + 900;
            break;

        case 1:
            s_at_heap0 = esp_get_free_heap_size();
            s_at_push0 = ui_sysbar::pushes();
            s_at_retry = 0;
            s_at_open = open_app_named(name);
            ZT("apptest", "open[%d/%d] name=%s ok=%d heap=%u", s_at_idx + 1, n, name,
               (int)s_at_open, (unsigned)s_at_heap0);
            s_at_stage = 2;
            s_at_ts = now + 1200;
            break;

        case 2:
            s_at_sig1 = zd_frame_sig();
            zd_inject_key(KEY_RIGHT, "right", true);
            s_at_stage = 3;
            s_at_ts = now + 650;
            break;

        case 3:
            s_at_sig2 = zd_frame_sig();
            s_at_react = (s_at_sig2.ok && s_at_sig1.ok && s_at_sig2.hash != s_at_sig1.hash);
            if (!s_at_react) {
                zd_inject_key(KEY_DOWN, "down", true);
                s_at_stage = 4;
                s_at_ts = now + 650;
            } else {
                s_at_stage = 5;
                s_at_ts = now + 700;
            }
            break;

        case 4: {
            ZdFrameSig s3 = zd_frame_sig();
            s_at_react = (s3.ok && s_at_sig2.ok && s3.hash != s_at_sig2.hash);
            s_at_stage = 5;
            s_at_ts = now + 700;
            break;
        }

        case 5: { /* 主机脚本看到这行会立刻发 ZDSHOT 存图 */
            const bool running = (strcmp(zd_fg_app(), "Launcher") != 0);
            /* 深色游戏(俄罗斯方块)首帧可能只有边框和分数, 方块还没落下:
             * 判空白就再采两次(每次间隔 800ms), 避免把"还没画出来"误判成失败 */
            if (s_at_retry > 0) s_at_sig1 = zd_frame_sig();
            const bool canvas_ok = s_at_sig1.ok &&
                                   (s_at_sig1.w == (int)GetHAL().display().width()) &&
                                   (s_at_sig1.h >= 100) && (s_at_sig1.h <= 135);
            /* 深色小游戏(贪吃蛇/俄罗斯方块)首屏只有几个像素块, 用"有 2 种以上颜色
             * 且至少有 3 个亮采样点"判非空白; 纯黑屏是 1 色 0 亮点, 仍会被判失败。 */
            const bool frame_ok = s_at_sig1.ok && (s_at_sig1.colors >= 2) &&
                                  (s_at_sig1.lit >= 3);
            if (!frame_ok && s_at_retry < 2 && running) {
                s_at_retry++;
                s_at_ts = now + 800;
                break;
            }
            ZT("apptest",
               "check[%d/%d] name=%s canvas=%dx%d ok=%d frame=%d colors=%d bright=%d lit=%d "
               "hash=%08lX key_react=%d running=%d pushes=+%u",
               s_at_idx + 1, n, name, s_at_sig1.w, s_at_sig1.h, (int)canvas_ok, (int)frame_ok,
               s_at_sig1.colors, s_at_sig1.bright, s_at_sig1.lit, (unsigned long)s_at_sig1.hash,
               (int)s_at_react, (int)running,
               (unsigned)(ui_sysbar::pushes() - s_at_push0));
            s_at_stage = 6;
            /* 停留 4.6s: 主机在这段时间发 ZDSHOT(整屏 base64 约 2.5~4s)存图 */
            s_at_ts = now + 4600;
            break;
        }

        case 6: /* 返回桌面: 走用户真实退出口(G0 侧键), 同时补一个 Esc 给游戏类 */
            GetHAL().goButton().injectClick();
            zd_inject_key(KEY_ESC, "esc", true);
            s_at_stage = 8;
            s_at_ts = now + 800;
            break;

        case 8: /* 有的多页应用(天链)第一次 G0 只是回首页, 再按一次才退出 */
            if (strcmp(zd_fg_app(), "Launcher") != 0) {
                GetHAL().goButton().injectClick();
                s_at_stage = 7;
                s_at_ts = now + 900;
            } else {
                s_at_stage = 7;
                s_at_ts = now + 100;
            }
            break;

        case 7: {
            const bool running = (strcmp(zd_fg_app(), "Launcher") != 0);
            const bool canvas_ok = s_at_sig1.ok &&
                                   (s_at_sig1.w == (int)GetHAL().display().width()) &&
                                   (s_at_sig1.h >= 100) && (s_at_sig1.h <= 135);
            /* 深色小游戏(贪吃蛇/俄罗斯方块)首屏只有几个像素块, 用"有 2 种以上颜色
             * 且至少有 3 个亮采样点"判非空白; 纯黑屏是 1 色 0 亮点, 仍会被判失败。 */
            const bool frame_ok = s_at_sig1.ok && (s_at_sig1.colors >= 2) &&
                                  (s_at_sig1.lit >= 3);
            const bool back_ok = !running;
            /* 判定口径: 能打开、画面正常、能退回桌面 = 该应用可用。
             * 按键响应单独记录(key=%d), 万用表这类常驻监控应用本来就没有界面按键,
             * 不把"无按键响应"算成崩溃/花屏级别的失败。 */
            const bool ok = s_at_open && canvas_ok && frame_ok && back_ok;
            if (ok) s_at_pass++;
            else s_at_fail++;
            ZT("apptest",
               "done[%d/%d] name=%s open=%d canvas=%d frame=%d key=%d back=%d heap_delta=%d "
               "result=%s",
               s_at_idx + 1, n, name, (int)s_at_open, (int)canvas_ok, (int)frame_ok,
               (int)s_at_react, (int)back_ok,
               (int)s_at_heap0 - (int)esp_get_free_heap_size(), ok ? "PASS" : "FAIL");
            if (running) close_other_apps(-1); /* 没退出去: 强行收掉, 不阻塞后面的项 */
            s_at_idx++;
            s_at_stage = 0;
            s_at_ts = now + 900;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  串口按键注入: 与物理键盘走同一条 onKeyEvent 链路(自动化测试用)      */
/* ------------------------------------------------------------------ */
static void zd_inject_key(KeScanCode_t code, const char* name, bool down)
{
    snprintf(s_inj_name, sizeof(s_inj_name), "%s", name ? name : "inj");
    s_inj_code = (int)code;
    s_inj_down = down ? 1 : 0;
}

/* 名字 -> 键码; 未知名返回 false */
static bool zd_key_code(const std::string& k, KeScanCode_t& code)
{
    code = (KeScanCode_t)0;
    if (k == "enter") code = KEY_ENTER;
    else if (k == "up") code = KEY_UP;
    else if (k == "down") code = KEY_DOWN;
    else if (k == "left") code = KEY_LEFT;
    else if (k == "right") code = KEY_RIGHT;
    else if (k == "l") code = KEY_L;
    else if (k == "g") code = KEY_G;
    else if (k == "x") code = KEY_X;
    else if (k == "k") code = KEY_K;
    else if (k == "u") code = KEY_U;
    else if (k == "v") code = KEY_V;
    else if (k == "h") code = KEY_H;
    else if (k == "r") code = KEY_R;
    else if (k == "p") code = KEY_P;
    else if (k == "t") code = KEY_T;
    else if (k == "i") code = KEY_I;
    else if (k == "y") code = KEY_Y;
    else if (k == "c") code = KEY_C;
    /* 数字键: 五子棋的 1/2/3 是"玩家作弊"三招, 自动化要能注入 */
    else if (k == "1") code = KEY_1;
    else if (k == "2") code = KEY_2;
    else if (k == "3") code = KEY_3;
    else if (k == "esc") code = KEY_ESC;
    else if (k == "equal") code = KEY_EQUAL;
    else if (k == "tab") code = KEY_TAB;
    else if (k == "space") code = KEY_SPACE;
    /* 符号键: 注入时 keyName 也必须是该符号, 计算器一类按 keyName 取字符的 app 才收得到 */
    else if (k == "+") code = KEY_EQUAL;
    else if (k == "-") code = KEY_MINUS;
    else if (k == "*") code = KEY_8;
    else if (k == "/") code = KEY_SLASH;
    else if (k == "(") code = KEY_9;
    else if (k == ")") code = KEY_0;
    else if (k == "^") code = KEY_6;
    else if (k == "`") code = KEY_GRAVE;
    /* 其余单字符: A-Z / 0-9 直接按 HID 连续段换算, 方便远程验证各 app 的按键 */
    else if (k.size() == 1) {
        char c = k[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') code = (KeScanCode_t)(KEY_A + (c - 'A'));
        else if (c >= '0' && c <= '9') code = (KeScanCode_t)(KEY_1 + ((c == '0') ? 9 : (c - '1')));
    }
    return code != 0;
}

void desktop_task(void *pvParameters)
{
    mclog::tagInfo("Task", "Desktop task started");
    
    while (1) {
        GetHAL().update();
        flow_tick();  /* 全链路自测状态机(空闲时不做事) */
        bar_tick();   /* 全局状态栏自测状态机(空闲时不做事) */
        go_tick();    /* 通用打开 app(ZDGO:Name, 取证用) */
        all_tick();   /* 全屏 app 逐一复核(ZDALL) */
        apptest_tick(); /* 应用全量自测(ZDAPPTEST) */

        GetHAL().feedTheDog();
    }
}

extern "C" void app_main(void)
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable Brownout Detector

    // Setup logger
    mclog::set_level(mclog::level_debug);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // 复位原因: 用于区分正常开机 / panic / 看门狗 / 掉电
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* rr_str    = "UNKNOWN";
        switch (rr) {
            case ESP_RST_POWERON:  rr_str = "POWERON"; break;
            case ESP_RST_EXT:      rr_str = "EXTERNAL"; break;
            case ESP_RST_SW:       rr_str = "SOFTWARE"; break;
            case ESP_RST_PANIC:    rr_str = "PANIC(crash)"; break;
            case ESP_RST_INT_WDT:  rr_str = "INT_WDT"; break;
            case ESP_RST_TASK_WDT: rr_str = "TASK_WDT"; break;
            case ESP_RST_WDT:      rr_str = "OTHER_WDT"; break;
            case ESP_RST_BROWNOUT: rr_str = "BROWNOUT"; break;
            case ESP_RST_DEEPSLEEP:rr_str = "DEEPSLEEP"; break;
            default: break;
        }
        ESP_LOGW("BOOT", "=== reset_reason=%d (%s) heap=%u ===", (int)rr, rr_str,
                 (unsigned)esp_get_free_heap_size());
    }

    // NVS 必须最先初始化：HAL 里 Settings("system") 要靠 nvs_open 拿句柄。
    // 以前顺序反了，句柄恒为 0，所有设置只读到默认值、写入直接 ESP_ERROR_CHECK 崩溃。
    ESP_LOGI("MAIN", "=== DEBUG: Before NVS init ===");
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);
    }
    ESP_LOGI("MAIN", "=== DEBUG: After NVS init ===");
    perf_boot("nvs_done");

    // HAL init
    ESP_LOGI("MAIN", "=== DEBUG: Before HAL init ===");
    GetHAL().init();
    ESP_LOGI("MAIN", "=== DEBUG: After HAL init ===");
    perf_boot("hal_done");

    // Security Init
    ESP_LOGI("MAIN", "=== DEBUG: Before SecurityManager ===");
    sys_security::SecurityManager::get().init();
    ESP_LOGI("MAIN", "=== DEBUG: After SecurityManager ===");
    perf_boot("security_done");

    // Check for previous crash
    // crash_handler::check_for_crash(); // SKIP: blocks on previous crash state

    // Auto connect WiFi - Removed to speed up boot
    // std::string ssid = GetHAL().getSettings().GetString("wifi_ssid");
    // std::string password = GetHAL().getSettings().GetString("wifi_password");
    // if (!ssid.empty()) {
    //     mclog::tagInfo("System", "Auto connecting to WiFi: {}", ssid);
    //     GetHAL().wifiConnect(ssid, password);
    // }

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // === Lazy loading: register apps (metadata + factory only, NO instantiation) ===
    // Apps are instantiated ONLY when opened, and freed when closed.
    ESP_LOGI("MAIN", "=== DEBUG: Registering apps (lazy) ===");
    
    RegisterApp<AppMultimeter>("Multimeter");
    RegisterApp<AppSetWiFi>("Network");
    RegisterApp<AppClock>("Clock");
    // RegisterApp<AppKeyboard>("Keyboard");  // DISABLED in CMakeLists
    RegisterApp<AppImu>("IMU");
    RegisterApp<AppStringIRToolKit>("StringIR");
    RegisterApp<AppScientificCalculator>("SciCalc");
    RegisterApp<AppFunctionCalculator>("FuncCalc");
    RegisterApp<AppSysInfo>("SysInfo");
    RegisterApp<AppDHT11>("Temp/Humi+");
    RegisterApp<AppUltrasonic>("Distance");
    RegisterApp<AppGomoku>("Gomoku");
    RegisterApp<AppShogi>("Shogi");
    RegisterApp<AppFPS>("FPS");
    RegisterApp<App2048>("2048");
    RegisterApp<AppAI>("DeepSeek");
    RegisterApp<AppRace>("Racing");
    RegisterApp<AppSnake>("Snake");
    RegisterApp<AppBreakout>("Breakout");
    RegisterApp<AppTetris>("Tetris");
    RegisterApp<AppPong>("Pong");
    RegisterApp<AppReversi>("Reversi");
    RegisterApp<AppTank>("Tank");
    RegisterApp<AppUno>("UNO");
    RegisterApp<AppConnect4>("Connect4");
    RegisterApp<AppCheckers>("Checkers");
    RegisterApp<AppFlappy>("Flappy");
    RegisterApp<AppInvaders>("Invaders");
    RegisterApp<AppMinesweeper>("Minesweeper");
    RegisterApp<AppWaterLevel>("Water Level");
    RegisterApp<AppRelayTest>("RelayTester");
    RegisterApp<AppDoudizhu>("Doudizhu");
    RegisterApp<AppMicMeter>("MicMeter");
    RegisterApp<AppIrMote>("IR Remote");
    RegisterApp<AppWhack>("Whack");
    RegisterApp<AppBlackjack>("Blackjack");
    RegisterApp<AppMaze>("Maze");
    RegisterApp<AppSettings>("设置");
    RegisterApp<AppZdMsg>("天链");

    // 全局通知层: 复用 240x26 系统栏画布, 不新增缓冲
    ui_notify::init();
    // 全局状态栏(所有 app/游戏共用): 同样复用系统栏画布, 由它独占叠加层,
    // 通知活跃时转调 ui_notify::draw()。必须在 ui_notify::init() 之后。
    ui_sysbar::init();
    // '=' / '+' 直达消息面板: 键盘回调只置标志, 真正开 app 在 app_task 里做。
    // 面板在前台时直接放行, 保证输入页里 '=' 仍是普通字符。
    GetHAL().keyboard.onKeyEvent.connect([](const Keyboard::KeyEvent_t& e) {
        if (!e.state || e.isModifier) return;
        if (e.keyCode != KEY_EQUAL) return;  // '=' 与 '+' 同码
        if (!ui_notify::active()) return;    // 没有待看的通知就不抢键
        if (zdmsg_is_open()) return;         // 输入优先
        ui_notify::request_open();
    });
    
    ESP_LOGI("MAIN", "=== DEBUG: %d apps registered ===", (int)GetAppRegistry().size());
    ESP_LOGI("MAIN", "=== DEBUG: free heap after register = %u KB ===",
             (unsigned)(esp_get_free_heap_size() / 1024));
    perf_boot("apps_registered");
    
    // Only Launcher stays resident (system app)
    GetMooncake().installApp(std::make_unique<Launcher>());
    ESP_LOGI("MAIN", "=== DEBUG: Launcher installed ===");
    ESP_LOGI("MAIN", "=== DEBUG: free heap after launcher = %u KB, min = %u KB ===",
             (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    perf_boot("launcher_installed");

    // START SYSTEM WIFI (Daemon)
    // mclog::tagInfo("System", "Starting System WiFi Daemon...");
    // register_system_callback(system_cmd_handler);
    // system_wifi.init(NULL);

    // Create FreeRTOS tasks
    audio::set_keyboard_sfx_enable(true);
    
    // Create desktop task
    xTaskCreate(&desktop_task, "desktop_task", DESKTOP_TASK_STACK_SIZE, NULL, DESKTOP_TASK_PRIORITY, &desktop_task_handle);
    
    // Create app task
    xTaskCreate(&app_task, "app_task", APP_TASK_STACK_SIZE, NULL, APP_TASK_PRIORITY, &app_task_handle);
    
    // Start the app task
    app_task_running = true;
    perf_boot("tasks_created");
    
    // === 开机静默自检 (O8): 快速硬件检测写 SD, 不打扰界面 ===
    {
        FILE* f = fopen("/sdcard/boot_selftest.txt", "w");
        if (f) {
            uint32_t before = esp_get_free_heap_size();
            fprintf(f, "BOOT SELFTEST\n");
            fprintf(f, "heap=%u largest=%u\n", (unsigned)before,
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            fprintf(f, "reset_reason=%d\n", (int)esp_reset_reason());
            fprintf(f, "battery=%d%%\n", (int)GetHAL().getBatLevel());
            fprintf(f, "display=OK (launcher rendered)\n");
            fprintf(f, "serial_pump=OK (listening)\n");
            fprintf(f, "crypto=OK (key loaded)\n");
            fclose(f);
            ESP_LOGI("MAIN", "boot selftest saved");
        }
    }
    
    // The main task can now be used for other purposes or just idle
    while (1) {
        SerialPump::pump(); // 系统级串口泵: 防 USB CDC 缓冲满死机 + 菜单下响应指令

        // 自测指令优先: 串口发 "HEAPTEST" → 逐个 app 测堆占用/泄漏
        if (SerialPump::takePending("HEAPTEST")) {
            run_heap_test();
        }
        // 压测: "SOAK" → 3 轮遍历全部 app, 检验长时间开关后堆是否回退
        if (SerialPump::takePending("SOAK")) {
            run_soak_test();
        }

        // 自测: "KEYTEST" → 20 秒内每 500ms 注入一次方向右键, 由菜单打印软件延迟
        {
            static uint32_t keytest_until = 0;
            static uint32_t keytest_next  = 0;
            static bool     keytest_down  = false;

            if (SerialPump::takePending("KEYTEST")) {
                keytest_until = perf_ms() + 20000;
                keytest_next  = perf_ms() + 300;
                keytest_down  = false;
                ESP_LOGI("PERF", "KEYTEST start (inject Right every 500ms)");
            }
            if (keytest_until != 0 && perf_ms() < keytest_until && perf_ms() >= keytest_next) {
                keytest_down = !keytest_down;
                keytest_next = perf_ms() + 500;
                GetHAL().keyboard.injectRawEvent(3, 12, keytest_down); // Right
            }
        }

        // === ZD v1 自测(无键盘验证用):
        //   ZDOPEN 打开「天链」面板; ZDSEND 发一条文本(等 C3 回 ACK);
        //   ZDSTAT 打印收发统计与节点表; ZDGAME 打开 Breakout 验证镜像与 ZD 共存
        if (SerialPump::takePending("ZDOPEN")) {
            bool ok = open_zdmsg_app();
            ESP_LOGI("ZD", "selftest open LoRaMsg ok=%d heap=%u", (int)ok,
                     (unsigned)esp_get_free_heap_size());
        }
        if (SerialPump::takePending("ZDSEND")) {
            uint16_t mid = 0, seq = 0;
            bool ok = zdlink_send_text("hello from cardputer", 20, &mid, &seq);
            ESP_LOGI("ZD", "selftest send ok=%d msgid=%u seq=%u", (int)ok, (unsigned)mid,
                     (unsigned)seq);
        }
        if (SerialPump::takePending("ZDSTAT")) {
            uint32_t rx = 0, bad = 0, tx = 0, ack = 0;
            zdlink_stats(&rx, &bad, &tx, &ack);
            ZdNodeInfo nodes[8];
            int n = zdlink_get_nodes(nodes, 8);
            ESP_LOGI("ZD", "selftest stat ready=%d rx=%lu bad=%lu tx=%lu ack=%lu nodes=%d",
                     (int)zdlink_ready(), (unsigned long)rx, (unsigned long)bad, (unsigned long)tx,
                     (unsigned long)ack, n);
            for (int i = 0; i < n; i++) {
                ESP_LOGI("ZD", "  node %08lX rssi=%d hops=%u mv=%u pct=%u role=%u snr4=%d lora=%u lrssi=%d",
                         (unsigned long)nodes[i].id, (int)nodes[i].rssi, (unsigned)nodes[i].hops,
                         (unsigned)nodes[i].batt_mv, (unsigned)nodes[i].batt_pct,
                         (unsigned)nodes[i].role, (int)nodes[i].snr, (unsigned)nodes[i].lora,
                         (int)nodes[i].lora_rssi);
            }
        }
        if (SerialPump::takePending("ZDTYPE")) {
            /* 注入键盘事件走面板真实的 回调→队列→按键处理→输入→发送 路径(不经过硬件矩阵):
             * Enter(菜单→消息列表) → Enter(→写消息) → h → i → Enter(发送) */
            auto type_key = [](KeScanCode_t code, const char* name) {
                Keyboard::KeyEvent_t e;
                e.state = true;
                e.isModifier = false;
                e.keyCode = code;
                e.keyName = name;
                GetHAL().keyboard.onKeyEvent.emit(e);
            };
            type_key(KEY_ENTER, "enter");
            type_key(KEY_ENTER, "enter");
            type_key(KEY_H, "h");
            type_key(KEY_I, "i");
            type_key(KEY_ENTER, "enter");
            ESP_LOGI("ZD", "selftest type 'hi' + enter");
        }
        if (SerialPump::takePending("ZDTAB")) {
            /* 验证面板的 R 目标: 进入写消息 → Tab 切到"经 C3 → LoRa" → j → Enter
             * 走 app 自己的 send_input → zdlink_send_lora_text(单播桥) */
            auto type_key = [](KeScanCode_t code, const char* name) {
                Keyboard::KeyEvent_t e;
                e.state = true;
                e.isModifier = false;
                e.keyCode = code;
                e.keyName = name;
                GetHAL().keyboard.onKeyEvent.emit(e);
            };
            type_key(KEY_ENTER, "enter");
            type_key(KEY_ENTER, "enter");
            type_key(KEY_TAB, "tab");
            type_key(KEY_J, "j");
            type_key(KEY_ENTER, "enter");
            ESP_LOGI("ZD", "selftest tab->lora + 'j' + enter");
        }
        if (SerialPump::takePending("ZDCLOSE")) {
            auto list = GetMooncake().getAppAbilityManager()->getAllAbilityInstance();
            for (auto& raw : list) {
                auto* app = static_cast<AppAbility*>(raw);
                if (app->getAppInfo().name == "天链" &&
                    GetMooncake().getAppCurrentState(app->getId()) == AppAbility::StateRunning) {
                    app->close();
                    ESP_LOGI("ZD", "selftest close LoRaMsg");
                    break;
                }
            }
        }
        if (SerialPump::takePending("ZDRX")) {
            /* 本地注入一条 TEXT(不走空口), 验证 收包→事件→面板列表 */
            zdlink_selftest_rx_text(0x7DF43F68, "测试消息：面板接收链路 OK");
            ESP_LOGI("ZD", "selftest inject local TEXT");
        }
        if (SerialPump::takePending("ZDBRIDGE")) {
            ZdBridge br[4];
            int n = zdlink_get_bridges(br, 4);
            ESP_LOGI("ZD", "selftest bridges=%d", n);
            for (int i = 0; i < n; i++) {
                ESP_LOGI("ZD", "  bridge %08lX caps=0x%02X ago=%lu", (unsigned long)br[i].id,
                         (unsigned)br[i].caps,
                         (unsigned long)(zdlink_now_ms() - br[i].seen_ms));
            }
        }
        if (SerialPump::takePending("ZDLORA")) {
            /* 真实联调: 中文文本 → 桥单播 → C3 上空口 */
            static const char kTxt[] = "ADV 中文测试：你好 LoRa";
            ZdBridge br[4];
            int n = zdlink_get_bridges(br, 4);
            uint32_t bid = 0;
            for (int i = 0; i < n; i++) {
                if ((br[i].caps & 0x03) == 0x03) { bid = br[i].id; break; }
            }
            uint16_t mid = 0, seq = 0;
            bool ok = bid && zdlink_send_lora_text(kTxt, (int)(sizeof(kTxt) - 1), bid, &mid, &seq);
            ESP_LOGI("ZD", "selftest lora send bridge=%08lX ok=%d msgid=%u seq=%u",
                     (unsigned long)bid, (int)ok, (unsigned)mid, (unsigned)seq);
        }
        if (SerialPump::takePending("ZDLORAA")) {
            /* 故意发往"有桥能力但禁发"的设备(如 AA:D0), 验证真实 LNACK(reason=1) */
            static const char kTxt[] = "probe disabled bridge";
            ZdBridge br[4];
            int n = zdlink_get_bridges(br, 4);
            uint32_t bid = 0;
            for (int i = 0; i < n; i++) {
                if ((br[i].caps & 0x01) && !(br[i].caps & 0x02)) { bid = br[i].id; break; }
            }
            if (!bid) {
                for (int i = 0; i < n; i++) {
                    if ((br[i].id & 0xFFFF) == 0xAAD0) { bid = br[i].id; break; }
                }
            }
            uint16_t mid = 0, seq = 0;
            bool ok = bid && zdlink_send_lora_text(kTxt, (int)(sizeof(kTxt) - 1), bid, &mid, &seq);
            ESP_LOGI("ZD", "selftest lora probe disabled bridge=%08lX ok=%d msgid=%u seq=%u",
                     (unsigned long)bid, (int)ok, (unsigned)mid, (unsigned)seq);
        }
        if (SerialPump::takePending("ZDLTEXT")) {
            /* 注入 LTEXT(模拟桥回传), 验证 L 标记 + 节点页 snr/hops */
            zdlink_selftest_rx_ltext(0x7DF331D0u, 0x7DF43F68u, -65, 17, 1,
                                     "LoRa 收到的中文测试 (inject)");
            ESP_LOGI("ZD", "selftest inject LTEXT");
        }
        if (SerialPump::takePending("ZDLNACK")) {
            zdlink_selftest_rx_lnack(3);
            ESP_LOGI("ZD", "selftest inject LNACK reason=3");
        }
        if (SerialPump::takePending("ZDGAME")) {
            ESP_LOGI("ZD", "selftest open Breakout ok=%d", (int)open_app_named("Breakout"));
        }

        /* ---- 全局状态栏(所有游戏/应用共用) ---- */
        if (SerialPump::takePending("ZDBAR")) {
            bar_begin();
        }
        /* 全屏 app 逐一复核: 每个都开一遍, 断言画布 121 高 + 内联条 + 26px 不再推 */
        if (SerialPump::takePending("ZDALL")) {
            all_begin();
        }
        if (SerialPump::takePending("ZDLINKE")) {
            ui_sysbar::force_link(ZD_VIA_E, 15000);
            ESP_LOGI("ZD", "selftest sysbar force ESP-NOW 15s");
        }
        if (SerialPump::takePending("ZDLINKL")) {
            ui_sysbar::force_link(ZD_VIA_L, 15000);
            ESP_LOGI("ZD", "selftest sysbar force LoRa 15s");
        }
        if (SerialPump::takePending("ZDGOMOKU")) {
            ESP_LOGI("ZD", "selftest open Gomoku ok=%d", (int)open_app_named("Gomoku"));
        }
        if (SerialPump::takePending("ZDRACE")) {
            ESP_LOGI("ZD", "selftest open Racing ok=%d", (int)open_app_named("Racing"));
        }
        /* 赛车手感自测: 打开日志 + 可注入倾斜量(-1..1, 99=恢复真实 IMU) */
        if (SerialPump::takePending("ZDRACEDBG")) {
            race_set_debug(true);
            ESP_LOGI("ZD", "selftest race dbg on, open Racing ok=%d",
                     (int)open_app_named("Racing"));
        }
        {
            std::string t = SerialPump::takePendingPrefix("ZDRACETILT:");
            if (!t.empty()) {
                float v = atof(t.c_str());
                if (v > 90.0f) v = NAN;
                race_set_test_tilt(v);
                ESP_LOGI("ZD", "selftest race tilt=%.2f", v);
            }
        }
        /* 应用全量自测: 逐个拉起 -> 抓帧/按键/返回 -> 逐项 PASS/FAIL
         * ZDAPPTEST: / ZDAPPTEST:<idx> 可从第 idx 项(0 基)续跑 */
        if (SerialPump::takePending("ZDAPPTEST")) {
            apptest_begin(0);
        }
        {
            std::string a = SerialPump::takePendingPrefix("ZDAPPTEST:");
            if (!a.empty()) apptest_begin(atoi(a.c_str()));
        }

        /* 全链路自测: 一条命令跑完 收到→通知→'='跳转→回复→ACK→降级 */
        if (SerialPump::takePending("ZDFLOW")) {
            flow_begin();
        }

        /* 对账: 收到多少条消息 / 弹了几次通知 / 被去重/CRC 丢了多少, 对不上直接写差多少 */
        if (SerialPump::takePending("ZDLEDGER")) {
            uint32_t text = 0, lora = 0, dup = 0, loop = 0;
            uint32_t rx = 0, bad = 0, tx = 0, ack = 0;
            zdlink_text_stats(&text, &lora, &dup, &loop);
            zdlink_stats(&rx, &bad, &tx, &ack);

            uint32_t n_rx = ui_notify::count_rx();
            uint32_t shown = ui_notify::count_shown();
            uint32_t skip = ui_notify::count_skip();
            uint32_t simple = ui_notify::count_simple();
            uint32_t msgs = text + lora - loop;
            bool ok = (n_rx == msgs) && (shown + skip == n_rx);
            ZT("ledger",
               "rx_text=%u rx_lora=%u loop=%u dup=%u bad=%u tx=%u ack=%u | ntf_rx=%u shown=%u "
               "skip=%u simple=%u unread=%u | msgs=%u fg=%s heap=%u result=%s",
               (unsigned)text, (unsigned)lora, (unsigned)loop, (unsigned)dup, (unsigned)bad,
               (unsigned)tx, (unsigned)ack, (unsigned)n_rx, (unsigned)shown, (unsigned)skip,
               (unsigned)simple, (unsigned)ui_notify::unread(), (unsigned)msgs, zd_fg_app(),
               (unsigned)esp_get_free_heap_size(), ok ? "PASS" : "FAIL");
            if (!ok) {
                ZT("ledger", "why=count_mismatch ntf_rx-msgs=%d shown+skip-ntf_rx=%d",
                   (int)n_rx - (int)msgs, (int)(shown + skip) - (int)n_rx);
            }
        }

        /* ---- 实机取证: 画布帧缓冲以 base64 打到串口, 主机还原 PNG ---- */
        if (SerialPump::takePending("ZDSHOT")) {
            zd_shot_dump();
        }
        if (SerialPump::takePending("ZDFONT")) {
            /* 字号对比页必须由 app 任务画(串口 pump 在别的任务, 直接写画布会被下一帧覆盖) */
            zdmsg_font_demo(true);
            vTaskDelay(pdMS_TO_TICKS(200));
            zd_shot_dump();
            zdmsg_font_demo(false);
        }
        /* 方向键自测: 硬件是 4x14 矩阵, 没有独立方向键; 面板在非输入页把
         * ; , . / 当方向键(与其它 app 的用法一致), 这里注入这四个键码验证。 */
        if (SerialPump::takePending("ZDNAV")) {
            auto nav_key = [](KeScanCode_t code, const char* name) {
                Keyboard::KeyEvent_t e;
                e.state = true;
                e.isModifier = false;
                e.keyCode = code;
                e.keyName = name;
                GetHAL().keyboard.onKeyEvent.emit(e);
            };
            nav_key(KEY_DOT, ".");
            nav_key(KEY_SEMICOLON, ";");
            nav_key(KEY_COMMA, ",");
            nav_key(KEY_SLASH, "/");
            /* 再敲一个字符: 验证列表页"打字即写"——不按 Enter 也能直接进写消息 */
            nav_key(KEY_K, "k");
            ESP_LOGI("ZD", "selftest nav inject . ; , / + k");
        }
        /* 数字 1..5 直达页面(键盘快捷键) */
        {
            static const char* kPgCmd[5] = {"ZDPG1", "ZDPG2", "ZDPG3", "ZDPG4", "ZDPG5"};
            static const KeScanCode_t kPgKey[5] = {KEY_1, KEY_2, KEY_3, KEY_4, KEY_5};
            for (int i = 0; i < 5; i++) {
                if (!SerialPump::takePending(kPgCmd[i])) continue;
                Keyboard::KeyEvent_t e;
                e.state = true;
                e.isModifier = false;
                e.keyCode = kPgKey[i];
                e.keyName = kPgCmd[i] + 4;
                GetHAL().keyboard.onKeyEvent.emit(e);
                ESP_LOGI("ZD", "selftest page key %d", i + 1);
            }
        }

        /* 时间校准自测: 设置页 → 时间 → +2 小时 +5 分钟 → 保存 */
        if (SerialPump::takePending("ZDTIME")) {
            auto kk = [](KeScanCode_t c, const char* n) {
                Keyboard::KeyEvent_t e;
                e.state = true;
                e.isModifier = false;
                e.keyCode = c;
                e.keyName = n;
                GetHAL().keyboard.onKeyEvent.emit(e);
                vTaskDelay(pdMS_TO_TICKS(120));
            };
            kk(KEY_4, "4");                                 /* 进设置页 */
            for (int i = 0; i < 4; i++) kk(KEY_DOWN, "d");  /* 选到"时间" */
            kk(KEY_ENTER, "e");                             /* 进入编辑 */
            kk(KEY_UP, "u");
            kk(KEY_UP, "u");                                /* 小时 +2 */
            for (int i = 0; i < 5; i++) kk(KEY_RIGHT, "r"); /* 分钟 +5 */
            kk(KEY_ENTER, "e");                             /* 保存 */
            ESP_LOGI("ZD", "selftest time edit done");
        }

        /* 时间校准取证: 注入一条 GPS 来源的 ZT_TIME(协议 v1.2), 走 解析→采纳→顶栏 */
        if (SerialPump::takePending("ZDTIMEGPS")) {
            uint32_t utc = (uint32_t)::time(nullptr) + 3600;  /* 比本机快 1 小时, 便于肉眼确认 */
            zdlink_selftest_rx_time(utc, 1, 1);
            ESP_LOGI("ZD", "selftest inject TIME utc=%lu src=GPS", (unsigned long)utc);
        }

        /* 时间编辑态取证: 设置页 → 时间 → 进入编辑后停住, 便于截图 */
        if (SerialPump::takePending("ZDTEDIT")) {
            auto kk = [](KeScanCode_t c, const char* n) {
                Keyboard::KeyEvent_t e;
                e.state = true;
                e.isModifier = false;
                e.keyCode = c;
                e.keyName = n;
                GetHAL().keyboard.onKeyEvent.emit(e);
                vTaskDelay(pdMS_TO_TICKS(120));
            };
            kk(KEY_4, "4");
            for (int i = 0; i < 4; i++) kk(KEY_DOWN, "d");
            kk(KEY_ENTER, "e");
            ESP_LOGI("ZD", "selftest time edit state");
        }

        /* 通知取证: 在"共享 240x109 画布"的 app(万用表)里收消息, 便于整屏抓图 */
        if (SerialPump::takePending("ZDMULTI")) {
            ESP_LOGI("ZD", "selftest open Multimeter ok=%d", (int)open_app_named("Multimeter"));
        }

        /* 桌面菜单导航注入: 菜单读的是 raw(row,col) 而不是 onKeyEvent,
         * 所以单独给一条 ZDTAP:<方向> 走 injectRawEvent(按下 80ms 后松开)。 */
        {
            std::string kt = SerialPump::takePendingPrefix("ZDTAP:");
            if (!kt.empty()) {
                uint8_t r = 0, c = 0;
                bool ok = true;
                if (kt == "left") { r = 3; c = 10; }
                else if (kt == "right") { r = 3; c = 12; }
                else if (kt == "up") { r = 2; c = 11; }
                else if (kt == "down") { r = 3; c = 11; }
                else if (kt == "enter") { r = 2; c = 13; }
                else ok = false;
                if (ok) {
                    GetHAL().keyboard.injectRawEvent(r, c, true);
                    vTaskDelay(pdMS_TO_TICKS(80));
                    GetHAL().keyboard.injectRawEvent(r, c, false);
                    ESP_LOGI("ZD", "selftest nav tap '%s' (%u,%u) fg=%s", kt.c_str(),
                             (unsigned)r, (unsigned)c, zd_fg_app());
                } else {
                    ESP_LOGW("ZD", "selftest nav '%s' unknown", kt.c_str());
                }
            }
        }
        /* 通用按键注入: ZDKEY:<名字>, 与物理按键走同一条 onKeyEvent 链路,
         * 只用于自动化操作(如跟五子棋 AI 对局), 不改任何 app 逻辑。 */
        {
            std::string k = SerialPump::takePendingPrefix("ZDKEY:");
            std::string kr = SerialPump::takePendingPrefix("ZDKEYR:");
            if (!kr.empty()) { /* 松开(油门等需要按住-松开的场景) */
                KeScanCode_t code = (KeScanCode_t)0;
                if (zd_key_code(kr, code)) {
                    zd_inject_key(code, kr.c_str(), false);
                    ESP_LOGI("ZD", "selftest key '%s' released fg=%s", kr.c_str(), zd_fg_app());
                } else {
                    ESP_LOGW("ZD", "selftest key '%s' unknown", kr.c_str());
                }
            }
            if (!k.empty()) {
                KeScanCode_t code = (KeScanCode_t)0;
                if (zd_key_code(k, code)) {
                    zd_inject_key(code, k.c_str(), true);
                    ESP_LOGI("ZD", "selftest key '%s' injected fg=%s", k.c_str(), zd_fg_app());
                } else {
                    ESP_LOGW("ZD", "selftest key '%s' unknown", k.c_str());
                }
            }
        }
        /* 全局通知的 '=' 直达自测: 走键盘信号(与物理按键同一条链路) */
        if (SerialPump::takePending("ZDEQ")) {
            Keyboard::KeyEvent_t e;
            e.state = true;
            e.isModifier = false;
            e.keyCode = KEY_EQUAL;
            e.keyName = "=";
            GetHAL().keyboard.onKeyEvent.emit(e);
            ESP_LOGI("NTF", "selftest inject '=' key");
        }
        /* 通知降级取证: ZDNTLOW 临时把降级阈值抬到 200KB(实机堆约 90KB),
         * 让 update() 走"堆不够"的真实分支; ZDNTHI 恢复默认 46KB。 */
        if (SerialPump::takePending("ZDNTLOW")) {
            ui_notify::set_low_heap(200000);
            ESP_LOGI("NTF", "selftest low-heap threshold forced, next notice = simple");
        }
        if (SerialPump::takePending("ZDNTHI")) {
            ui_notify::set_low_heap(0);
            ESP_LOGI("NTF", "selftest low-heap threshold restored");
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

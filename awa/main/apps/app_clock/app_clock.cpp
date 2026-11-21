#include "app_clock.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include "../utils/ui_kit.h"
#include <ctime>

AppClock::AppClock() { setAppInfo().name = "Clock"; }
AppClock::~AppClock() {}

void AppClock::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _last_update = 0;
}

void AppClock::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - _last_update < 200) return;
    _last_update = now_ms;

    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);
    ui_title(cv, "时钟");
    render_interface();
    ui_hint(cv, "G0 返回");
    GetHAL().pushCanvas();
}

void AppClock::onClose() { mclog::tagInfo(getAppInfo().name, "closed"); }

void AppClock::render_interface() {
    auto& cv = GetHAL().canvas();

    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    bool synced = (tmv.tm_year + 1900) >= 2024;

    char buf[64];
    if (synced) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        int64_t sec = esp_timer_get_time() / 1000000;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", (int)((sec / 3600) % 24), (int)((sec / 60) % 60), (int)(sec % 60));
    }
    ui_big_value(cv, 120, 34, buf, nullptr, UI_ACCENT);

    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(UI_SUB);
    cv.setTextDatum(top_center);
    if (synced) {
        static const char* wd[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d  星期%s", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 wd[tmv.tm_wday % 7]);
        cv.drawString(buf, 120, 62);
    } else {
        cv.drawString("时间未同步（未连接网络）", 120, 62);
    }
    cv.setTextDatum(top_left);

    // 系统运行时长
    int64_t up = esp_timer_get_time() / 1000000;
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(0x6B4D);
    snprintf(buf, sizeof(buf), "已运行 %02d:%02d:%02d", (int)((up / 3600) % 100), (int)((up / 60) % 60), (int)(up % 60));
    cv.setTextDatum(top_center);
    cv.drawString(buf, 120, 82);
    cv.setTextDatum(top_left);
}

void AppClock::show_system_time() { render_interface(); }
void AppClock::show_network_time() { render_interface(); }
void AppClock::update_time_display() { render_interface(); }

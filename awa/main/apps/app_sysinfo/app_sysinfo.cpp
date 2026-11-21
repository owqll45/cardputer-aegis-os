#include "app_sysinfo.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <M5Unified.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_app_desc.h>
#include <soc/rtc.h>

AppSysInfo::AppSysInfo() { setAppInfo().name = "SysInfo"; }
AppSysInfo::~AppSysInfo() {}

void AppSysInfo::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");
    // 不使用全屏 sprite：屏幕底部 26px 属于系统状态栏，
    // 若用 135 高 sprite 覆盖它，退出后无法恢复，会留下长方形残影。
    _scroll   = 0;
    _last     = 0;
    _dirty    = true;
    _exit_req = false;

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (!e.state) return;
        int k = e.keyCode;
        if (k == KEY_E || k == KEY_UP) {
            if (_scroll > 0) { _scroll--; _dirty = true; }
            return;
        }
        if (k == KEY_S || k == KEY_DOWN) {
            _scroll++;
            _dirty = true;
            return;
        }
    });
}

void AppSysInfo::onRunning()
{
    if (_exit_req || GetHAL().goButton().wasClicked()) {
        _exit_req = false;
        close();
        return;
    }

    uint32_t now = GetHAL().millis();
    if (!_dirty && now - _last < 1000) return;
    _last  = now;
    _dirty = false;

    draw();
    GetHAL().pushCanvas();
}

void AppSysInfo::onClose()
{
    mclog::tagInfo(getAppInfo().name, "closed");
    if (_key_slot != -1) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    _scroll   = 0;
    _dirty    = false;
    _exit_req = false;
}

void AppSysInfo::draw()
{
    auto& cv = GetHAL().canvas();
    int w    = cv.width();
    int h    = cv.height();
    cv.fillScreen(TFT_BLACK);

    std::vector<std::string> lines;
    char buf[96];

    // ---- 芯片 ----
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(buf, sizeof(buf), "芯片: ESP32-S3 %d核 修订v%d.%d",
             chip.cores, chip.revision / 100, chip.revision % 100);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "CPU: %d MHz  IDF: %s",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_get_idf_version());
    lines.push_back(buf);

    // ---- Flash ----
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(buf, sizeof(buf), "Flash: %lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    lines.push_back(buf);

    // ---- 内存 ----
    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t heap_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "内存: 剩%lu/共%lu KB",
             (unsigned long)(heap_free / 1024), (unsigned long)(heap_total / 1024));
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "内存最低: %lu KB",
             (unsigned long)(esp_get_minimum_free_heap_size() / 1024));
    lines.push_back(buf);

    // ---- PSRAM ----
    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (ps_total > 0) {
        snprintf(buf, sizeof(buf), "PSRAM: 剩%lu/共%lu KB",
                 (unsigned long)(ps_free / 1024), (unsigned long)(ps_total / 1024));
    } else {
        snprintf(buf, sizeof(buf), "PSRAM: 无");
    }
    lines.push_back(buf);

    // ---- 电池 ----
    int   bat_level = M5.Power.getBatteryLevel();
    float bat_v     = M5.Power.getBatteryVoltage() / 1000.0f;
    float bat_cur   = M5.Power.getBatteryCurrent();
    bool  charging  = M5.Power.isCharging();
    snprintf(buf, sizeof(buf), "电池: %d%% (%.2fV %s)", bat_level, bat_v, charging ? "充电中" : "放电");
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "电流: %.0f mA", bat_cur);
    lines.push_back(buf);

    // ---- 运行时间 ----
    unsigned long sec = (unsigned long)(esp_timer_get_time() / 1000000ULL);
    snprintf(buf, sizeof(buf), "运行: %02lu:%02lu:%02lu", sec / 3600, (sec % 3600) / 60, sec % 60);
    lines.push_back(buf);

    // ---- MAC ----
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, sizeof(buf), "STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lines.push_back(buf);
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, sizeof(buf), "AP  MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lines.push_back(buf);

    // ---- 固件 ----
    const esp_app_desc_t* app = esp_app_get_description();
    snprintf(buf, sizeof(buf), "固件: %s v%s", app->project_name, app->version);
    lines.push_back(buf);
    snprintf(buf, sizeof(buf), "编译: %s %s", app->date, app->time);
    lines.push_back(buf);

    // ---- 布局 ----
    const int title_h = 15;
    const int hint_h  = 12;
    const int line_h  = 13;
    int visible       = (h - title_h - hint_h) / line_h;
    int total         = (int)lines.size();
    if (_scroll > total - visible) _scroll = total - visible;
    if (_scroll < 0) _scroll = 0;

    // 标题栏
    cv.fillRect(0, 0, w, title_h, 0x051F);
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(TFT_CYAN);
    cv.setCursor(4, 1);
    cv.print("系统信息");
    // 滚动位置指示
    if (total > visible) {
        snprintf(buf, sizeof(buf), "%d/%d", _scroll + 1, total - visible + 1);
        cv.setFont(&fonts::efontCN_10);
        cv.setTextColor(TFT_DARKGREY);
        cv.setCursor(w - 4 - cv.textWidth(buf), 2);
        cv.print(buf);
    }

    // 内容行
    cv.setFont(&fonts::efontCN_12);
    for (int i = 0; i < visible; i++) {
        int idx = _scroll + i;
        if (idx >= total) break;
        cv.setTextColor(TFT_WHITE);
        cv.setCursor(4, title_h + i * line_h + 1);
        cv.print(lines[idx].c_str());
    }

    // 右侧滚动条
    if (total > visible) {
        int bar_h   = h - title_h - hint_h;
        int thumb_h = bar_h * visible / total;
        if (thumb_h < 8) thumb_h = 8;
        int denom   = (total - visible) > 0 ? (total - visible) : 1;
        int thumb_y = title_h + (bar_h - thumb_h) * _scroll / denom;
        cv.fillRect(w - 3, title_h, 3, bar_h, 0x2104);
        cv.fillRect(w - 3, thumb_y, 3, thumb_h, 0x630C);
    }

    // 底部提示
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(TFT_DARKGREY);
    cv.setCursor(4, h - hint_h + 1);
    cv.print("E/S 滚动  G0 返回");
}

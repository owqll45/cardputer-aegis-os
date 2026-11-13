#include "app_wifi_scan.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <esp_wifi.h>

static uint32_t app_millis() { return esp_timer_get_time() / 1000; }

AppWifiScan::AppWifiScan() { setAppInfo().name = "WiFi Scan"; }
AppWifiScan::~AppWifiScan() {}

void AppWifiScan::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _time_count = 0; _scan_result.clear();
    GetHAL().wifiInit(); esp_wifi_start();
    wifi_scan_config_t cfg = { .ssid=nullptr, .bssid=nullptr, .channel=0, .show_hidden=false,
        .scan_type=WIFI_SCAN_TYPE_ACTIVE, .scan_time={.active={.min=100,.max=300}} };
    esp_wifi_scan_start(&cfg, true);
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    std::vector<wifi_ap_record_t> recs(n); esp_wifi_scan_get_ap_records(&n, recs.data());
    for (int i = 0; i < n; i++) _scan_result.push_back({recs[i].rssi, std::string((char*)recs[i].ssid)});
}

void AppWifiScan::onRunning() {
    auto* c = &GetHAL().canvas();
    c->fillScreen(TFT_BLACK);
    if (_scan_result.empty()) { c->setTextColor(TFT_YELLOW); c->setTextSize(2); c->setCursor(30, 50); c->print("Scanning..."); }
    else render_page_result();
    c->pushSprite(0, 0);
    // Unified exit: ESC or home button
    if (GetHAL().goButton().wasClicked()) { close(); return; }
    auto& kb = GetHAL().keyboard;
    if (kb.getLatestKeyEvent().state && (kb.getLatestKeyEvent().keyCode == KEY_ESC || kb.getLatestKeyEvent().keyCode == KEY_ENTER)) {
        kb.clearKeyEvent(); close(); return;
    }
    if (app_millis() - _time_count > 5000) close();
}

void AppWifiScan::onClose() { mclog::tagInfo(getAppInfo().name, "closed"); }

void AppWifiScan::render_page_result() {
    auto* c = &GetHAL().canvas();
    c->setTextColor(TFT_WHITE); c->setTextSize(1); c->setCursor(5, 5);
    c->printf("Found %d networks:", (int)_scan_result.size());
    int y = 20;
    for (int i = 0; i < (int)_scan_result.size() && i < 8; i++) {
        c->setCursor(5, y);
        int rssi = _scan_result[i].first;
        c->setTextColor(rssi > -50 ? TFT_GREEN : rssi > -70 ? TFT_YELLOW : TFT_RED);
        c->printf("%s (%ddBm)", _scan_result[i].second.c_str(), rssi);
        y += 14;
    }
}

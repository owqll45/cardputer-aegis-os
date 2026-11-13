#include "app_sentinel_test.h"
#include <mooncake_log.h>

AppSentinelTest::AppSentinelTest() {
    auto& info = setAppInfo();
    info.name = "SentinelTest";
}

AppSentinelTest::~AppSentinelTest() {}

void AppSentinelTest::onOpen() {
    // No Sprite needed to save 32KB RAM
    M5.Display.fillScreen(TFT_BLACK);
}

void AppSentinelTest::onClose() {
    M5.Display.fillScreen(TFT_BLACK);
}

void AppSentinelTest::_draw_stat(int x, int y, const char* label, const char* value, uint16_t color) {
    M5.Display.setTextColor(0x7BEF, TFT_BLACK);
    M5.Display.drawString(label, x, y);
    M5.Display.setTextColor(color, TFT_BLACK);
    M5.Display.drawString(value, x + 35, y); // Reduced from 60 to 35 to save horizontal space
}

void AppSentinelTest::onRunning() {
    // Direct drawing to screen
    M5.Display.setFont(&fonts::efontCN_12);
    M5.Display.setTextSize(1);
    GetHAL().reportAppHealth(); // Feed Watchdog
    
    // Header
    M5.Display.fillRect(0, 0, 240, 15, 0x3186);
    M5.Display.setTextColor(TFT_WHITE, 0x3186);
    M5.Display.drawCenterString("哨兵-8D 实验调试站 v4.9c", 120, 2);

    auto status = GetHAL().getLifeGuardStatus();
    
    // Column 1: Core Biometrics (x=5)
    int y = 20;
    _draw_stat(5, y, "心率:", (std::to_string(status.heart_rate) + " BPM").c_str(), status.heart_rate > 100 ? TFT_RED : TFT_GREEN);
    _draw_stat(5, y+15, "压力:", (std::to_string(status.hrv) + " ms").c_str(), status.hrv < 30 ? TFT_ORANGE : TFT_CYAN);
    _draw_stat(5, y+30, "呼吸:", std::to_string(status.respiration_rate).c_str(), TFT_WHITE);
    _draw_stat(5, y+45, "跳变:", std::to_string(status.delta_hr).c_str(), abs(status.delta_hr) > 10 ? TFT_YELLOW : TFT_WHITE);

    // Column 2: Algorithm State (x=120)
    _draw_stat(120, y, "风险:", fmt::format("{:.1f}", status.risk_score).c_str(), status.risk_score > 5 ? TFT_RED : TFT_CYAN);
    _draw_stat(120, y+15, "状态:", status.current_mood_cn, status.risk_score < 3 ? TFT_GREEN : TFT_RED);
    _draw_stat(120, y+30, "体动:", status.motion_detected ? "活动" : "静止", status.motion_detected ? TFT_GREEN : 0x7BEF);
    _draw_stat(120, y+45, "药效:", status.morning_med_status == Hal::MED_COVERED ? "良好" : "待检", 0x7BEF);

    // BLE Status Indicator
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("H613 连接:", 5, 85);
    M5.Display.setTextColor(status.heart_rate > 0 ? TFT_GREEN : 0x7BEF, TFT_BLACK);
    M5.Display.drawString(status.heart_rate > 0 ? "已在线" : "搜索传感器...", 70, 85);

    // Manual Simulation Controls
    M5.Display.drawRect(5, 100, 230, 32, 0x7BEF);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    
    static int selected = 0;
    const char* labels[] = {"无", "1:睡眠瘫痪", "2:深层梦境", "3:情绪压力", "4:噩梦警报"};
    M5.Display.drawString("模拟选择:", 10, 102);
    M5.Display.setTextColor(selected > 0 ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(labels[selected], 70, 102);
    
    M5.Display.setTextColor(0x7BEF, TFT_BLACK);
    M5.Display.drawString("1-4选 | Enter或点(.)触发", 10, 116);

    _handle_input(selected);
}

void AppSentinelTest::_handle_input(int& selected) {
    GetHAL().keyboard.update();
    auto& key = GetHAL().keyboard.getLatestKeyEvent();
    
    if (key.state) {
        if (key.keyCode >= KEY_1 && key.keyCode <= KEY_4) {
            selected = key.keyCode - KEY_1 + 1;
        } else if (key.keyCode == KEY_ENTER || key.keyCode == 13 || key.keyCode == 10 || key.keyCode == KEY_DOT) {
            if (selected == 1) GetHAL().triggerLifeAlert(Hal::STATE_GHOST_PRESS);
            else if (selected == 2) GetHAL().triggerLifeAlert(Hal::STATE_SAD_DREAM);
            else if (selected == 3) GetHAL().triggerLifeAlert(Hal::STATE_DAY_STRESS);
            else if (selected == 4) GetHAL().triggerLifeAlert(Hal::STATE_NIGHTMARE);
            selected = 0; 
        }
        GetHAL().keyboard.clearKeyEvent();
    }
}

#include "app_heart_rate.h"
#include "hal/hal.h"
#include <mooncake_log.h>
#include <cmath>

static const char* TAG = "AppHeartRate";
static AppHeartRate* _app_instance = nullptr;

AppHeartRate::AppHeartRate() {
    auto& info = setAppInfo();
    info.name = "HeartRate";
    info.icon = nullptr;
}

AppHeartRate::~AppHeartRate() {
    if (_app_instance == this) _app_instance = nullptr;
}

void AppHeartRate::onOpen() {
    mclog::tagInfo(TAG, "onOpen - Heart Rate UI");
    _app_instance = this;
    _heart_rate = 0;
    _is_connected = false;
    _history.clear();
    _history.resize(240, 0); 
    _ecg_wave.clear();
    _ecg_wave.resize(240, 15);
    _ecg_phase = 0;
    
    M5.Display.fillScreen(TFT_BLACK);
}

void AppHeartRate::onClose() {
    mclog::tagInfo(TAG, "onClose");
    M5.Display.fillScreen(TFT_BLACK);
    _app_instance = nullptr;
}

void AppHeartRate::onRunning() {
    // Read directly from HAL background BLE service
    auto status = GetHAL().getLifeGuardStatus();
    _heart_rate = status.heart_rate;
    _is_connected = (_heart_rate > 0);
    
    GetHAL().reportAppHealth(); 
    
    M5.Display.startWrite();
    
    if (!_is_connected) {
        // Scanning / Waiting for data
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextDatum(middle_center);
        
        static int scan_dots = 0;
        scan_dots = (GetHAL().millis() / 500) % 4;
        
        M5.Display.setFont(&fonts::efontCN_12);
        M5.Display.setTextColor(0x07FF, TFT_BLACK); 
        
        char scan_msg[64];
        snprintf(scan_msg, sizeof(scan_msg), "搜索并连接心率带中");
        for (int i = 0; i < scan_dots; i++) strcat(scan_msg, ".");
        M5.Display.drawString(scan_msg, 120, 55);
        
        M5.Display.setTextColor(0x7BEF, TFT_BLACK); 
        M5.Display.setFont(&fonts::Font0);
        M5.Display.drawString("BLE Heart Rate Service (后台运行中)", 120, 80);
        
        // Spinning ring animation
        int cx = 120, cy = 30;
        int angle = (GetHAL().millis() / 80) % 12;
        for(int i = 0; i < 12; i++) {
            float a = i * 30 * 0.0174533f;
            uint16_t c = (i == angle) ? 0x07FF : 0x2104;
            M5.Display.fillCircle(cx + cos(a)*15, cy + sin(a)*15, 2, c);
        }
        
    } else {
        // Connected - show heart rate data
        M5.Display.fillRect(0, 0, 240, 80, TFT_BLACK);
        
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::Font7);
        M5.Display.setTextColor(_heart_rate > 140 ? TFT_RED : (_heart_rate > 100 ? TFT_YELLOW : TFT_GREEN));
        M5.Display.drawNumber(_heart_rate, 100, 40);
        
        M5.Display.setFont(&fonts::efontCN_12);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("BPM (后台同步)", 100, 70);
        
        M5.Display.setTextDatum(top_left);
        M5.Display.setFont(&fonts::Font0);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        char stats[64];
        snprintf(stats, sizeof(stats), "HRV:%dms RR:%d Risk:%.1f", 
                 status.hrv, status.respiration_rate, status.risk_score);
        M5.Display.drawString(stats, 5, 80);

        static float heart_scale = 1.0;
        static uint32_t last_beat = 0;
        uint32_t interval = 60000 / (_heart_rate > 0 ? _heart_rate : 60);
        if (GetHAL().millis() - last_beat > interval) {
            heart_scale = 1.4;
            last_beat = GetHAL().millis();
        }
        heart_scale = heart_scale * 0.85 + 1.0 * 0.15;
        _draw_heart(&M5.Display, 200, 40, (int)(15 * heart_scale), TFT_RED);

        M5.Display.fillRect(0, 90, 240, 45, 0x0001);
        _draw_ecg(&M5.Display, 0, 105, 240, 30, 0x07FF);
    }

    M5.Display.endWrite();
    
    GetHAL().keyboard.update();
    auto& key = GetHAL().keyboard.getLatestKeyEvent();
    if ((key.state && (key.keyCode == KEY_ESC || key.keyCode == KEY_ENTER || key.keyCode == 27)) || GetHAL().goButton().wasPressed()) {
        close();
        GetHAL().keyboard.clearKeyEvent();
    }
}

void AppHeartRate::_draw_heart(M5GFX* target, int x, int y, int size, uint16_t color) {
    int r = size / 2;
    target->fillCircle(x - r, y - r, r, color);
    target->fillCircle(x + r, y - r, r, color);
    target->fillTriangle(x - size, y - r + 3, x + size, y - r + 3, x, y + size, color);
}

void AppHeartRate::_draw_ecg(M5GFX* target, int x, int y, int w, int h, uint16_t color) {
    for(int i = 0; i < w-1; i++) _ecg_wave[i] = _ecg_wave[i+1];
    
    float val = h/2;
    if (_heart_rate > 0) {
        float freq = _heart_rate / 60.0f;
        _ecg_phase += 0.15f * freq;
        if (_ecg_phase > 2*M_PI) _ecg_phase -= 2*M_PI;
        
        if (_ecg_phase < 0.5) val -= sin(_ecg_phase * M_PI / 0.5) * 3;
        else if (_ecg_phase < 0.7) val = h/2;
        else if (_ecg_phase < 0.8) val += (_ecg_phase - 0.7) * 60;
        else if (_ecg_phase < 0.9) val -= (_ecg_phase - 0.8) * 180;
        else if (_ecg_phase < 1.0) val += (_ecg_phase - 0.9) * 120;
        else if (_ecg_phase < 1.5) val -= sin((_ecg_phase-1.0) * M_PI / 0.5) * 5;
    }
    _ecg_wave[w-1] = (int)val;
    
    for(int i = 0; i < w-1; i++) {
        target->drawLine(x+i, y+_ecg_wave[i], x+i+1, y+_ecg_wave[i+1], color);
    }
}

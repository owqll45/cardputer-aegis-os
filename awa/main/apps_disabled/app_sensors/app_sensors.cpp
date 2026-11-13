#include "app_sensors.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

AppSensors::AppSensors() { setAppInfo().name = "Sensors"; }
AppSensors::~AppSensors() {}

void AppSensors::onCreate() {
    mclog::tagInfo(getAppInfo().name, "created");
    _currentSensor = TEMPERATURE; _dht11 = nullptr; _ultrasonic = nullptr;
    _isActive = true; _lastDhtUpdateTime = 0; _lastDistUpdateTime = 0;
    _sensor_mutex = portMUX_INITIALIZER_UNLOCKED; _handle_key_event_slot_id = -1;
}

void AppSensors::onRunning() {
    auto* c = &GetHAL().canvas();
    c->fillScreen(TFT_BLACK);
    updateSensors(); drawHeader(); drawSensorData();
    c->pushSprite(0, 0);
}

void AppSensors::onClose() {
    mclog::tagInfo(getAppInfo().name, "closed");
    _isActive = false;
    if (_dht11) { delete _dht11; _dht11 = nullptr; }
    if (_ultrasonic) { delete _ultrasonic; _ultrasonic = nullptr; }
}

void AppSensors::drawHeader() {
    auto* c = &GetHAL().canvas();
    c->setTextColor(TFT_CYAN); c->setTextSize(1); c->setCursor(5, 5);
    const char* names[] = {"Temp", "Humid", "Dist"};
    c->printf("[%s]", names[_currentSensor]);
}

void AppSensors::drawSensorData() {
    auto* c = &GetHAL().canvas();
    c->setTextColor(TFT_WHITE); c->setTextSize(2); int y = 35;
    switch (_currentSensor) {
        case TEMPERATURE: c->setCursor(10, y); c->printf("%.1f C", _dataCache.temperature); break;
        case HUMIDITY: c->setCursor(10, y); c->printf("%.1f %%", _dataCache.humidity); break;
        case DISTANCE: c->setCursor(10, y); c->printf("%.1f cm", _dataCache.distance); break;
    }
}

void AppSensors::updateSensors() {
    uint32_t now = esp_timer_get_time() / 1000;
    if (now - _lastDhtUpdateTime > 2000) {
        if (!_dht11) _dht11 = new DHT11(2);
        if (_dht11) {
            float t = 0, h = 0;
            if (_dht11->readTempAndHumi(&t, &h)) {
                _dataCache.temperature = t; _dataCache.humidity = h;
            }
        }
        _lastDhtUpdateTime = now;
    }
    if (now - _lastDistUpdateTime > 500) {
        if (!_ultrasonic) _ultrasonic = new Ultrasonic(6, 5);
        if (_ultrasonic) _dataCache.distance = _ultrasonic->readDistance();
        _lastDistUpdateTime = now;
    }
    // Unified exit: ESC
    auto& kb = GetHAL().keyboard;
    if (kb.getLatestKeyEvent().state && (kb.getLatestKeyEvent().keyCode == KEY_ESC || kb.getLatestKeyEvent().keyCode == KEY_ENTER)) {
        kb.clearKeyEvent(); close(); return;
    }
    // Home button exits (sensor switching moved to S key)
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }
    if (kb.getLatestKeyEvent().state && kb.getLatestKeyEvent().keyCode == KEY_S) {
        kb.clearKeyEvent();
        _currentSensor = (SensorType)((_currentSensor + 1) % 3);
    }
}

void AppSensors::drawChart(int x, int y, int w, int h, const std::vector<float>& data, uint16_t color, const char* label) {
    auto* c = &GetHAL().canvas();
    c->drawRoundRect(x, y, w, h, 3, TFT_DARKGREY);
    if (data.empty()) return;
    float mn = data[0], mx = data[0];
    for (float v : data) { if (v < mn) mn = v; if (v > mx) mx = v; }
    float range = mx - mn; if (range < 0.1) range = 0.1;
    int pts = std::min((int)data.size(), w - 2); int start = data.size() - pts;
    for (int i = 0; i < pts - 1; i++) {
        int x1 = x + 1 + i, x2 = x + 2 + i;
        int y1 = y + h - 2 - (int)((data[start + i] - mn) / range * (h - 4));
        int y2 = y + h - 2 - (int)((data[start + i + 1] - mn) / range * (h - 4));
        c->drawLine(x1, y1, x2, y2, color);
    }
    c->setTextColor(TFT_DARKGREY); c->setTextSize(1); c->setCursor(x + 3, y + 2); c->print(label);
}

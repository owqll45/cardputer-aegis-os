#pragma once

#include <mooncake.h>
#include <hal/hal.h>
#include <smooth_ui_toolkit.h>
#include <vector>
#include <string>
#include "dht11.h"
#include "ultrasonic.h"

using namespace mooncake;
using namespace smooth_ui_toolkit;

class AppSensors : public mooncake::AppAbility {
public:
    AppSensors();
    ~AppSensors();
    
private:
    enum SensorType {
        TEMPERATURE = 0,
        HUMIDITY = 1,
        DISTANCE = 2
    };
    
    // [修复]：优化 UI 布局常量以适应 135px 高度
    struct UIConstants {
        static constexpr int SCREEN_WIDTH = 240;
        static constexpr int SCREEN_HEIGHT = 135;
        
        static constexpr int HEADER_HEIGHT = 28; // 缩小头部
        static constexpr int FOOTER_HEIGHT = 18; // 缩小底部
        
        // 剩余空间: 135 - 28 - 18 = 89px
        // 留给数值显示 25px，剩余给图表
        static constexpr int CHART_HEIGHT = 55;  // 调整图表高度
    };
    
    struct SensorData {
        float temperature = 0.0f;
        float humidity = 0.0f;
        float distance = 0.0f;
        bool dhtValid = false;
    };

    SensorType _currentSensor;
    DHT11* _dht11;
    Ultrasonic* _ultrasonic;
    SensorData _dataCache;
    
    void drawHeader();
    void drawSensorData(); 
    void updateSensors();  
    void drawChart(int x, int y, int w, int h, const std::vector<float>& data, uint16_t color, const char* label);
    
    bool _isActive;
    uint32_t _lastDhtUpdateTime;
    uint32_t _lastDistUpdateTime;
    
    std::vector<float> _historyTemp;
    std::vector<float> _historyHumidity;
    std::vector<float> _historyDistance;
    
    int32_t _handle_key_event_slot_id;
    
    // [CRITICAL FIX] Mutex to prevent system crashes during sensor reads
    // Used to disable interrupts during DHT11 read operations
    portMUX_TYPE _sensor_mutex;

    
public:
    void onCreate() override;
    void onRunning() override;
    void onClose() override;
};

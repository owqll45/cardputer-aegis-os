#pragma once
#include <mooncake.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

// 配置: 侧边 Grove 接口引脚 (G1 = GPIO1, G2 = GPIO2)
// Cardputer Grove G1 = GPIO1 (ADC1_CH0)
// Cardputer Grove G2 = GPIO2 (ADC1_CH1)
#define MULTIMETER_PIN_V    1   // 电压测量 (G1)
#define MULTIMETER_PIN_OHM  2   // 电阻/通断测量 (G2) - 需外部上拉电阻

// 默认分压参数 (假设用户使用简单的 10k:2.5k 分压以测 0-15V)
// 如果直连，请将 R1 设为 0
#define VOLT_R1 0.0f
#define VOLT_R2 10000.0f 

class AppMultimeter : public mooncake::AppAbility {
public:
    AppMultimeter();
    ~AppMultimeter();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // 0=Voltage, 1=Continuity/Ohm
    int _mode = 0; 
    
    // ADC
    adc_oneshot_unit_handle_t _adc_handle = nullptr;
    adc_cali_handle_t _cali_handle = nullptr;
    adc_channel_t _adc_chan_v;
    adc_channel_t _adc_chan_ohm;
    bool _do_calibration = false;

    // Data
    float _val_voltage = 0.0f;
    float _val_ohm = 0.0f;
    bool _is_connected = false; // For continuity
    
    // History for Graph
    float _graph_buffer[240];
    int _graph_head = 0;

    void _init_adc();
    void _deinit_adc();
    float _read_voltage_mv(adc_channel_t chan);
    
    void _draw_ui();
    void _draw_graph_card(int x, int y, int w, int h);
    void _draw_value_card(int x, int y, int w, int h);
};
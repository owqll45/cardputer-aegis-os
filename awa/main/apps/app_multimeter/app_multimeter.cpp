#include "app_multimeter.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <M5GFX.h>
#include <M5Unified.hpp>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "../utils/ui_kit.h"

AppMultimeter::AppMultimeter() {
    setAppInfo().name = "Multimeter";
}

AppMultimeter::~AppMultimeter() {}

void AppMultimeter::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Multimeter opened");
    memset(_graph_buffer, 0, sizeof(_graph_buffer));
    _graph_head = 0;
    _init_adc();
}

void AppMultimeter::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);
    ui_title(cv, "电压表  G1");

    _val_voltage = _read_voltage_mv(_adc_chan_v);
    if (VOLT_R1 > 0) {
        _val_voltage = _val_voltage * (VOLT_R1 + VOLT_R2) / VOLT_R2;
    }

    _graph_buffer[_graph_head] = _val_voltage;
    _graph_head = (_graph_head + 1) % 240;

    _draw_ui();

    if (_adc_handle == nullptr) {
        ui_hint(cv, "ADC 不可用  G0 返回");
    } else {
        ui_hint(cv, "实时采样 10Hz  G0 返回");
    }
    GetHAL().pushCanvas();

    vTaskDelay(pdMS_TO_TICKS(100));
}

void AppMultimeter::onClose() {
    mclog::tagInfo(getAppInfo().name, "Multimeter closed");
    _deinit_adc();
}

void AppMultimeter::_init_adc() {
    // ADC1 GPIO1 (Grove G1)
    _adc_chan_v = ADC_CHANNEL_0;

    // 板上电池采样已由 M5.Power 占用了 ADC1 单元, 再 new_unit 必然失败并刷错误日志。
    // 直接复用 Power 的共享单元, 只给它加一个通道配置。
    _adc_handle = M5.Power.getAdcOneShotHandle();
    if (_adc_handle == nullptr) {
        ESP_LOGE(getAppInfo().name.c_str(), "ADC1 unavailable");
        return;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(_adc_handle, _adc_chan_v, &chan_config) != ESP_OK) {
        _adc_handle = nullptr;
        return;
    }

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &_cali_handle) != ESP_OK) {
        _cali_handle = nullptr;
    }
}

void AppMultimeter::_deinit_adc() {
    // 单元归 M5.Power 所有, 不能 del, 只释放本 app 的校准句柄
    _adc_handle = nullptr;
    if (_cali_handle) {
        adc_cali_delete_scheme_curve_fitting(_cali_handle);
        _cali_handle = nullptr;
    }
}

float AppMultimeter::_read_voltage_mv(adc_channel_t chan) {
    int raw = 0;
    if (_adc_handle == nullptr || adc_oneshot_read(_adc_handle, chan, &raw) != ESP_OK) {
        return 0;
    }
    int voltage_mv = 0;
    if (_cali_handle) {
        adc_cali_raw_to_voltage(_cali_handle, raw, &voltage_mv);
    } else {
        voltage_mv = raw * 3300 / 4095;
    }
    return (float)voltage_mv;
}

void AppMultimeter::_draw_ui() {
    auto& cv = GetHAL().canvas();

    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", _val_voltage / 1000.0f);
    ui_big_value(cv, 120, 24, buf, "V", UI_ACCENT);

    _draw_graph_card(6, 73, 228, 24);
}

void AppMultimeter::_draw_value_card(int x, int y, int w, int h) {
    ui_card(GetHAL().canvas(), x, y, w, h);
}

void AppMultimeter::_draw_graph_card(int x, int y, int w, int h) {
    auto& cv = GetHAL().canvas();
    cv.fillRoundRect(x, y, w, h, 5, UI_CARD);
    cv.drawRoundRect(x, y, w, h, 5, UI_LINE);

    float min_val = 3300, max_val = 0;
    for (int i = 0; i < 240; i++) {
        if (_graph_buffer[i] < min_val) min_val = _graph_buffer[i];
        if (_graph_buffer[i] > max_val) max_val = _graph_buffer[i];
    }
    float range = max_val - min_val;
    if (range < 20) range = 20;

    for (int i = 0; i < w - 4; i++) {
        int idx = (_graph_head + i) % 240;
        int py = y + h - 3 - (int)((_graph_buffer[idx] - min_val) / range * (h - 6));
        if (py < y + 1) py = y + 1;
        cv.drawPixel(x + 2 + i, py, UI_OK);
    }
}

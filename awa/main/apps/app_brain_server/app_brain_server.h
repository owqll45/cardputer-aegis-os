#pragma once
#include <mooncake.h>
#include "../../hal/hal.h"
#include "../../hal/swap_memory.h"
#include <string>
#include <vector>
#include "cJSON.h"

class AppBrainServer : public mooncake::AppAbility {
public:
    AppBrainServer();
    ~AppBrainServer();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // Virtual Memory for logs to save RAM
    SwapVector<SwapString> _thinking_logs;
    
    // Connectivity State
    enum ConnectMode {
        MODE_SELECT = 0,
        MODE_BLE_DIRECT,
        MODE_WIFI_HOTSPOT,
        MODE_WIFI_STATION
    };
    ConnectMode _current_mode = MODE_SELECT;
    
    // UI State
    int _menu_index = 0;
    bool _is_scanning = true;
    uint32_t _last_scan_ts = 0;
    
    float _current_risk = 0.0f;
    int _current_hr = 0;
    std::string _status_text = "等待连接...";
    
    void _update_ui();
    void _render_mode_selection();
    void _handle_mode_selection();
    void _add_log(const std::string& text, uint16_t color = TFT_WHITE);
    void _handle_client_message(int fd, cJSON* root);
    void _init_mode(ConnectMode mode);
};

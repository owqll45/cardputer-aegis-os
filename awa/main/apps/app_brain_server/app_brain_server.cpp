#include "app_brain_server.h"
#include "../../hal/brain_server.h"
#include "../../hal/brain_coordinator.h"
#include "../../hal/brain_logger.h"
#include "../../crash_handler.h"
#include <mooncake_log.h>

using namespace brain_server;
using namespace brain_coordinator;
using namespace brain_logger;

AppBrainServer::AppBrainServer() : _thinking_logs("brain_logs") {
    setAppInfo().name = "BrainServer";
}

AppBrainServer::~AppBrainServer() {}

void AppBrainServer::onOpen() {
    mclog::tagInfo("BrainServer", "onOpen - Manual Mode");
    crash_handler::set_running_app("BrainServer");
    _thinking_logs.clear();
    
    // Default to Select Mode
    _current_mode = MODE_SELECT;
    _menu_index = 0;
    _is_scanning = true;
    _last_scan_ts = GetHAL().millis();
    
    M5.Display.fillScreen(TFT_BLACK);
}

void AppBrainServer::onRunning() {
    GetHAL().reportAppHealth();

    // Limit UI update to ~10 FPS
    static uint32_t last_ui_update = 0;
    if (GetHAL().millis() - last_ui_update > 100) {
        last_ui_update = GetHAL().millis();
        
        if (_current_mode == MODE_SELECT) {
            _render_mode_selection();
            _handle_mode_selection();
        } else {
            _update_ui();
        }
    }
    
    // Auto-Jump: If in Select Mode but BLE data appears -> Jump to BLE Mode
    if (_current_mode == MODE_SELECT) {
        if (GetHAL().getLifeGuardStatus().heart_rate > 0) {
            mclog::tagInfo("BrainServer", "Auto-Jump: Heart Rate detected, switching to BLE Mode!");
            _init_mode(MODE_BLE_DIRECT);
        }
    }
    
    if (GetHAL().goButton().wasClicked()) {
        close();
    }
}

void AppBrainServer::_handle_mode_selection() {
    auto key = GetHAL().keyboard.getLatestKeyEvent();
    if (!key.state) return; // Only on press

    if (strcmp(key.keyName, "up") == 0 || key.keyCode == KEY_UP) {
        _menu_index--;
        if (_menu_index < 0) _menu_index = 2;
    } else if (strcmp(key.keyName, "down") == 0 || key.keyCode == KEY_DOWN) {
        _menu_index++;
        if (_menu_index > 2) _menu_index = 0;
    } else if (key.keyCode == KEY_ENTER || key.keyCode == 13 || key.keyCode == 10 || key.keyCode == 'g' || strcmp(key.keyName, "enter") == 0) {
        // Confirm Selection
        switch (_menu_index) {
            case 0: _init_mode(MODE_BLE_DIRECT); break;
            case 1: _init_mode(MODE_WIFI_HOTSPOT); break;
            case 2: _init_mode(MODE_WIFI_STATION); break;
        }
    }
}

void AppBrainServer::_render_mode_selection() {
    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_BLACK);
    
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawCenterString("连接模式选择", 120, 10);
    
    const char* options[] = {
        "1. 蓝牙直连 (仅心率)",
        "2. Wi-Fi 热点 (服务器)",
        "3. 连接路由 (混合模式)"
    };
    
    for (int i = 0; i < 3; i++) {
        if (i == _menu_index) {
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            M5.Display.fillRect(10, 40 + i * 25, 220, 20, TFT_WHITE);
        } else {
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        M5.Display.drawString(options[i], 20, 42 + i * 25);
    }
    
    // Scan Status
    M5.Display.setTextColor(TFT_DARKGRAY, TFT_BLACK);
    M5.Display.drawCenterString("正在扫描蓝牙设备...", 120, 115);
    
    M5.Display.endWrite();
}

void AppBrainServer::_init_mode(ConnectMode mode) {
    _current_mode = mode;
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    
    switch (mode) {
        case MODE_BLE_DIRECT:
            M5.Display.drawCenterString("正在连接蓝牙...", 120, 60);
            // BLE is already running in background via HAL
            break;
            
        case MODE_WIFI_HOTSPOT:
            M5.Display.drawCenterString("启动 Wi-Fi 热点...", 120, 60);
            GetHAL().purgeMemory(true); // Kill BLE/BG apps
            GetHAL().wifiApInit("Sentinel_Brain", "sentinel-link");
            BrainCoreServer::getInstance().start();
            break;
            
        case MODE_WIFI_STATION:
            M5.Display.drawCenterString("连接 Wi-Fi...", 120, 60);
            // Assume WiFi set by SetWiFi app
            if (!GetHAL().isWifiConnected()) {
                M5.Display.setTextColor(TFT_RED, TFT_BLACK);
                M5.Display.drawCenterString("未配置 Wi-Fi!", 120, 80);
                GetHAL().delay(2000);
                _current_mode = MODE_SELECT; // Go back
                return;
            }
            BrainCoreServer::getInstance().start();
            break;
            
        default: break;
    }
    
    // Init Logger
    BrainLogger::getInstance().init();
    
    // Common Init
    BrainCoreServer::getInstance().setMessageHandler([this](int fd, cJSON* root) {
        _handle_client_message(fd, root);
    });
}

void AppBrainServer::onClose() {
    mclog::tagInfo("BrainServer", "onClose - Resource Deactivation");
    
    // Disable Resources
    BrainCoreServer::getInstance().stop();
    BrainLogger::getInstance().deinit();
    GetHAL().wifiDeinit();
    
    // Restore System state
    crash_handler::set_running_app("Launcher");
    M5.Display.fillScreen(TFT_BLACK);
}

void AppBrainServer::_handle_client_message(int fd, cJSON* root) {
    // 1. Log Raw Data
    char* raw_json = cJSON_PrintUnformatted(root);
    if (raw_json) {
        BrainLogger::getInstance().logFrame(raw_json);
        free(raw_json);
    }

    // 2. Process via Coordinator
    auto verdict = BrainCoordinator::getInstance().processData(root);
    
    // 3. Update State
    _current_risk = verdict.risk_score;
    _current_hr = cJSON_GetObjectItem(root, "hr") ? cJSON_GetObjectItem(root, "hr")->valueint : 0;
    _status_text = verdict.summary;
    
    for (const auto& log : verdict.thinking) {
        _add_log(log, 0x7BEF);
    }

    // 4. Send Response back to Android
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "type", "VERDICT");
    cJSON_AddNumberToObject(response, "risk_score", verdict.risk_score);
    cJSON_AddStringToObject(response, "summary", verdict.summary.c_str());
    
    BrainCoreServer::getInstance().sendTo(fd, std::string(cJSON_PrintUnformatted(response)));
    cJSON_Delete(response);
}

void AppBrainServer::_add_log(const std::string& text, uint16_t color) {
    _thinking_logs.push_back(SwapString(text, color));
    // No need to erase, let it grow on SD card (History)
}

void AppBrainServer::_update_ui() {
    // Check Disconnection (Only for BLE Mode)
    if (_current_mode == MODE_BLE_DIRECT) {
        // If HR drops to 0, it means disconnected or not wearing
        if (GetHAL().getLifeGuardStatus().heart_rate == 0) {
            _status_text = "设备断连 / 未佩戴";
            M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        } else {
            _status_text = "蓝牙直连: 数据接收中";
        }
        _current_hr = GetHAL().getLifeGuardStatus().heart_rate;
        _current_risk = GetHAL().getLifeGuardStatus().risk_score;
    } else {
        // WiFi Mode logic (existing)
        auto clients = BrainCoreServer::getInstance().getConnectedClients();
        if (clients.empty()) {
            // Local fallback
            auto local_status = GetHAL().getLifeGuardStatus();
            _current_hr = local_status.heart_rate;
            _current_risk = local_status.risk_score;
            _status_text = "本地模式 (无连接): " + std::string(local_status.current_mood_cn);
        } else {
            // Use Client Data (handled by BrainCoreServer callback)
            _status_text = "远程连接: " + std::to_string(clients.size()) + " 设备";
        }
    }

    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_BLACK);
    
    // Header
    M5.Display.fillRect(0, 0, 240, 30, 0x18E3); 
    M5.Display.setTextColor(TFT_WHITE, 0x18E3);
    M5.Display.drawCenterString("Sentinel Brain Core", 120, 7);
    
    // Clients
    auto clients = BrainCoreServer::getInstance().getConnectedClients();
    M5.Display.setCursor(5, 20);
    if (!GetHAL().isWifiConnected()) {
        M5.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
        M5.Display.print("网络未就绪...");
    } else {
        M5.Display.setTextColor(0x7BEF, TFT_BLACK);
        if (clients.empty()) {
            M5.Display.setTextColor(TFT_MAGENTA, TFT_BLACK);
            M5.Display.printf("合体版 (本地监控)");
        } else {
            M5.Display.printf("在线终端: %d", (int)clients.size());
        }
        
        // Show IP or AP info
        M5.Display.setCursor(120, 20);
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.printf("热点: Sentinel_Brain");
    }

    // Core Metrics
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(5, 35);
    M5.Display.printf("当前心率: ");
    M5.Display.setTextColor(_current_hr > 100 ? TFT_RED : TFT_GREEN, TFT_BLACK);
    M5.Display.printf("%d BPM", _current_hr);
    
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(120, 35);
    M5.Display.printf("风险评分: ");
    M5.Display.setTextColor(_current_risk > 7.5 ? TFT_RED : TFT_CYAN, TFT_BLACK);
    M5.Display.printf("%.1f", _current_risk);

    // Status Summary
    M5.Display.drawRect(5, 50, 230, 25, 0x7BEF);
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.setCursor(10, 55);
    std::string display_text = _status_text;
    if (display_text.length() > 40) display_text = display_text.substr(0, 37) + "...";
    M5.Display.print(display_text.c_str());

    // Thinking Logs
    M5.Display.setTextColor(0x7BEF, TFT_BLACK);
    M5.Display.setCursor(5, 80);
    M5.Display.print("专家推演日志 (SD虚拟内存):");
    int y = 90;
    
    size_t count = _thinking_logs.size();
    size_t start = (count > 5) ? count - 5 : 0;

    for (size_t i = start; i < count; i++) {
        auto log = _thinking_logs.get(i);
        M5.Display.setTextColor(log.color, TFT_BLACK);
        M5.Display.setCursor(10, y);
        M5.Display.print(log.data);
        y += 10;
    }

    // Memory/Feature Warning
    if (!GetHAL().isWifiConnected() && _current_hr == 0) {
        M5.Display.setFont(&fonts::efontCN_10_b);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawCenterString("内存不足: 心率已关闭 (请尝试连接Wi-Fi)", 120, 128);
    }

    M5.Display.endWrite();
}

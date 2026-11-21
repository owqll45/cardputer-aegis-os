#include "network_manager.h"
#include "../hal_config.h"
#include "utils/ir_nec/ir_helper.h"
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_event.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <vector>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>

static const std::string _tag = "Network";

/* WiFi Scan Constants */
#define DEFAULT_SCAN_LIST_SIZE 6
static wifi_ap_record_t _ap_info[DEFAULT_SCAN_LIST_SIZE];

/* WiFi Event Group */
static EventGroupHandle_t s_wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT          = BIT0;
static const int WIFI_DISCONNECTED_BIT       = BIT1;
static const int WIFI_FAIL_BIT               = BIT2;
static const int WIFI_STARTED_BIT            = BIT3;

/* BLE Globals (shared with LifeGuardManager) */
extern TaskHandle_t s_ble_task_handle;
extern bool _hr_service_inited;

/* WiFi Event Handler */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    const char* TAG = "wifi";

    /* Wifi started */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STARTED_BIT);
    }

    /* Disconnected */
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }

    /* Connected */
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

NetworkManager::NetworkManager()
{
}

NetworkManager::~NetworkManager()
{
}

void NetworkManager::init()
{
    mclog::tagInfo(_tag, "network init");
}

void NetworkManager::update()
{
}

/* -------------------------------------------------------------------------- */
/*                                    WiFI                                    */
/* -------------------------------------------------------------------------- */
void NetworkManager::wifiScan(std::vector<ScanResult_t>& scanResult)
{
    mclog::tagInfo(_tag, "wifi scan");

    scanResult.clear();

    if (!_is_wifi_inited) {
        mclog::tagError(_tag, "wifi not inited, skip scan");
        return;
    }

    uint16_t number   = DEFAULT_SCAN_LIST_SIZE;
    uint16_t ap_count = 0;
    memset(_ap_info, 0, sizeof(_ap_info));

    /* Start WiFi scan */
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to start wifi scan: {}", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to get AP number: {}", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_scan_get_ap_records(&number, _ap_info);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to get AP records: {}", esp_err_to_name(ret));
        return;
    }

    /* Process scan results */
    for (int i = 0; i < number; i++) {
        std::string ssid = (char*)_ap_info[i].ssid;
        int rssi         = _ap_info[i].rssi;

        /* Skip empty SSID */
        if (ssid.empty()) {
            continue;
        }

        /* Add to ap_list */
        scanResult.push_back(std::make_pair(rssi, ssid));
    }

    /* Sort ap_list by RSSI (from strongest to weakest) */
    std::sort(scanResult.begin(), scanResult.end(),
              [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) {
                  return a.first > b.first;  /* Higher RSSI first */
              });

    mclog::tagInfo(_tag, "wifi scan completed, found {} APs", scanResult.size());
}

void NetworkManager::wifiInit()
{
    mclog::tagInfo(_tag, "wifi init");

    if (_is_wifi_inited) {
        mclog::tagInfo(_tag, "wifi already inited");
        return;
    }

    // If WiFi init failed before (out of memory), don't retry:
    // esp_netif_create_default_wifi_sta() would abort internally (ESP_ERROR_CHECK)
    static bool _wifi_init_failed = false;
    if (_wifi_init_failed) {
        mclog::tagWarn(_tag, "wifi init previously failed, skip retry");
        return;
    }

    /* --- Networking Core Init (Lazy Load) --- */
    static bool _net_core_inited = false;
    if (!_net_core_inited) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            mclog::tagWarn(_tag, "NVS issue in wifiInit, erasing...");
            nvs_flash_erase();
            nvs_flash_init();
        }
        esp_netif_init();
        esp_event_loop_create_default();  // may already exist, ignore error
        _net_core_inited = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Balanced buffers: enough for AP beacon (mgmt=8), reduced data buffers for RAM
    cfg.static_rx_buf_num  = 2;
    cfg.dynamic_rx_buf_num = 8;
    cfg.static_tx_buf_num  = 2;
    cfg.dynamic_tx_buf_num = 8;
    cfg.cache_tx_buf_num   = 2;
    cfg.rx_mgmt_buf_num    = 8;
    cfg.ampdu_rx_enable    = 0;
    cfg.ampdu_tx_enable    = 0;

    // Graceful degradation: don't abort on WiFi init failure (memory low)
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE("Network", "wifi init failed: 0x%x (out of memory), skip WiFi", err);
        _wifi_init_failed = true;
        return;
    }

    // Only create default netif AFTER esp_wifi_init succeeds
    // (esp_netif_create_default_wifi_sta aborts internally if wifi not inited)
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }

    esp_wifi_set_mode(WIFI_MODE_STA);

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE("Network", "wifi start failed: 0x%x, skip WiFi", err);
        _wifi_init_failed = true;
        return;
    }
    // 关闭省电模式，否则音频流式播放会周期性停顿（听起来一顿一顿）
    esp_wifi_set_ps(WIFI_PS_NONE);
    _is_wifi_inited = true;
}

void NetworkManager::wifiApInit(const std::string& ssid, const std::string& password)
{
    mclog::tagInfo(_tag, "wifi ap init: ssid: {}", ssid);

    if (_is_wifi_inited) {
        wifiDeinit();
    }

    /* Radio Coexistence (RAM Scavenging) */
#if CONFIG_BT_ENABLED
    if (s_ble_task_handle != NULL || _hr_service_inited) {
        mclog::tagWarn(_tag, "Stopping BLE to reclaim RAM for Wi-Fi AP...");

        /* TODO: Move BLE cleanup to LifeGuardManager */
        // ble_gap_disc_cancel();

        // int rc = nimble_port_stop();
        // if (rc == 0) {
        //     nimble_port_deinit();
        // }

        // if (s_ble_task_handle) {
        //     vTaskDelete(s_ble_task_handle);
        //     s_ble_task_handle = NULL;
        // }

        // _hr_service_inited = false;

        vTaskDelay(200 / portTICK_PERIOD_MS);
        mclog::tagInfo(_tag, "BLE Stopped. Free Heap: {} KB", (int)(esp_get_free_heap_size() / 1024));
    }
#endif

    /* NVS, Netif, and Event Loop are now initialized via Lazy Load */
    static bool _net_core_inited2 = false;
    if (!_net_core_inited2) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            mclog::tagWarn(_tag, "NVS issue in wifiApInit, erasing...");
            nvs_flash_erase();
            nvs_flash_init();
        }
        ESP_ERROR_CHECK(esp_netif_init());
        if (esp_event_loop_create_default() != ESP_OK) {}
        _net_core_inited2 = true;
    }

    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num  = 2;
    cfg.dynamic_rx_buf_num = 4;
    cfg.static_tx_buf_num  = 2;
    cfg.dynamic_tx_buf_num = 4;
    cfg.cache_tx_buf_num   = 4;
    cfg.rx_mgmt_buf_num    = 4;
    cfg.ampdu_rx_enable    = 0;
    cfg.ampdu_tx_enable    = 0;

    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_ERR_NO_MEM) {
        mclog::tagWarn(_tag, "OOM: Wi-Fi init failed. Sacrificing BLE...");

        /* TODO: Move BLE cleanup to LifeGuardManager */
        // if (s_ble_task_handle != NULL) {
        //     ble_gap_disc_cancel();
        //     nimble_port_stop();
        //     vTaskDelay(200 / portTICK_PERIOD_MS);
        //     if (s_ble_task_handle) vTaskDelete(s_ble_task_handle);
        //     s_ble_task_handle = NULL;
        //     nimble_port_deinit();
        //     _hr_service_inited = false;

            err = esp_wifi_init(&cfg);
        // }
    }

    if (err != ESP_OK) {
        mclog::tagError(_tag, "Wi-Fi Init Fatal Error: {}", (int)err);
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawCenterString("Wi-Fi Error: OOM", 120, 60);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        return;
    }

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ssid.c_str(), sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = ssid.length();
    strncpy((char*)wifi_config.ap.password, password.c_str(), sizeof(wifi_config.ap.password));
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode       = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    mclog::tagInfo(_tag, "wifi ap started. SSID: {}, PWD: {}", ssid, password);
    _is_wifi_inited    = true;
    _is_wifi_connected = true;
    _is_ap_active      = true;
}

void NetworkManager::wifiApStaInit(const std::string& apSsid, const std::string& apPassword,
                                    const std::string& staSsid, const std::string& staPassword)
{
    mclog::tagInfo(_tag, "wifi apsta init: AP={}, STA={}", apSsid, staSsid);

    if (_is_wifi_inited) {
        wifiDeinit();
    }

    static bool _net_core_inited = false;
    if (!_net_core_inited) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            mclog::tagWarn(_tag, "NVS issue in wifiApStaInit, erasing...");
            nvs_flash_erase();
            nvs_flash_init();
        }
        ESP_ERROR_CHECK(esp_netif_init());
        if (esp_event_loop_create_default() != ESP_OK) {}
        _net_core_inited = true;
    }

    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        esp_netif_create_default_wifi_ap();
    }
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num  = 2;
    cfg.dynamic_rx_buf_num = 4;
    cfg.static_tx_buf_num  = 2;
    cfg.dynamic_tx_buf_num = 4;
    cfg.cache_tx_buf_num   = 4;
    cfg.rx_mgmt_buf_num    = 4;
    cfg.ampdu_rx_enable    = 0;
    cfg.ampdu_tx_enable    = 0;

    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "Wi-Fi APSTA Init Fatal Error: {}", (int)err);
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawCenterString("Wi-Fi Error: OOM", 120, 60);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        return;
    }

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, apSsid.c_str(), sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = apSsid.length();
    strncpy((char*)ap_config.ap.password, apPassword.c_str(), sizeof(ap_config.ap.password));
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode       = apPassword.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (!staSsid.empty()) {
        wifi_config_t sta_config = {};
        strncpy((char*)sta_config.sta.ssid, staSsid.c_str(), sizeof(sta_config.sta.ssid));
        strncpy((char*)sta_config.sta.password, staPassword.c_str(), sizeof(sta_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    _is_wifi_inited = true;
    _is_ap_active   = true;

    if (!staSsid.empty()) {
        esp_err_t conn_err = esp_wifi_connect();
        if (conn_err == ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(
                s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                8000 / portTICK_PERIOD_MS);
            if (bits & WIFI_CONNECTED_BIT) {
                _is_wifi_connected = true;
                mclog::tagInfo(_tag, "APSTA: STA connected to {}", staSsid);
            } else {
                _is_wifi_connected = false;
                mclog::tagWarn(_tag, "APSTA: STA failed to connect to {}", staSsid);
            }
        }
    } else {
        _is_wifi_connected = false;
    }

    mclog::tagInfo(_tag, "APSTA started. AP: {} / STA: {}", apSsid, staSsid.empty() ? "(none)" : staSsid);
}

void NetworkManager::wifiDeinit()
{
    mclog::tagInfo(_tag, "wifi deinit");

    if (!_is_wifi_inited) {
        mclog::tagInfo(_tag, "wifi not inited, skipping deinit");
        return;
    }

    /* Properly deinitialize Wi-Fi */
    esp_wifi_stop();
    esp_wifi_deinit();

    _is_wifi_inited    = false;
    _is_wifi_connected = false;
    _is_ap_active      = false;
}

bool NetworkManager::wifiConnect(const std::string& ssid, const std::string& password)
{
    mclog::tagInfo(_tag, "wifi connect: {}", ssid);

    if (!_is_wifi_inited) {
        wifiInit();
        if (!_is_wifi_inited) {
            mclog::tagError(_tag, "wifi not inited, cannot connect");
            return false;
        }
    }

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password));

    /* 清掉上一次连接残留的事件位，否则失败位一直保留，后续连接会立即返回失败 */
    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);

    /* Set WiFi configuration */
    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (cfg_err != ESP_OK) {
        mclog::tagError(_tag, "wifi set config failed: {}", esp_err_to_name(cfg_err));
        return false;
    }

    /* Connect */
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        mclog::tagError(_tag, "wifi connect failed: {}", esp_err_to_name(err));
        return false;
    }

    /* Wait for connection with timeout */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        10000 / portTICK_PERIOD_MS);  /* 10 second timeout */

    if (bits & WIFI_CONNECTED_BIT) {
        _is_wifi_connected = true;
        mclog::tagInfo(_tag, "wifi connected successfully");
        return true;
    } else {
        _is_wifi_connected = false;
        mclog::tagError(_tag, "wifi connection failed or timed out (bits=0x{:x})", (unsigned)bits);
        return false;
    }
}

void NetworkManager::wifiDisconnect()
{
    mclog::tagInfo(_tag, "wifi disconnect");

    if (_is_wifi_connected) {
        esp_wifi_disconnect();
        _is_wifi_connected = false;
    }
}

/* -------------------------------------------------------------------------- */
/*                                   EspNow                                   */
/* -------------------------------------------------------------------------- */
void NetworkManager::espNowInit()
{
    mclog::tagInfo(_tag, "esp now init");
    /* Implement ESP-NOW initialization */
    _is_esp_now_inited = true;
}

void NetworkManager::espNowDeinit()
{
    _is_esp_now_inited = false;
}

void NetworkManager::espNowSend(const std::string& data)
{
    /* Implement ESP-NOW send logic */
}

bool NetworkManager::espNowAvailable()
{
    return !_esp_now_received_data.empty();
}

const std::string& NetworkManager::espNowGetReceivedData()
{
    return _esp_now_received_data;
}

void NetworkManager::espNowClearReceivedData()
{
    _esp_now_received_data.clear();
}

/* -------------------------------------------------------------------------- */
/*                                     IR                                     */
/* -------------------------------------------------------------------------- */
void NetworkManager::irInit()
{
    if (_is_ir_inited) {
        return;
    }
    mclog::tagInfo(_tag, "ir init on GPIO{}", HAL_PIN_IR_TX);
    ir_helper_init((gpio_num_t)HAL_PIN_IR_TX);
    _is_ir_inited = true;
}

void NetworkManager::irSend(uint8_t addr, uint8_t cmd)
{
    if (!_is_ir_inited) {
        irInit();
    }
    ir_helper_send(addr, cmd);
}

/* -------------------------------------------------------------------------- */
/*                                    BLE                                     */
/* -------------------------------------------------------------------------- */
void NetworkManager::bleKeyboardInit()
{
    mclog::tagInfo(_tag, "ble keyboard init");
    _is_ble_keyboard_inited = true;
}

bool NetworkManager::bleKeyboardIsConnected() const
{
    /* Return BLE keyboard connection status */
    return false;
}

/* -------------------------------------------------------------------------- */
/*                                    USB                                     */
/* -------------------------------------------------------------------------- */
void NetworkManager::usbKeyboardInit()
{
    mclog::tagInfo(_tag, "usb keyboard init");
    _is_usb_keyboard_inited = true;
}

bool NetworkManager::usbKeyboardIsConnected() const
{
    /* Return USB keyboard connection status */
    return false;
}

#pragma once
#include <string>
#include <vector>
#include <utility>

class NetworkManager {
public:
    using ScanResult_t = std::pair<int, std::string>;

    NetworkManager();
    ~NetworkManager();

    void init();
    void update();

    /* WiFi */
    void wifiInit();
    void wifiApInit(const std::string& ssid, const std::string& password);
    void wifiApStaInit(const std::string& apSsid, const std::string& apPassword,
                       const std::string& staSsid = "", const std::string& staPassword = "");
    void wifiDeinit();
    void wifiScan(std::vector<ScanResult_t>& scanResult);
    bool wifiConnect(const std::string& ssid, const std::string& password);
    bool isWifiConnected() const { return _is_wifi_connected; }
    bool isApActive() const { return _is_ap_active; }
    void wifiDisconnect();

    /* EspNow */
    void espNowInit();
    void espNowDeinit();
    void espNowSend(const std::string& data);
    bool espNowAvailable();
    const std::string& espNowGetReceivedData();
    void espNowClearReceivedData();

    /* IR */
    void irInit();
    void irSend(uint8_t addr, uint8_t cmd);

    /* BLE Keyboard */
    void bleKeyboardInit();
    bool bleKeyboardIsConnected() const;

    /* USB Keyboard */
    void usbKeyboardInit();
    bool usbKeyboardIsConnected() const;

private:
    bool _is_wifi_inited = false;
    bool _is_wifi_connected = false;
    bool _is_ap_active = false;
    bool _is_esp_now_inited = false;
    bool _is_ir_inited = false;
    bool _is_ble_keyboard_inited = false;
    bool _is_usb_keyboard_inited = false;
    int _ble_keyboard_event_slot_id = -1;
    int _usb_keyboard_event_slot_id = -1;
    std::string _esp_now_received_data;
};

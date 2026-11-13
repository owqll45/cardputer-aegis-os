#pragma once
#include <mooncake.h>
#include <cstdint>
#include <hal/hal.h>
#include <string>
#include <vector>

/**
 * 网络设置：
 *   - WiFi 默认关闭（不占用内存），需要时打开
 *   - 扫描附近 WiFi 并选择连接
 *   - 也可手动输入 SSID / 密码
 */
class AppSetWiFi : public mooncake::AppAbility {
public:
    AppSetWiFi();
    ~AppSetWiFi();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum Page_t { PAGE_MAIN = 0, PAGE_SCAN, PAGE_INPUT };
    enum State_t { ST_IDLE = 0, ST_SCANNING, ST_CONNECTING, ST_OK, ST_FAIL };

    Page_t _page = PAGE_MAIN;
    State_t _state = ST_IDLE;

    bool _wifi_on = false;
    bool _need_ssid = true;  // true: 正在输入 SSID；false: 正在输入密码

    std::vector<NetworkManager::ScanResult_t> _aps;
    int _sel = 0;
    int _scroll = 0;

    std::string _input;
    std::string _ssid;
    std::string _password;
    std::string _msg;

    int _key_slot = -1;
    uint32_t _cursor_t = 0;
    bool _cursor = false;
    uint32_t _msg_t = 0;

    bool _scan_req = false;
    bool _connect_req = false;
    bool _exit_req = false;

    void render_main();
    void render_scan();
    void render_input();
    void handle_key(const Keyboard::KeyEvent_t& e);
    void do_scan();
    void do_connect();
    void power_off();
};

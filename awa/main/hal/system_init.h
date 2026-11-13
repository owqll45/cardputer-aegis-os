#pragma once
#include <string>
#include <functional>
#include <esp_http_server.h>

enum class InitState {
    PENDING,
    IN_PROGRESS,
    COMPLETED
};

struct InitConfig {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string api_type = "deepseek";
    std::string api_key;
    std::string api_url;
    std::string llm_model = "deepseek-chat";
};

class SystemInit {
public:
    static SystemInit& getInstance();

    void init();
    void update();

    InitState getState() const { return _state; }
    bool isFirstBoot() const { return _state == InitState::PENDING || _state == InitState::IN_PROGRESS; }
    bool isInitCompleted() const { return _state == InitState::COMPLETED; }

    void markCompleted();
    void reset();

    bool applyConfiguration(const InitConfig& config);
    InitConfig getCurrentConfig() const { return _config; }

    std::string getPortalIP() const { return "192.168.4.1"; }

private:
    SystemInit() = default;
    ~SystemInit() = default;
    SystemInit(const SystemInit&) = delete;

    InitState _state = InitState::PENDING;
    InitConfig _config;
    httpd_handle_t _portal_server = nullptr;

    bool checkNvsFlag();
    void setNvsFlag();
    bool startPortal();
    void stopPortal();
    void registerEndpoints();
    std::string getSetupHtml();
};

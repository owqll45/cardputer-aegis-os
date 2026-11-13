#pragma once
#include <cstdint>
#include <string>

class PowerManager {
public:
    struct PowerStatus_t {
        float voltage;
        float current;
        int percentage;
        bool is_charging;
        bool usb_plugged;
        std::string health_status;
    };

    PowerManager();
    ~PowerManager();

    void init();
    void update();

    uint8_t getBatLevel() const { return _bat_level; }
    PowerStatus_t getPowerStatus() const { return _power_status; }
    void updatePowerSafety();
    uint32_t getLastAppHeartbeat() const { return _last_app_heartbeat; }
    void setLastAppHeartbeat(uint32_t timestamp) { _last_app_heartbeat = timestamp; }

private:
    uint8_t _bat_level = 0;
    PowerStatus_t _power_status = {0};
    uint32_t _last_app_heartbeat = 0;
    uint32_t _boot_time = 0;
    static constexpr uint32_t BOOT_PROTECTION_MS = 10000;
};

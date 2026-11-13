#include "system_utils.h"
#include <M5GFX.h>
#include <esp_mac.h>
#include <mooncake_log.h>

static const std::string _tag = "SystemUtils";

SystemUtils::SystemUtils()
{
}

SystemUtils::~SystemUtils()
{
}

void SystemUtils::init()
{
    mclog::tagInfo(_tag, "system utils init");
}

void SystemUtils::update()
{
}

void SystemUtils::delay(std::uint32_t ms)
{
    m5gfx::delay(ms);
}

std::uint32_t SystemUtils::millis()
{
    return m5gfx::millis();
}

void SystemUtils::feedTheDog()
{
    vTaskDelay(1);
}

std::vector<uint8_t> SystemUtils::getDeviceMac()
{
    std::vector<uint8_t> mac(6);
    esp_read_mac(mac.data(), ESP_MAC_EFUSE_FACTORY);
    return mac;
}

std::string SystemUtils::getDeviceMacString()
{
    auto mac = getDeviceMac();
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

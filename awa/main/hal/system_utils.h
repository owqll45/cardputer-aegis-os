#pragma once
#include <cstdint>
#include <string>
#include <vector>

class SystemUtils {
public:
    SystemUtils();
    ~SystemUtils();

    void init();
    void update();

    /* System Utilities */
    void delay(std::uint32_t ms);
    std::uint32_t millis();
    void feedTheDog();
    std::vector<uint8_t> getDeviceMac();
    std::string getDeviceMacString();

private:
};

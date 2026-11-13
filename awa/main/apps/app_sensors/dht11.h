#pragma once

#include <stdint.h>
#include "hal/hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class DHT11 {
public:
    DHT11(int pin);
    ~DHT11();
    
    int readTemperature();
    int readHumidity();
    bool readTempAndHumi(float* temperature, float* humidity);
    
private:
    int _pin;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    
    bool readSensor();
    uint8_t data[5];
    uint32_t _lastReadTime;
};

#pragma once

#include <stdint.h>
#include "hal/hal.h"

class Ultrasonic {
public:
    Ultrasonic(int trigPin, int echoPin);
    ~Ultrasonic();
    
    float readDistance();
    
private:
    int _trigPin;
    int _echoPin;
    uint32_t _lastReadTime;
};

#include "ultrasonic.h"
#include "hal/hal.h"
#include <driver/gpio.h>
#include <rom/ets_sys.h>

Ultrasonic::Ultrasonic(int trigPin, int echoPin) : _trigPin(trigPin), _echoPin(echoPin), _lastReadTime(0) {
    gpio_reset_pin((gpio_num_t)_trigPin);
    gpio_reset_pin((gpio_num_t)_echoPin);
    gpio_set_direction((gpio_num_t)_trigPin, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)_echoPin, GPIO_MODE_INPUT);
    gpio_set_level((gpio_num_t)_trigPin, 0);
}

Ultrasonic::~Ultrasonic() {
}

float Ultrasonic::readDistance() {
    // Implement debouncing - avoid reading too frequently (max 20 times per second)
    uint32_t currentTime = GetHAL().millis();
    if (currentTime - _lastReadTime < 50) {
        return -1.0f;
    }
    
    // Send trigger pulse
    gpio_set_level((gpio_num_t)_trigPin, 0);
    ets_delay_us(2);
    gpio_set_level((gpio_num_t)_trigPin, 1);
    ets_delay_us(10);
    gpio_set_level((gpio_num_t)_trigPin, 0);
    
    // Wait for echo pulse with timeout
    int64_t startWait;
    
    // Wait for echo start (LOW -> HIGH transition)
    startWait = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)_echoPin) == 0) {
        if (esp_timer_get_time() - startWait > 20000) {
            _lastReadTime = currentTime;
            return -1.0f;
        }
    }
    
    int64_t echoStart = esp_timer_get_time();
    
    // Wait for echo end (HIGH -> LOW transition)
    startWait = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)_echoPin) == 1) {
        if (esp_timer_get_time() - startWait > 20000) {
            _lastReadTime = currentTime;
            return -1.0f;
        }
    }
    
    int64_t echoEnd = esp_timer_get_time();
    int64_t duration = echoEnd - echoStart;
    
    // Calculate distance (speed of sound is 343m/s = 0.0343cm/μs, divided by 2 for round trip)
    float distance = (duration * 0.0343f) / 2.0f;
    
    // Filter out unrealistic values (0-500cm)
    if (distance < 2.0f || distance > 500.0f) {
        _lastReadTime = currentTime;
        return -1.0f;
    }
    
    _lastReadTime = currentTime;
    return distance;
}

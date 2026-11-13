#include "dht11.h"
#include "hal/hal.h"
#include <driver/gpio.h>
#include <rom/ets_sys.h>

DHT11::DHT11(int pin) : _pin(pin), _lastReadTime(0) {
    gpio_reset_pin((gpio_num_t)_pin);
    gpio_set_direction((gpio_num_t)_pin, GPIO_MODE_INPUT);
    gpio_set_level((gpio_num_t)_pin, 1);
}

DHT11::~DHT11() {
}

bool DHT11::readSensor() {
    // Reset data
    for (int i = 0; i < 5; i++) {
        data[i] = 0;
    }

    // Send start signal
    gpio_set_direction((gpio_num_t)_pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)_pin, 0);
    GetHAL().delay(18);
    
    // Begin Critical Section
    portENTER_CRITICAL(&_mux);
    
    gpio_set_level((gpio_num_t)_pin, 1);
    ets_delay_us(40);
    gpio_set_direction((gpio_num_t)_pin, GPIO_MODE_INPUT);

    uint64_t startWait;
    bool success = true;
    
    // Wait for response signal (HIGH -> LOW transition)
    startWait = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)_pin) == 1) {
        if (esp_timer_get_time() - startWait > 200) { // 200us timeout
            success = false; break;
        }
    }

    if (success) {
        // Wait for DHT11 to pull LOW (80us)
        startWait = esp_timer_get_time();
        while (gpio_get_level((gpio_num_t)_pin) == 0) {
            if (esp_timer_get_time() - startWait > 200) { success = false; break; }
        }
    }

    if (success) {
        // Wait for DHT11 to pull HIGH (80us)
        startWait = esp_timer_get_time();
        while (gpio_get_level((gpio_num_t)_pin) == 1) {
            if (esp_timer_get_time() - startWait > 200) { success = false; break; }
        }
    }

    if (success) {
        // Read 40 bits of data
        for (int i = 0; i < 40; i++) {
            // Wait for LOW (50us)
            startWait = esp_timer_get_time();
            while (gpio_get_level((gpio_num_t)_pin) == 0) {
                if (esp_timer_get_time() - startWait > 100) { success = false; break; }
            }
            if (!success) break;

            uint32_t pulseStart = esp_timer_get_time();

            // Wait for HIGH
            startWait = esp_timer_get_time();
            while (gpio_get_level((gpio_num_t)_pin) == 1) {
                if (esp_timer_get_time() - startWait > 100) { success = false; break; }
            }
            if (!success) break;

            uint32_t pulseDuration = esp_timer_get_time() - pulseStart;
            data[i / 8] <<= 1;
            if (pulseDuration > 40) {
                data[i / 8] |= 1;
            }
        }
    }
    
    portEXIT_CRITICAL(&_mux);
    
    if (!success) return false;

    // Check checksum
    if (data[4] != (data[0] + data[1] + data[2] + data[3])) {
        return false;
    }

    return true;
}

bool DHT11::readTempAndHumi(float* temperature, float* humidity) {
    if (!temperature || !humidity) {
        return false;
    }
    
    // Implement debouncing - avoid reading too frequently
    uint32_t currentTime = GetHAL().millis();
    if (currentTime - _lastReadTime < 1000) {
        return false;
    }
    
    if (readSensor()) {
        *temperature = static_cast<float>(data[2]);
        *humidity = static_cast<float>(data[0]);
        _lastReadTime = currentTime;
        return true;
    }
    return false;
}

int DHT11::readTemperature() {
    float temp;
    float humi;
    if (readTempAndHumi(&temp, &humi)) {
        return static_cast<int>(temp);
    }
    return -1;
}

int DHT11::readHumidity() {
    float temp;
    float humi;
    if (readTempAndHumi(&temp, &humi)) {
        return static_cast<int>(humi);
    }
    return -1;
}

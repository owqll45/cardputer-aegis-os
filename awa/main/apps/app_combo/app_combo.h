#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>

class AppCombo : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;
    
    // State
    bool _is_connected = false;
    bool _mouse_mode = false;
    
    // IMU
    float _accX, _accY, _accZ;
    float _gyroX, _gyroY, _gyroZ;
    float _pitch, _roll, _yaw;
    uint32_t _last_update_time = 0;
    
    void updateUI();
    void processMouse();
    void sendKey(char c);

public:
    AppCombo();
    ~AppCombo();
    
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

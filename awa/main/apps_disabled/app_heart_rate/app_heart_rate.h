#pragma once
#include <mooncake.h>
#include <vector>

#include <M5GFX.h>

class AppHeartRate : public mooncake::AppAbility {
public:
    AppHeartRate();
    ~AppHeartRate();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void _draw_heart(M5GFX* target, int x, int y, int size, uint16_t color);
    void _draw_ecg(M5GFX* target, int x, int y, int w, int h, uint16_t color);

    int _heart_rate = 0;
    bool _is_connected = false;
    std::vector<int> _history;
    std::vector<int> _ecg_wave;
    float _ecg_phase = 0;
};

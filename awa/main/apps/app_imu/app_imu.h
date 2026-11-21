#pragma once
#include <mooncake.h>
#include <cstdint>
#include <hal/hal.h>

/*
 * 水平仪（IMU）
 * - 双轴气泡 + 俯仰/横滚角度 + 角速度
 * - SPACE 校准（把当前姿态归零）
 */
class AppImu : public mooncake::AppAbility {
public:
    AppImu();
    ~AppImu();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    m5::imu_data_t _imu;
    float _zero_pitch = 0.0f;
    float _zero_roll  = 0.0f;
    float _pitch = 0.0f;
    float _roll  = 0.0f;
    volatile bool _cal_req = false;
    int _key_slot = -1;
    LGFX_Sprite* _canvas = nullptr;

    void draw();
};

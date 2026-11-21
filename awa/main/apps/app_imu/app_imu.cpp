#include "app_imu.h"
#include <apps/utils/app_canvas.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <math.h>
#include <stdio.h>

AppImu::AppImu() { setAppInfo().name = "Level"; }
AppImu::~AppImu() {}

void AppImu::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");
    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    GetHAL().imu.begin();
    _zero_pitch = 0.0f;
    _zero_roll  = 0.0f;

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& k) {
        if (k.isModifier || !k.state) return;
        if (k.keyCode == KEY_SPACE) _cal_req = true;
    });

    _imu = m5::imu_data_t();
    draw();
}

void AppImu::onRunning()
{
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }

    GetHAL().imu.getImuData(&_imu);

    float ax = _imu.accel.x;
    float ay = _imu.accel.y;
    float az = _imu.accel.z;

    float pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / (float)M_PI;
    float roll  = atan2f(ay, az) * 180.0f / (float)M_PI;

    if (_cal_req) {
        _cal_req    = false;
        _zero_pitch = pitch;
        _zero_roll  = roll;
    }

    _pitch = pitch - _zero_pitch;
    _roll  = roll - _zero_roll;

    draw();
}

void AppImu::onClose()
{
    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

void AppImu::draw()
{
    if (!_canvas) return;
    auto& cv = *_canvas;

    cv.fillScreen(0x0841);

    // 标题
    cv.fillRect(0, 0, 240, 13, 0x1082);
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(TFT_WHITE);
    cv.setCursor(4, 3);
    cv.print("LEVEL / 水平仪");
    cv.setTextColor(0x7BEF);
    cv.setCursor(150, 3);
    cv.print(_cal_req ? "CAL..." : "G0 EXIT");

    // 气泡圆盘
    const int cx = 68, cy = 72, R = 48;
    cv.fillCircle(cx, cy, R, 0x10A2);
    cv.drawCircle(cx, cy, R, 0x4A69);
    cv.drawCircle(cx, cy, (int)(R * 0.66f), 0x39E7);
    cv.drawCircle(cx, cy, (int)(R * 0.33f), 0x39E7);
    cv.drawCircle(cx, cy, 2, 0xFFFF);
    cv.drawFastHLine(cx - R, cy, R * 2, 0x39E7);
    cv.drawFastVLine(cx, cy - R, R * 2, 0x39E7);

    // ±30° 映射到半径
    float nx = _roll / 30.0f;
    float ny = _pitch / 30.0f;
    float m  = sqrtf(nx * nx + ny * ny);
    if (m > 1.0f) {
        nx /= m;
        ny /= m;
        m = 1.0f;
    }
    int bx = cx + (int)(nx * (R - 7));
    int by = cy + (int)(ny * (R - 7));

    bool level = (fabsf(_pitch) < 1.5f && fabsf(_roll) < 1.5f);
    uint16_t bubble = level ? TFT_GREEN : (m > 0.75f ? TFT_RED : TFT_YELLOW);
    cv.fillCircle(bx, by, 7, bubble);
    cv.drawCircle(bx, by, 7, TFT_WHITE);

    // 右侧读数
    char buf[40];
    cv.setFont(&fonts::efontCN_10);

    cv.setTextColor(0x7BEF);
    cv.setCursor(132, 22);
    cv.print("PITCH");
    cv.setTextColor(level ? TFT_GREEN : TFT_WHITE);
    cv.setFont(&fonts::Font2);
    cv.setCursor(132, 31);
    snprintf(buf, sizeof(buf), "%+6.1f", _pitch);
    cv.print(buf);

    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(0x7BEF);
    cv.setCursor(132, 52);
    cv.print("ROLL");
    cv.setTextColor(level ? TFT_GREEN : TFT_WHITE);
    cv.setFont(&fonts::Font2);
    cv.setCursor(132, 61);
    snprintf(buf, sizeof(buf), "%+6.1f", _roll);
    cv.print(buf);

    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(0x7BEF);
    cv.setCursor(132, 82);
    snprintf(buf, sizeof(buf), "G %+4.0f %+4.0f", _imu.gyro.x, _imu.gyro.y);
    cv.print(buf);
    cv.setCursor(132, 92);
    snprintf(buf, sizeof(buf), "Z %+4.0f d/s", _imu.gyro.z);
    cv.print(buf);

    // 倾角条
    int barW = 100;
    int barX = 132, barY = 106;
    cv.drawRect(barX, barY, barW, 6, 0x4A69);
    float t = _roll / 30.0f;
    if (t > 1) t = 1;
    if (t < -1) t = -1;
    cv.fillRect(barX + barW / 2, barY + 1, (int)(t * barW / 2), 4, bubble);
    cv.drawFastVLine(barX + barW / 2, barY - 1, 8, TFT_WHITE);

    cv.setTextColor(0x7BEF);
    cv.setCursor(4, 108);
    cv.print("SPACE 校准归零   G0 返回");

    appCanvasPush(&cv);
}

#include "app_imu.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include "../utils/ui_kit.h"
#include <cmath>

AppImu::AppImu() { setAppInfo().name = "IMU"; }
AppImu::~AppImu() {}

void AppImu::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _panel_angle = 0.0f;
    GetHAL().imu.begin();
}

void AppImu::onRunning() {
    // 全局统一返回键：G0 侧键（与五子棋一致）
    if (GetHAL().goButton().wasClicked()) { close(); return; }

    auto& cv = GetHAL().canvas();
    cv.fillScreen(UI_BG);
    ui_title(cv, "姿态 IMU");

    GetHAL().imu.getImuData(&_imu_data);
    render_imu_data_label();
    render_imu_panel();

    ui_hint(cv, "水平仪实时显示  G0 返回");
    GetHAL().pushCanvas();
}

void AppImu::onClose() { mclog::tagInfo(getAppInfo().name, "closed"); }

void AppImu::render_imu_data_label() {
    auto& cv = GetHAL().canvas();
    char buf[48];

    ui_card(cv, 84, 28, 150, 34);
    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    cv.setCursor(90, 31);
    cv.print("加速度 m/s²");
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(UI_ACCENT);
    snprintf(buf, sizeof(buf), "%+.1f %+.1f %+.1f", _imu_data.accel.x, _imu_data.accel.y, _imu_data.accel.z);
    cv.setCursor(90, 44);
    cv.print(buf);

    ui_card(cv, 84, 66, 150, 24);
    ui_kv(cv, 90, 70, 138, "角速度", buf);
}

void AppImu::render_imu_panel() {
    auto& cv = GetHAL().canvas();
    update_panel_angle();

    int cx = 41, cy = 60, r = 26;
    cv.fillCircle(cx, cy, r, UI_CARD);
    cv.drawCircle(cx, cy, r, UI_LINE);
    cv.drawCircle(cx, cy, 10, UI_LINE);
    cv.drawFastHLine(cx - r + 4, cy, (r - 4) * 2, UI_LINE);
    cv.drawFastVLine(cx, cy - r + 4, (r - 4) * 2, UI_LINE);

    float rad = _panel_angle * (float)M_PI / 180.0f;
    int ex = cx + (int)((r - 6) * sinf(rad));
    int ey = cy - (int)((r - 6) * cosf(rad));
    cv.drawLine(cx, cy, ex, ey, UI_ACCENT);
    cv.fillCircle(cx, cy, 3, UI_OK);
    cv.fillCircle(ex, ey, 4, UI_ACCENT);

    char buf[16];
    snprintf(buf, sizeof(buf), "%+.1f°", _panel_angle);
    cv.setFont(&fonts::efontCN_10_b);
    cv.setTextColor(UI_SUB);
    cv.setTextDatum(top_center);
    cv.drawString(buf, cx, 86);
    cv.setTextDatum(top_left);
}

void AppImu::update_panel_angle() {
    _panel_angle = atan2f(_imu_data.accel.x, _imu_data.accel.y) * 180.0f / (float)M_PI;
}

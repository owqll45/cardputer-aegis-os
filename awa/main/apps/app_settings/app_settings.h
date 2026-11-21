/*
 * app_settings.h - 系统设置（中文）
 *
 * 240x109 画布，单列可滚动列表：
 *   ↑↓ 选择   ←→ 调整   ENTER 执行   G0 返回
 * 所有改动即时生效并写入 NVS("system")，重启后由 HalFacade::init 重新应用。
 */
#pragma once
#include <mooncake.h>
#include <cstdint>

class AppSettings : public mooncake::AppAbility {
public:
    AppSettings();
    ~AppSettings();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum ItemId {
        IT_VOLUME = 0,
        IT_MUTE,
        IT_BRIGHT,
        IT_ACCENT,
        IT_WIFI,
        IT_LINK,
        IT_BT,
        IT_TZ,
        IT_NTP,
        IT_BAT,
        IT_MEM,
        IT_ABOUT,
        IT_RESET,
        IT_COUNT
    };

    int      _sel       = 0;
    int      _scroll    = 0;
    int      _key_slot  = -1;
    bool     _dirty     = true;
    bool     _show_about = false;
    bool     _confirm_reset = false;
    uint32_t _confirm_ms = 0;
    uint32_t _last_refresh = 0;
    uint32_t _repeat_ms = 0;
    uint32_t _ntp_ms = 0;
    uint32_t _toast_ms = 0;
    int      _ntp_state = 0;   // 0 未校时 1 校时中 2 成功 3 失败
    char     _toast[40] = {};

    // NVS 镜像值
    int  _nv_vol    = 128;   // 0..255，与 AudioManager 的 sound_volume 一致
    int  _nv_bright = 100;   // 0..100
    int  _nv_accent = 0;
    int  _nv_tz     = 8;
    bool _nv_mute   = false;
    bool _nv_wifi   = true;
    bool _nv_link   = true;

    void loadValues();
    void moveSel(int delta);
    void adjust(int delta);
    void activate();
    void startNtp();
    void resetDefaults();
    void setToast(const char* msg);

    const char* valueText(int item, char* buf, size_t n, uint16_t* color);
    void draw();
    void drawAbout();
};

#pragma once
#include <mooncake.h>
#include <cstdint>
#include <hal/hal.h>

/*
 * 麦克风电平 / 8 段频谱 / 录音回放
 * 16kHz 采样，512 点一帧（32ms）。
 * 按 SPACE 录一段再经喇叭回放，用来确认麦克风真的在采音。
 */
class AppMicMeter : public mooncake::AppAbility {
public:
    AppMicMeter();
    ~AppMicMeter();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum Mode { M_IDLE = 0, M_REC, M_PLAY };

    static const int NSAMP = 512;
    static const int NBAND = 8;
    static const int NHIST = 60;

    int16_t* _buf = nullptr;
    bool _have_data = false;
    bool _mic_started = false;
    bool _mic_ok = false;

    // 录音回放
    int16_t* _rec_buf = nullptr;
    int _rec_cap = 0;      // 实际分配到的采样点数
    int _rec_pos = 0;
    int _rec_chunk = 0;
    bool _rec_waiting = false;
    uint8_t _mode = M_IDLE;
    uint32_t _mode_ms = 0;

    float _rms_db = -90.0f;
    float _peak_db = -90.0f;
    float _peak_hold = -90.0f;
    uint32_t _peak_hold_ms = 0;
    float _band[NBAND] = {0};
    int _raw_peak = 0;

    float _hist[NHIST] = {0};
    int _hist_pos = 0;
    int _rec_fail = 0;
    uint32_t _frames = 0;
    uint32_t _log_ms = 0;

    int _key_slot = -1;
    bool _want_close = false;

    LGFX_Sprite* _canvas = nullptr;

    void analyze(const int16_t* data, int n);
    void draw();
    void startRecord();
    void finishRecord();
    void finishPlay();
};

#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include "keyboard/keyboard.h"
#include <string>
#include <vector>

class AppAI : public mooncake::AppAbility {
public:
    AppAI();
    ~AppAI();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum State {
        ST_IDLE,
        ST_THINKING,   // DeepSeek 思考 / ASR 识别 / TTS 等待
        ST_SPEAKING,   // 正在播放语音
        ST_RECORDING,  // 按住 V 录音
        ST_SETTINGS,   // WiFi 设置
    };

    struct Msg {
        bool user;
        std::string text;
    };

    struct Job {
        enum Kind { JOB_CHAT, JOB_ASR, JOB_TTS, JOB_WIFI } kind = JOB_CHAT;

        std::vector<Msg> msgs;  // JOB_CHAT
        std::string extra;      // JOB_TTS 文本 / JOB_WIFI SSID
        std::string extra2;     // JOB_WIFI 密码
        std::string result;     // 结果文本
        bool ok = false;
        bool done = false;
        bool abandoned = false;
        bool to_input = false;  // JOB_CHAT：结果回填输入框（拼音转中文）

        // 录音缓冲（JOB_ASR）：分成多块小内存，避免依赖一块大连续内存
        std::vector<uint8_t*> wav_blocks;
        std::vector<size_t> wav_cap;    // 每块容量
        std::vector<size_t> wav_used;
        size_t wav_total = 0;       // 已录制的 PCM 字节数
        size_t wav_cap_total = 0;   // 全部分块总容量
        uint32_t rate = 8000;
        volatile bool stop_rec = false;
    };

    LGFX_Sprite* _canvas = nullptr;
    bool _canvas_ok = true;  // 录音期间临时释放整屏画布内存
    int _key_slot = -1;
    int _W = 240;
    int _H = 135;

    State _state = ST_IDLE;
    std::string _input;
    std::string _pending_chat;  // 播报中按下发送，等 TTS 收尾后立即发起
    std::vector<Msg> _history;
    std::vector<std::string> _lines;  // 已换行的显示行
    bool _lines_dirty = true;

    Job* _job = nullptr;

    // UI 动效
    bool _dirty = true;
    bool _blink = true;
    uint32_t _last_blink = 0;
    uint32_t _last_dots = 0;
    uint32_t _last_petals = 0;
    uint32_t _last_wave = 0;
    uint32_t _last_canvas_retry = 0;
    int _dots = 0;
    int _wave = 0;
    std::string _toast;
    uint32_t _toast_until = 0;
    bool _was_online = false;

    // 语音
    std::string _mimo_key;
    bool _tts_on = true;
    bool _v_rec = false;  // 本次 V 按下是否触发录音
    bool _v_pending = false;    // V 按下但还没到长按阈值（短按视为输入字母 v）
    uint32_t _v_down_ms = 0;
    bool _want_record = false;  // 请求录音：真正的开工放到应用任务里，避免和绘制抢画布

    // WiFi 设置
    bool _wifi_connecting = false;
    bool _scanning = false;
    bool _need_scan = false;
    std::vector<std::string> _ap_list;
    int _ap_sel = 0;
    int _set_phase = 0;  // 0=选择网络 1=输入密码 2=手动输入名称
    std::string _ssid_buf;
    std::string _pass_buf;

    struct Petal {
        float x = 0, y = 0, vy = 0.6f, phase = 0;
        uint8_t size = 2;
    };
    Petal _petals[10];

    void handleKey(const Keyboard::KeyEvent_t& keyEvent);
    void submitText();
    void startPinyinConvert();
    void startChat(const std::string& text);
    void startRecording();
    void stopRecording();
    void startTts(const std::string& text);
    void releaseCanvasForAsr();
    void restoreCanvas();
    void drawRecordingScreen();
    uint32_t _rec_start = 0;
    uint32_t _rec_max_ms = 0;
    void startWifiJob(const std::string& ssid, const std::string& pass);
    void openSettings();
    void doScan();
    void handleSettingsKey(const Keyboard::KeyEvent_t& keyEvent);
    void drawSettings();
    void pollJob();
    void appendMsg(bool user, const std::string& text);
    void setToast(const std::string& text);

    void rebuildLines();
    std::vector<std::string> wrapText(const std::string& text, int maxWidth);
    void drawUI();
    void drawSakura(int cx, int cy, int r);
    void updatePetals(bool move);

    static void jobEntry(void* arg);
    static bool doChat(const std::vector<Msg>& msgs, std::string& out);
    static bool doAsr(const std::vector<uint8_t*>& blocks, const std::vector<size_t>& used,
                      size_t total, uint32_t rate, std::string& out);
    static bool doTts(const std::string& text, volatile bool* stop, std::string& err);

    static std::string loadMimoKey();
};

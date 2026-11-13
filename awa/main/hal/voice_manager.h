#pragma once
#include <string>
#include <functional>

enum class VoiceState { IDLE, LISTENING, PROCESSING, ERROR };

enum class VoiceTriggerMode {
    KEYBOARD_KEY,
    HOLD_BUTTON,
    CONTINUOUS
};

struct VoiceEvent {
    VoiceState state;
    std::string text;
    struct { int duration_ms = 0; bool isEmpty() const { return duration_ms == 0; } } buffer;
};

struct VoiceConfig {
    int trigger_key = 13;
    VoiceTriggerMode trigger_mode = VoiceTriggerMode::KEYBOARD_KEY;
    bool enable_vad = false;
    int buffer_ms = 5000;
};

class VoiceManager {
public:
    static VoiceManager& getInstance();

    void init(const VoiceConfig& cfg);
    void update();

    void setVoiceCallback(std::function<void(const VoiceEvent&)> cb);
    VoiceState getState() const { return _state; }

private:
    VoiceManager() = default;
    ~VoiceManager() = default;
    VoiceManager(const VoiceManager&) = delete;

    VoiceConfig _config;
    VoiceState _state = VoiceState::IDLE;
    std::function<void(const VoiceEvent&)> _callback;
};

#include "voice_manager.h"
#include <mooncake_log.h>
#include <hal/hal.h>

static const std::string TAG = "VoiceManager";

VoiceManager& VoiceManager::getInstance() {
    static VoiceManager instance;
    return instance;
}

void VoiceManager::init(const VoiceConfig& cfg) {
    _config = cfg;
    _state = VoiceState::IDLE;
    mclog::tagInfo(TAG, "init: trigger_mode={}", (int)cfg.trigger_mode);
}

void VoiceManager::update() {
    if (_config.trigger_mode == VoiceTriggerMode::KEYBOARD_KEY && _callback) {
        auto& key = GetHAL().keyboard.getLatestKeyEvent();
        if (key.state && key.keyCode == _config.trigger_key) {
            VoiceEvent evt;
            evt.state = VoiceState::LISTENING;
            evt.buffer.duration_ms = 0;
            _callback(evt);

            evt.state = VoiceState::PROCESSING;
            evt.text = "[voice input detected]";
            _callback(evt);
        }
    }
}

void VoiceManager::setVoiceCallback(std::function<void(const VoiceEvent&)> cb) {
    _callback = cb;
}

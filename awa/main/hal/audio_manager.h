#pragma once
#include <M5Unified.hpp>

class Settings;

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    void init(Settings* settings);
    void update();

    m5::Speaker_Class& getSpeaker() { return *_speaker; }
    m5::Mic_Class& getMic() { return *_mic; }

    void setGlobalMute(bool mute);
    bool isGlobalMute() const { return _global_mute; }

private:
    m5::Speaker_Class* _speaker = nullptr;
    m5::Mic_Class* _mic = nullptr;
    bool _global_mute = false;
    Settings* _settings = nullptr;
};

#include "audio_manager.h"
#include "utils/settings/settings.h"
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <esp_log.h>
#include <esp_system.h>

static const std::string _tag = "Audio";

AudioManager::AudioManager()
{
}

AudioManager::~AudioManager()
{
}

void AudioManager::init(Settings* settings)
{
    _speaker = &M5.Speaker;
    _mic = &M5.Mic;

    _settings = settings;
}

void AudioManager::update()
{
}

void AudioManager::setGlobalMute(bool mute)
{
    _global_mute = mute;
    if (mute) {
        M5.Speaker.stop();   // 立刻停掉正在播的提示音: DMA 结束, 调用方的临时缓冲马上释放
        M5.Speaker.setVolume(0);
    } else {
        int vol = 128;
        if (_settings) vol = _settings->GetInt("sound_volume", 128);
        M5.Speaker.setVolume(vol);
    }
    // 静音开关一定要能在日志里看到堆的变化, 方便确认那块提示音内存确实还回去了
    ESP_LOGW("AUDIO", "mute=%d heap=%u min_heap=%u", (int)mute,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

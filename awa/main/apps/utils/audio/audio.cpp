/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <esp_system.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <apps/utils/toast/toast.h>
#include <random>

namespace audio {

static std::vector<int> c_major_scale = {60, 62, 64, 65, 67, 69, 71};  // C大调音阶（C D E F G A B）

// playRaw() 是异步 DMA，直接引用调用者传入的缓冲区，不会拷贝。
// 因此缓冲区在播放结束前必须保持有效，否则就是 use-after-free。
// 这里等播放完成（带超时保护）后再让调用者的局部 vector 析构。
static void wait_until_done()
{
    uint32_t t0 = GetHAL().millis();
    while (GetHAL().speaker.isPlaying()) {
        if (GetHAL().millis() - t0 > 800) {  // 超时保护, 避免卡死
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// 分块流式播放:
// 一次只生成 512 帧(立体声 int16 共 2KB), 三块轮转复用, 峰值内存约 6KB。
// 三块而不是两块: Speaker 的虚拟通道队列深度为 2, 填第 i 块时第 i-3 块一定已播完,
// 因此可以安全覆盖, 同时始终预存下一块, 块与块之间不会出现空隙。
static constexpr int kChunkFrames        = 512;
// 沿用改前的 playRaw 播放参数(44100Hz / 单声道), 保证音高、时长与改前完全一致;
// 只显式指定虚拟通道, 避免和按键音抢同一个队列。
static constexpr int kStreamSampleRate   = 44100;
static constexpr int kMelodyChannel      = 6;
static constexpr int kToneChannel        = 5;

// fill(out, startFrame, frames): 生成 [startFrame, startFrame+frames) 这段帧
template <typename FillFn>
static void stream_play(int total_frames, int channel, FillFn fill)
{
    if (total_frames <= 0) {
        return;
    }

    std::vector<int16_t> buf[3];
    for (int i = 0; i < 3; ++i) {
        buf[i].resize(static_cast<size_t>(kChunkFrames) * 2);
    }

    // 实际播放时长按 len = frames*2 个单声道采样 @kStreamSampleRate 计算, 留 1.5s 余量
    const uint32_t deadline = GetHAL().millis() + static_cast<uint32_t>(2000.0 * total_frames / kStreamSampleRate) + 1500;

    int pos = 0;
    int cur = 0;
    while (pos < total_frames) {
        if (static_cast<int32_t>(GetHAL().millis() - deadline) >= 0) {
            break;
        }

        const int frames = std::min(kChunkFrames, total_frames - pos);
        fill(buf[cur].data(), pos, frames);

        // playRaw 是异步 DMA; 队列满时它内部会等到有空位再入队, 返回 false 则重试到超时
        while (!GetHAL().speaker.playRaw(buf[cur].data(), static_cast<size_t>(frames) * 2, kStreamSampleRate, false, 1,
                                         channel)) {
            if (static_cast<int32_t>(GetHAL().millis() - deadline) >= 0) {
                // 超时: 先停掉本通道再返回, 否则 DMA 还在读这些局部缓冲(use-after-free)
                GetHAL().speaker.stop(static_cast<uint8_t>(channel));
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        pos += frames;
        cur = (cur + 1) % 3;
    }

    // 等最后一块播完(或超时)再返回, 保证这三个局部缓冲可以安全释放
    while (GetHAL().speaker.isPlaying(static_cast<uint8_t>(channel))) {
        if (static_cast<int32_t>(GetHAL().millis() - deadline) >= 0) {
            // 超时兜底: 必须先把通道停掉, 不能让 DMA 继续读已经释放的局部缓冲
            GetHAL().speaker.stop(static_cast<uint8_t>(channel));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void play_tone(int frequency, double durationSec)
{
    if (GetHAL().speaker.getVolume() <= 0 || GetHAL().isGlobalMute()) {
        return;
    }

    const int sample_rate = GetHAL().speaker.config().sample_rate;
    const int samples     = static_cast<int>(sample_rate * durationSec);
    if (samples <= 0) {
        return;
    }

    const int fade_len    = 200;  // 淡出长度（采样点）
    const float amplitude = 32767.0f / 5;

    // 极短音(按键咔哒 4ms 等)一块就装得下: 保持原来的整块路径, 不折腾
    if (samples <= kChunkFrames) {
        // 局部缓冲即可: 但 playRaw 是异步 DMA, 直接读这块内存,
        // 所以必须等它播完再返回, 否则就是 use-after-free。
        std::vector<int16_t> buffer(static_cast<size_t>(samples) * 2);

        for (int i = 0; i < samples; ++i) {
            float amp = amplitude;

            // 应用结尾淡出（fade-out）
            if (i >= samples - fade_len) {
                float fade_factor = static_cast<float>(samples - i) / fade_len;
                amp *= fade_factor;
            }

            int16_t value     = static_cast<int16_t>(amp * sin(2.0 * M_PI * frequency * i / sample_rate));
            buffer[i * 2]     = value;  // 左声道
            buffer[i * 2 + 1] = value;  // 右声道
        }

        GetHAL().speaker.playRaw(buffer.data(), buffer.size());
        wait_until_done();
        return;
    }

    // 较长音(落子/提示音)分块生成, 峰值内存从 samples*4 字节降到约 6KB
    stream_play(samples, kToneChannel, [&](int16_t* out, int start, int frames) {
        for (int k = 0; k < frames; ++k) {
            const int i = start + k;
            float amp   = amplitude;
            if (i >= samples - fade_len) {
                float fade_factor = static_cast<float>(samples - i) / fade_len;
                amp *= fade_factor;
            }
            const int16_t value = static_cast<int16_t>(amp * sin(2.0 * M_PI * frequency * i / sample_rate));
            out[k * 2]          = value;  // 左声道
            out[k * 2 + 1]      = value;  // 右声道
        }
    });
}

void play_melody(const std::vector<int>& midiList, double durationSec = 0.1)
{
    if (GetHAL().speaker.getVolume() <= 0 || GetHAL().isGlobalMute()) {
        return;
    }
    if (midiList.empty()) {
        return;
    }

    const int sample_rate      = GetHAL().speaker.config().sample_rate;
    const int samples_per_note = static_cast<int>(sample_rate * durationSec);
    if (samples_per_note <= 0) {
        return;
    }
    const int fade_len         = 200;  // 每个音符结尾的淡出长度
    const float amplitude      = 32767.0f / 5;
    const int total_frames     = static_cast<int>(midiList.size()) * samples_per_note;

    mclog::tagInfo("audio", "melody stream notes={} frames={} chunk={} heap={} min_heap={}", (int)midiList.size(),
                   total_frames, kChunkFrames, (unsigned)esp_get_free_heap_size(),
                   (unsigned)esp_get_minimum_free_heap_size());

    stream_play(total_frames, kMelodyChannel, [&](int16_t* out, int start, int frames) {
        for (int k = 0; k < frames; ++k) {
            const int gi        = start + k;
            const int note_idx  = gi / samples_per_note;
            const int i         = gi % samples_per_note;
            const int midiNote  = midiList[static_cast<size_t>(note_idx)];

            float amp = amplitude;

            // 应用淡出（仅每个音符的结尾）
            if (i >= samples_per_note - fade_len) {
                float fade_factor = static_cast<float>(samples_per_note - i) / fade_len;
                amp *= fade_factor;
            }

            int16_t sample = 0;
            if (midiNote >= 0) {
                double freq = 440.0 * pow(2.0, (midiNote - 69) / 12.0);
                sample      = static_cast<int16_t>(amp * sin(2.0 * M_PI * freq * i / sample_rate));
            }

            out[k * 2]     = sample;  // 左声道
            out[k * 2 + 1] = sample;  // 右声道
        }
    });
}

void play_tone_from_midi(int midi, double durationSec)
{
    if (GetHAL().speaker.getVolume() <= 0) {
        return;
    }

    double freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    play_tone(static_cast<int>(freq), durationSec);
}

void play_random_tone(int semitoneShift = 0, double durationSec = 0.15)
{
    if (GetHAL().speaker.getVolume() <= 0 || GetHAL().isGlobalMute()) {
        return;
    }

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, static_cast<int>(c_major_scale.size()) - 1);

    int index = dist(gen);
    int midi  = c_major_scale[index] + semitoneShift;

    play_tone_from_midi(midi, durationSec);
}

/* -------------------------------------------------------------------------- */
/*                                  静音开关                                   */
/* -------------------------------------------------------------------------- */
void toggle_mute()
{
    // 状态统一放在 HAL: setGlobalMute(true) 会 stop() 掉正在播的声音并设音量 0,
    // 所有 play_* 在分配发声缓冲之前就 return, 所以静音时那块内存根本不申请。
    GetHAL().setGlobalMute(!GetHAL().isGlobalMute());
}

bool is_muted()
{
    return GetHAL().isGlobalMute();
}

/* -------------------------------------------------------------------------- */
/*                                  Keyboard                                  */
/* -------------------------------------------------------------------------- */
static void _keyboard_sfx_on_key_event(const Keyboard::KeyEvent_t& event)
{
    if (!event.state) {
        return;
    }

    // 全局静音键: '-' (KEY_MINUS), 任何 app 里都生效
    if (event.keyCode == KEY_MINUS) {
        toggle_mute();
        // 只置一个原子标志, 真正的绘制/推屏在 app_task 里做(这里绝不碰画布)
        ui_toast::notify(GetHAL().isGlobalMute());
        return;
    }

    // 静音时既不发声, 也绝不把音量偷偷调回来, 更不申请缓冲
    if (GetHAL().isGlobalMute() || GetHAL().speaker.getVolume() <= 0) {
        return;
    }

    GetHAL().speaker.setVolume(128); // Moderate volume for UI clicks

    // Cyberpunk / Mechanical Click Sound - Refined
    // Lower pitch for "thock" feel, extremely short
    play_tone(1500, 0.004); 
}

void set_keyboard_sfx_enable(bool enable)
{
    static size_t slot_id  = 0;
    static bool is_enabled = false;

    mclog::tagInfo("audio", "set keyboard sfx enable: {}", enable);
    if (enable) {
        if (is_enabled) {
            return;
        }
        is_enabled = true;
        slot_id    = GetHAL().keyboard.onKeyEvent.connect(_keyboard_sfx_on_key_event);
    } else {
        if (!is_enabled) {
            return;
        }
        is_enabled = false;
        GetHAL().keyboard.onKeyEvent.disconnect(slot_id);
    }
}

}  // namespace audio
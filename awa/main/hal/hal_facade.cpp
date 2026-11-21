#include "hal_facade.h"
#include "utils/sys_time.h"
#include "utils/perf.h"
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <driver/gpio.h>

static std::unique_ptr<HalFacade> _facade_instance;
static const std::string _tag = "HalFacade";

HalFacade& GetHAL()
{
    if (!_facade_instance) {
        mclog::tagInfo(_tag, "creating hal facade instance");
        _facade_instance = std::make_unique<HalFacade>();
    }
    return *_facade_instance.get();
}

void HalFacade::init()
{
    mclog::tagInfo(_tag, "hal facade init");

    uint32_t t0 = perf_ms();
    M5.begin();
    uint32_t t1 = perf_ms();
    ESP_LOGI("PERF", "  hal.M5.begin = %u ms (t=%u)", (unsigned)(t1 - t0), (unsigned)t1);
    /* M5Unified 为了 Capsule/Dial/DinMeter 的 power-hold，无条件把 GPIO46 拉成输出高。
     * 而 Cardputer ADV 的 GPIO46 是 ES8311 的 ADC 数据输入（麦克风）。
     * 输出高会把 I2S 数据线钉死在 1，录回来的采样恒为 -1（听起来就是"录不到声音"）。
     * 这里恢复成普通输入。 */
    if (M5.getBoard() == m5::board_t::board_M5CardputerADV) {
        gpio_reset_pin(GPIO_NUM_46);
        gpio_set_direction(GPIO_NUM_46, GPIO_MODE_INPUT);
        gpio_pullup_dis(GPIO_NUM_46);
        gpio_pulldown_dis(GPIO_NUM_46);
        mclog::tagInfo(_tag, "GPIO46 restored to input (mic data in)");
    }
    M5.Display.setBrightness(255);
    mclog::tagInfo(_tag, "M5.begin() done, brightness set");

    /* CRITICAL: Initialize display pointer AFTER M5.begin() */
    _display_manager.setDisplay(&M5.Display);
    _display_ptr = &M5.Display;

    /* Initialize all modules in order */
    _system_utils.init();
    ESP_LOGI("PERF", "  hal.system_utils = %u ms", (unsigned)(perf_ms() - t1)); t1 = perf_ms();

    _display_manager.init();
    _display_manager.setEmergencyMode(false);
    ESP_LOGI("PERF", "  hal.display = %u ms", (unsigned)(perf_ms() - t1)); t1 = perf_ms();

    /* CRITICAL: Initialize canvas pointers AFTER resetCanvas */
    _canvas_ptr = &_display_manager.getCanvas();  // FIX: use _canvas (pushed to display) not _canvas_ui
    _canvas_system_bar_ptr = &_display_manager.getSystemBarCanvas();
    _canvas_keyboard_bar_ptr = &_display_manager.getKeyboardBarCanvas();

    /* homeButton 现在与 goButton 共用同一个锁存实例，无需额外指针 */

    _settings = new Settings("system", true);

    /* 把系统设置里持久化的项在开机时重新应用：音量 / 静音 / 亮度 / 时区 */
    {
        int vol = _settings->GetInt("sound_volume", 128);
        if (vol < 0) vol = 0;
        if (vol > 255) vol = 255;
        M5.Speaker.setVolume(vol);

        int bright = _settings->GetInt("brightness", 100);
        if (bright < 10) bright = 10;
        if (bright > 100) bright = 100;
        M5.Display.setBrightness((uint8_t)(bright * 255 / 100));

        applyTimeZone(_settings->GetInt("tz_offset", 8));

        ESP_LOGI("HAL", "settings restored vol=%d bright=%d accent=%d tz=%d mute=%d wifi=%d link=%d",
                 vol, bright, _settings->GetInt("accent", 0), _settings->GetInt("tz_offset", 8),
                 (int)_settings->GetBool("mute", false), (int)_settings->GetBool("wifi_on", true),
                 (int)_settings->GetBool("glink_on", true));
    }

    _audio_manager.init(_settings);
    if (_settings->GetBool("mute", false)) {
        _audio_manager.setGlobalMute(true);
    }
    ESP_LOGI("PERF", "  hal.audio = %u ms", (unsigned)(perf_ms() - t1)); t1 = perf_ms();
    _input_manager.init();
    ESP_LOGI("PERF", "  hal.keyboard = %u ms", (unsigned)(perf_ms() - t1)); t1 = perf_ms();
    _power_manager.init();
    _network_manager.init();
    _storage_manager.init();
    ESP_LOGI("PERF", "  hal.power/net/storage = %u ms", (unsigned)(perf_ms() - t1)); t1 = perf_ms();
    _life_guard_manager.init();
    _system_manager.init();
    _ui_renderer.init();
    ESP_LOGI("PERF", "  hal.others = %u ms", (unsigned)(perf_ms() - t1));

    mclog::tagInfo(_tag, "all modules initialized");

    /* Update M5 state (buttons, etc.) for crash_handler usage */
    M5.update();

    /* Trigger display initialization by a dummy operation */
    _display_manager.getDisplay().startWrite();
    _display_manager.getDisplay().endWrite();
}

void HalFacade::update()
{
    /* 刷新 M5 物理按键（BtnA/GPIO0）等状态，否则 goButton() 永远不触发；
       同时把点击锁存，供刷新率低的应用取用 */
    M5.update();
    _go_button.poll();

    /* Update all modules */
    _display_manager.update();
    _audio_manager.update();
    _input_manager.update();
    _power_manager.update();
    _network_manager.update();
    _storage_manager.update();
    _life_guard_manager.update();
    _system_manager.update();
    _ui_renderer.update();
    _system_utils.update();

    /* Feed watchdog */
    _system_utils.feedTheDog();
}

/* UIContextGuard Implementation */
HalFacade::UIContextGuard::UIContextGuard()
{
    auto& display = GetHAL().display();
    font = display.getFont();
    datum = display.getTextDatum();
}

HalFacade::UIContextGuard::~UIContextGuard()
{
    auto& display = GetHAL().display();
    display.setFont(font);
    display.setTextDatum(datum);
}


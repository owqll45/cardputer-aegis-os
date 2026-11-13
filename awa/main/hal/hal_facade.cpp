#include "hal_facade.h"
#include <M5Unified.hpp>
#include <mooncake_log.h>

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

    M5.begin();
    M5.Display.setBrightness(255);
    mclog::tagInfo(_tag, "M5.begin() done, brightness set");

    /* CRITICAL: Initialize display pointer AFTER M5.begin() */
    _display_manager.setDisplay(&M5.Display);
    _display_ptr = &M5.Display;

    /* Initialize all modules in order */
    _system_utils.init();

    _display_manager.init();
    _display_manager.setEmergencyMode(false);

    /* CRITICAL: Initialize canvas pointers AFTER resetCanvas */
    _canvas_ptr = &_display_manager.getCanvas();  // FIX: use _canvas (pushed to display) not _canvas_ui
    _canvas_system_bar_ptr = &_display_manager.getSystemBarCanvas();
    _canvas_keyboard_bar_ptr = &_display_manager.getKeyboardBarCanvas();

    /* homeButton 现在与 goButton 共用同一个锁存实例，无需额外指针 */

    _settings = new Settings("system", true);

    _audio_manager.init(_settings);
    _input_manager.init();
    _power_manager.init();
    _network_manager.init();
    _storage_manager.init();
    _life_guard_manager.init();
    _system_manager.init();
    _ui_renderer.init();

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


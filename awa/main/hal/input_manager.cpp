#include "input_manager.h"
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const std::string _tag = "Input";

InputManager::InputManager()
{
}

InputManager::~InputManager()
{
}

void InputManager::init()
{
    mclog::tagInfo(_tag, "input init");

    _home_button = &M5.BtnA;

    if (!_keyboard.init()) {
        mclog::tagError(_tag, "keyboard init failed");
        return;
    }
}

void InputManager::update()
{
    _keyboard.update();

    // 周期性系统水位(每 5 秒): 还原运行中 heap / 最小 heap / 栈剩余的变化,
    // 尤其能看出全屏 sprite(240x135 8bpp = 32400B) 分配失败前的余量
    static int64_t last_log_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us >= 5000000) {
        last_log_us = now_us;
        ESP_LOGI("SYS", "heap=%u min_heap=%u stack_hw=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
}

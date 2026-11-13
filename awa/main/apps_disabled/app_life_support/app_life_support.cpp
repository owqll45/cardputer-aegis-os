#include "app_life_support.h"
#include "../../crash_handler.h"
#include <mooncake_log.h>

AppLifeSupport::AppLifeSupport()
{
    auto& info = setAppInfo();
    info.name  = "LifeSupport";
}

AppLifeSupport::~AppLifeSupport()
{
}

void AppLifeSupport::onOpen()
{
    mclog::tagInfo("LifeSupport", "onOpen");
    crash_handler::set_running_app("LifeSupport");

    _canvas = new LGFX_Sprite(&GetHAL().display());
    _canvas->createSprite(240, 135);
    _selected_index = 0;
}

void AppLifeSupport::onRunning()
{
    GetHAL().reportAppHealth();

    _draw_ui();

    GetHAL().keyboard.update();
    auto& key = GetHAL().keyboard.getLatestKeyEvent();

    if (key.state) {
        if (key.keyCode == KEY_ENTER || key.keyCode == 13 || key.keyCode == 10 || key.keyCode == 'g' ||
            strcmp(key.keyName, "enter") == 0) {
            if (_selected_index == 0) {
                crash_handler::show_life_alert("惊恐发作危险 (Panic Attack)",
                                               "检测到剧烈生理波动及早搏\n请配合进行深呼吸引导与系统干预", TFT_RED);
            } else if (_selected_index == 1) {
                crash_handler::show_life_alert("生理过载休克预警 (Overload)",
                                               "心率及压力读数超过身体机能极值\n已启动维生系统自动镇定剂注射协议!",
                                               TFT_ORANGE);
            } else if (_selected_index == 2) {
                crash_handler::show_life_alert("睡眠梦魇锁死 (Ghost Press)",
                                               "检测到深度睡眠产生严重体征急降\n正在释放低频神经促醒脉冲...",
                                               TFT_PURPLE);
            } else if (_selected_index == 3) {
                mclog::tagError("LifeSupport", "Triggering Fatal Crash...");
                while (1) {
                }  // Watchdog Crash
            } else if (_selected_index == 4) {
                GetHAL().renderSafeModeMonitor();
            }
        } else if (key.keyCode == ';' || key.keyCode == 0x33 || key.keyCode == KEY_UP) {
            _selected_index = (_selected_index - 1 + 5) % 5;
        } else if (key.keyCode == '.' || key.keyCode == 0x37 || key.keyCode == KEY_DOWN) {
            _selected_index = (_selected_index + 1) % 5;
        }
        GetHAL().keyboard.clearKeyEvent();
    }

    if (GetHAL().goButton().wasClicked()) {
        close();
    }
}

void AppLifeSupport::onClose()
{
    if (_canvas) {
        _canvas->deleteSprite();
        delete _canvas;
        _canvas = nullptr;
    }
    M5.Display.fillScreen(TFT_BLACK);
}

void AppLifeSupport::_draw_ui()
{
    if (!_canvas) return;
    _canvas->fillScreen(TFT_BLACK);

    _canvas->setFont(&fonts::efontCN_14);
    _canvas->setTextColor(TFT_RED, TFT_BLACK);
    _canvas->drawCenterString("生命维持追踪干预系统 v4.9", 120, 4);

    // -------- LIVE DATA --------
    auto status = GetHAL().getLifeGuardStatus();
    _canvas->setFont(&fonts::efontCN_12);
    if (status.heart_rate > 0) {
        _canvas->setTextColor(TFT_GREEN, TFT_BLACK);
        _canvas->setCursor(10, 22);
        _canvas->printf("系统状�? 活体同步�?| 当前心率: %d BPM", status.heart_rate);
        _canvas->setCursor(10, 37);
        _canvas->printf("HRV阻尼: %dms | 压降: %d | 综合危急�? %.1f", status.hrv, status.delta_hr, status.risk_score);
    } else {
        _canvas->setTextColor(TFT_YELLOW, TFT_BLACK);
        _canvas->setCursor(10, 22);
        _canvas->printf("系统状�? 搜索蓝牙维生传感器矩�?..");
        _canvas->setCursor(10, 37);
        _canvas->printf("HRV阻尼: -- | 压降: -- | 综合危急�? --");
    }
    // ---------------------------

    _canvas->drawFastHLine(10, 52, 220, 0x3186);

    _canvas->setTextColor(TFT_WHITE, TFT_BLACK);
    _canvas->setCursor(10, 58);
    _canvas->print("触发底层强干扰覆写测试协�?");

    const char* options[] = {"1. 突发惊恐抢救注入 (Panic)", "2. 全局生理过载休克预警 (Overload)",
                             "3. 睡眠梦魇体征锁死干预 (Nightmare)", "4. 强制系统内核崩溃断电",
                             "5. 强制挂载物理生命维护模式"};

    for (int i = 0; i < 5; i++) {
        int y = 73 + i * 14;  // 缩小间距以放得下5项
        if (i == _selected_index) {
            _canvas->fillRoundRect(5, y - 1, 230, 14, 2, 0x3186);
            _canvas->setTextColor(TFT_YELLOW, 0x3186);
        } else {
            _canvas->setTextColor(TFT_LIGHTGRAY, TFT_BLACK);
        }
        _canvas->setCursor(15, y + 1);
        _canvas->print(options[i]);
    }

    _canvas->pushSprite(0, 0);
}

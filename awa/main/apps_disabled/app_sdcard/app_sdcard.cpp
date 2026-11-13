#include "app_sdcard.h"
#include <mooncake_log.h>
#include <hal/hal.h>

AppSdcard::AppSdcard() { setAppInfo().name = "SD Card"; }
AppSdcard::~AppSdcard() {}

void AppSdcard::onOpen() { mclog::tagInfo(getAppInfo().name, "opened"); _time_count = 0; }

void AppSdcard::onRunning() {
    auto* c = &GetHAL().canvas();
    c->fillScreen(TFT_BLACK);
    c->setTextColor(TFT_WHITE); c->setTextSize(2); c->setCursor(10, 10); c->print("SD Card");
    c->setTextColor(TFT_DARKGREY); c->setTextSize(1); c->setCursor(10, 40);
    c->print("Insert microSD card into");
    c->setCursor(10, 55); c->print("Cardputer slot to use.");
    c->setCursor(10, 80); c->setTextColor(TFT_CYAN);
    c->print("SD Card Manager");
    c->setTextColor(TFT_DARKGREY); c->setCursor(10, 115);
    c->print("[ESC] or [Home] to exit");
    c->pushSprite(0, 0);
    // Unified exit: ESC or home button
    if (GetHAL().goButton().wasClicked()) { close(); return; }
    auto& kb = GetHAL().keyboard;
    if (kb.getLatestKeyEvent().state && (kb.getLatestKeyEvent().keyCode == KEY_ESC || kb.getLatestKeyEvent().keyCode == KEY_ENTER)) {
        kb.clearKeyEvent(); close(); return;
    }
}

void AppSdcard::onClose() { mclog::tagInfo(getAppInfo().name, "closed"); }
void AppSdcard::probe_sd_card() { /* placeholder */ }

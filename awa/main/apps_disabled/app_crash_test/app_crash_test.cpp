// Minimal Crash Test App
#include "app_crash_test.h"
#include <mooncake_log.h>
#include <hal/hal.h>

AppCrashTest::AppCrashTest() {
    setAppInfo().name = "CrashTest";
}

AppCrashTest::~AppCrashTest() {}

void AppCrashTest::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
}

void AppCrashTest::onRunning() {
    auto* c = &GetHAL().canvas();
    c->fillScreen(TFT_BLACK);
    c->setTextColor(TFT_RED);
    c->setTextSize(2);
    c->setCursor(10, 10);
    c->print("Crash Test");
    c->setTextSize(1);
    c->setTextColor(TFT_WHITE);
    c->setCursor(10, 50);
    c->print("ESC to exit");
    c->pushSprite(0, 0);
    
    if (GetHAL().goButton().wasClicked()) close();
}

void AppCrashTest::onClose() {
    mclog::tagInfo(getAppInfo().name, "closed");
}

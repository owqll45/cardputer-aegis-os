#include "system_init.h"
#include "config_store.h"
#include "sd_card_manager.h"
#include "web_admin.h"
#include <hal/hal.h>
#include <mooncake_log.h>

static const std::string TAG = "SysInit";

SystemInit& SystemInit::getInstance() {
    static SystemInit instance;
    return instance;
}

void SystemInit::init() {
    mclog::tagInfo(TAG, "Checking setup status...");
    _setup_done = ConfigStore::getInstance().isFirstBootDone();

    if (!_setup_done) {
        showSetupScreen();
        return;
    }

    auto& sd = SdCardManager::getInstance();
    sd.init();

    if (sd.getState() == SdState::NEEDS_FORMAT) {
        _needs_format = true;
        showFormatPrompt();
    }
}

void SystemInit::showSetupScreen() {
    static bool started = false;
    if (!started) {
        auto& c = GetHAL().canvas();
        c.fillSprite(TFT_BLACK);
        c.setTextColor(TFT_WHITE, TFT_BLACK); c.setTextSize(1);
        c.setCursor(20, 10); c.print("Cardputer ADV");
        c.setCursor(15, 28); c.setTextSize(1); c.setTextColor(TFT_CYAN, TFT_BLACK);
        c.print("\u521d\u6b21\u8bbe\u7f6e");
        c.setTextColor(TFT_LIGHTGRAY, TFT_BLACK); c.setTextSize(1);
        const char* lines[] = {
            "", "\u8bf7\u5728\u7535\u8111\u4e0a\u6253\u5f00:", "http://192.168.4.1",
            "\u5b8c\u6210API\u914d\u7f6e\u540e\u53ef\u4f7f\u7528", "", "\u6b63\u5728\u542f\u52a8AP...", nullptr
        };
        int y = 46;
        for (int i = 0; lines[i]; i++) { c.setCursor(12, y); c.print(lines[i]); y += 14; }
        GetHAL().wifiApInit("Cardputer-ADV", "12345678");
        WebAdmin::getInstance().start(80);
        mclog::tagInfo(TAG, "AP started: Cardputer-ADV");
        started = true;
    }
}

void SystemInit::showFormatPrompt() {
    static bool init = false;
    if (!init) {
        auto& c = GetHAL().canvas();
        c.fillSprite(TFT_BLACK);
        c.setTextColor(TFT_RED, TFT_BLACK); c.setTextSize(1);
        c.setCursor(30, 30); c.print("\u26a0 TF\u5361\u683c\u5f0f\u4e0d\u652f\u6301");
        c.setTextColor(TFT_YELLOW, TFT_BLACK);
        c.setCursor(18, 52); c.print("\u662f\u5426\u683c\u5f0f\u5316? \u56de\u8f66=\u662f");
        init = true;
    }
}

void SystemInit::update() {}

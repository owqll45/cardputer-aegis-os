#include "app_repl.h"
#include <mooncake_log.h>
#include <hal/hal.h>

AppREPL::AppREPL() { setAppInfo().name = "Python REPL"; }
AppREPL::~AppREPL() {}

void AppREPL::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _pika_main = pikaScriptInit();
}

void AppREPL::onRunning() {
    auto* c = &GetHAL().canvas();
    c->fillScreen(TFT_BLACK);
    c->setTextColor(TFT_GREEN); c->setTextSize(1); c->setCursor(5, 5); c->print("PikaScript Python REPL");
    c->setTextColor(TFT_WHITE); c->setCursor(5, 25);
    if (_pika_main) c->print("Interpreter ready!");
    else c->print("Init failed");
    c->setTextColor(TFT_DARKGREY); c->setCursor(5, 120); c->print("ESC: back");
    c->pushSprite(0, 0);
    if (GetHAL().goButton().wasClicked()) close();
}

void AppREPL::onClose() { mclog::tagInfo(getAppInfo().name, "closed"); }
void AppREPL::execute_python_command(const std::string& command) { /* TODO */ }
void AppREPL::setup_pikapython_output_redirect() { /* TODO */ }

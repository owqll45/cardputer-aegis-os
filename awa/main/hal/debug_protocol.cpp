#include "debug_protocol.h"
#include <mooncake_log.h>
#include <cstring>
#include <esp_system.h>

static const std::string TAG = "DebugProtocol";

DebugProtocol& DebugProtocol::getInstance() {
    static DebugProtocol instance;
    return instance;
}

void DebugProtocol::init() { mclog::tagInfo(TAG, "init"); }
void DebugProtocol::update() {}

DebugResult DebugProtocol::execute(const std::string& cmd) {
    DebugResult r;
    if (cmd == "info" || cmd == "status") {
        r.output = "Cardputer ADV - OK\n";
        r.output += "Heap: " + std::to_string(esp_get_free_heap_size() / 1024) + "KB\n";
    } else if (cmd == "help") {
        r.output = "Commands: info, status, help, heap, reboot\n";
    } else if (cmd == "heap") {
        r.output = "Free: " + std::to_string(esp_get_free_heap_size()) + "\n";
    } else if (cmd == "reboot") {
        r.output = "Rebooting...\n";
        esp_restart();
    } else {
        r.error = "Unknown command: " + cmd;
    }
    return r;
}

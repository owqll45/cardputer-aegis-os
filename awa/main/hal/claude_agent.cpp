#include "claude_agent.h"
#include "ai_manager.h"
#include <mooncake_log.h>

static const std::string TAG = "ClaudeAgent";

ClaudeAgent& ClaudeAgent::getInstance() {
    static ClaudeAgent instance;
    return instance;
}

void ClaudeAgent::init() { mclog::tagInfo(TAG, "init"); }
void ClaudeAgent::update() {}

std::string ClaudeAgent::process(const std::string& input) {
    mclog::tagInfo(TAG, "process: {} chars", input.size());
    return AiManager::getInstance().chat(input);
}

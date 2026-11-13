#pragma once
#include <string>

class ClaudeAgent {
public:
    static ClaudeAgent& getInstance();

    void init();
    void update();

    std::string process(const std::string& input);

private:
    ClaudeAgent() = default;
};

#pragma once
#include <string>

struct DebugResult {
    std::string output;
    std::string error;
};

class DebugProtocol {
public:
    static DebugProtocol& getInstance();

    void init();
    void update();

    DebugResult execute(const std::string& cmd);

private:
    DebugProtocol() = default;
};

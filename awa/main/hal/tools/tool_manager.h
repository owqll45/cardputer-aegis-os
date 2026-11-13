#pragma once
#include <string>
#include <vector>
#include <functional>

struct ToolResult {
    bool success = false;
    std::string output;
    std::string error;
};

class ToolManager {
public:
    static ToolManager& getInstance();

    void init();
    void update();

    void setAskUserCallback(std::function<void(const std::string&, const std::vector<std::string>&)> cb) { _ask_user_cb = cb; }

    ToolResult executeTool(const std::string& tool_name, const std::string& params);

    std::vector<std::string> getToolList() const;

private:
    ToolManager() = default;
    ToolResult askUser(const std::string& question);
    ToolResult getTimeInfo();
    ToolResult runJsCode(const std::string& code);
    ToolResult getDeviceInfo();
    ToolResult getBatteryInfo();

    std::function<void(const std::string&, const std::vector<std::string>&)> _ask_user_cb;
};

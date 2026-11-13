#pragma once
#include <string>
#include <vector>
#include <functional>

struct McpTool {
    std::string name;
    std::string description;
    std::string input_schema;
};

struct McpToolResult {
    bool success = false;
    std::string content;
    std::string error;
};

class McpClient {
public:
    static McpClient& getInstance();

    void init();
    void update();

    bool connect(const std::string& url);
    void disconnect();
    bool isConnected() const { return _connected; }

    std::vector<McpTool> listTools();
    McpToolResult callTool(const std::string& name, const std::string& args);

    void setOnToolsUpdated(std::function<void(const std::vector<McpTool>&)> cb) { _on_tools_updated = cb; }

private:
    McpClient() = default;

    bool sendJsonRpc(const std::string& method, const std::string& params, std::string& response, int timeout_ms = 10000);
    bool initialize();
    void refreshTools();

    bool _connected = false;
    bool _initialized = false;
    std::string _server_url;
    std::vector<McpTool> _cached_tools;
    int _next_id = 1;
    std::function<void(const std::vector<McpTool>&)> _on_tools_updated;
};

#include "mcp_client.h"
#include "config_store.h"
#include <mooncake_log.h>
#include <cJSON.h>
#include <esp_http_client.h>
#include <cstring>

static const std::string TAG = "MCP";

McpClient& McpClient::getInstance() {
    static McpClient instance;
    return instance;
}

void McpClient::init() {
    mclog::tagInfo(TAG, "init");
    auto& s = ConfigStore::getInstance();
    if (s.isMcpEnabled() && !s.getMcpUrl().empty()) {
        connect(s.getMcpUrl());
    }
}

void McpClient::update() {
}

bool McpClient::sendJsonRpc(const std::string& method, const std::string& params, std::string& response, int timeout_ms) {
    if (_server_url.empty()) {
        mclog::tagWarn(TAG, "No server URL");
        return false;
    }

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", _next_id++);
    cJSON_AddStringToObject(req, "method", method.c_str());

    if (!params.empty()) {
        cJSON* p = cJSON_Parse(params.c_str());
        if (p) {
            cJSON_AddItemToObject(req, "params", p);
        } else {
            cJSON_AddStringToObject(req, "params", params.c_str());
        }
    }

    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (!body) {
        mclog::tagWarn(TAG, "Failed to create JSON-RPC body");
        return false;
    }

    std::string body_str(body);
    free(body);

    std::string url = _server_url;
    if (url.find("/sse") != std::string::npos) {
        size_t pos = url.find("/sse");
        url = url.substr(0, pos) + "/message";
    } else if (url.find("/mcp") == std::string::npos) {
        if (url.back() == '/') url += "mcp";
        else url += "/mcp";
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = timeout_ms;
    config.buffer_size = 4096;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        mclog::tagWarn(TAG, "HTTP client init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body_str.c_str(), body_str.length());

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        mclog::tagWarn(TAG, "HTTP error: {} status={}", esp_err_to_name(err), status);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_len = esp_http_client_get_content_length(client);
    if (content_len > 0 && content_len < 16384) {
        response.resize(content_len);
        int read = esp_http_client_read(client, &response[0], content_len);
        if (read > 0) response.resize(read);
        else response.clear();
    }

    esp_http_client_cleanup(client);
    mclog::tagInfo(TAG, "JSON-RPC {} response: {} bytes", method, response.size());
    return true;
}

bool McpClient::initialize() {
    std::string params = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"CardputerADV\",\"version\":\"1.0.0\"}}";
    std::string response;

    if (!sendJsonRpc("initialize", params, response, 15000)) {
        mclog::tagWarn(TAG, "MCP initialize failed");
        return false;
    }

    cJSON* j = cJSON_Parse(response.c_str());
    if (!j) {
        mclog::tagWarn(TAG, "Invalid initialize response");
        return false;
    }

    cJSON* result = cJSON_GetObjectItem(j, "result");
    if (result) {
        cJSON* pv = cJSON_GetObjectItem(result, "protocolVersion");
        if (pv && pv->valuestring) {
            mclog::tagInfo(TAG, "MCP server protocol: {}", pv->valuestring);
        }
        cJSON* info = cJSON_GetObjectItem(result, "serverInfo");
        if (info) {
            cJSON* name = cJSON_GetObjectItem(info, "name");
            if (name && name->valuestring) {
                mclog::tagInfo(TAG, "MCP server: {}", name->valuestring);
            }
        }
    }
    cJSON_Delete(j);

    sendJsonRpc("notifications/initialized", "{}", response, 5000);

    _initialized = true;
    refreshTools();
    return true;
}

void McpClient::refreshTools() {
    std::string response;
    if (!sendJsonRpc("tools/list", "{}", response, 10000)) {
        mclog::tagWarn(TAG, "tools/list failed");
        return;
    }

    cJSON* j = cJSON_Parse(response.c_str());
    if (!j) return;

    _cached_tools.clear();
    cJSON* result = cJSON_GetObjectItem(j, "result");
    if (result) {
        cJSON* tools = cJSON_GetObjectItem(result, "tools");
        if (cJSON_IsArray(tools)) {
            for (int i = 0; i < cJSON_GetArraySize(tools); i++) {
                cJSON* t = cJSON_GetArrayItem(tools, i);
                McpTool tool;
                cJSON* name = cJSON_GetObjectItem(t, "name");
                if (name && name->valuestring) tool.name = name->valuestring;
                cJSON* desc = cJSON_GetObjectItem(t, "description");
                if (desc && desc->valuestring) tool.description = desc->valuestring;
                cJSON* schema = cJSON_GetObjectItem(t, "inputSchema");
                if (schema) {
                    char* s = cJSON_PrintUnformatted(schema);
                    if (s) { tool.input_schema = s; free(s); }
                }
                _cached_tools.push_back(tool);
                mclog::tagInfo(TAG, "Tool: {} - {}", tool.name, tool.description);
            }
        }
    }
    cJSON_Delete(j);

    if (_on_tools_updated) _on_tools_updated(_cached_tools);
    mclog::tagInfo(TAG, "Found {} tools", _cached_tools.size());
}

bool McpClient::connect(const std::string& url) {
    _server_url = url;
    _connected = false;
    _initialized = false;

    mclog::tagInfo(TAG, "Connecting to: {}", url);

    if (initialize()) {
        _connected = true;
        mclog::tagInfo(TAG, "Connected with {} tools", _cached_tools.size());
        return true;
    }

    mclog::tagWarn(TAG, "Connection failed");
    return false;
}

void McpClient::disconnect() {
    _connected = false;
    _initialized = false;
    _server_url.clear();
    _cached_tools.clear();
    mclog::tagInfo(TAG, "Disconnected");
}

std::vector<McpTool> McpClient::listTools() {
    if (_connected && _initialized) {
        refreshTools();
    }
    return _cached_tools;
}

McpToolResult McpClient::callTool(const std::string& name, const std::string& args) {
    McpToolResult result;
    result.success = false;

    if (!_connected || !_initialized) {
        result.error = "MCP未连接";
        return result;
    }

    std::string params = "{\"name\":\"" + name + "\",\"arguments\":" + args + "}";
    std::string response;

    if (!sendJsonRpc("tools/call", params, response, 30000)) {
        result.error = "调用失败";
        return result;
    }

    cJSON* j = cJSON_Parse(response.c_str());
    if (!j) {
        result.error = "响应解析失败";
        return result;
    }

    cJSON* res = cJSON_GetObjectItem(j, "result");
    if (res) {
        cJSON* content = cJSON_GetObjectItem(res, "content");
        if (cJSON_IsArray(content)) {
            std::string combined;
            for (int i = 0; i < cJSON_GetArraySize(content); i++) {
                cJSON* item = cJSON_GetArrayItem(content, i);
                cJSON* text = cJSON_GetObjectItem(item, "text");
                if (text && text->valuestring) {
                    if (!combined.empty()) combined += "\n";
                    combined += text->valuestring;
                }
            }
            result.content = combined;
            result.success = true;
        } else {
            char* r = cJSON_PrintUnformatted(res);
            if (r) { result.content = r; free(r); }
            result.success = true;
        }
    }

    cJSON* err = cJSON_GetObjectItem(j, "error");
    if (err) {
        cJSON* msg = cJSON_GetObjectItem(err, "message");
        if (msg && msg->valuestring) result.error = msg->valuestring;
        else result.error = "未知错误";
    }

    cJSON_Delete(j);
    mclog::tagInfo(TAG, "Tool {} result: {} bytes ok={}", name, result.content.size(), result.success);
    return result;
}

#pragma once
#include <string>
#include <vector>

struct AssistantConfig {
    std::string name = "AI助手";
    std::string avatar = "";
    double temperature = 0.7;
    std::string model_id = "deepseek-chat";
    int max_context_messages = 0;
    int max_tokens = 4096;
    bool streaming = true;
    bool show_thinking = true;
    bool auto_collapse_thinking = true;
    bool latex_rendering = true;
    bool code_block_highlight = true;
    bool auto_scroll = true;
    std::string system_prompt = "";
    std::string user_name = "用户";
    std::string language = "zh-CN";
};

struct ToolConfig {
    bool ask_user = true;
    bool get_time = true;
    bool run_js = false;
    bool device_info = true;
    bool battery_info = true;
    bool web_search = false;
};

class ConfigStore {
public:
    static ConfigStore& getInstance();

    void init();
    void save();
    void load();

    void setAssistantConfig(const AssistantConfig& cfg);
    const AssistantConfig& getAssistantConfig() const { return _assistant; }

    void setApiKey(const std::string& key);
    std::string getApiKey() const { return _api_key; }

    void setApiUrl(const std::string& url);
    std::string getApiUrl() const { return _api_url; }

    void setProvider(const std::string& p);
    std::string getProvider() const { return _provider; }

    void setSttEnabled(bool e);
    bool isSttEnabled() const { return _stt_enabled; }

    void setTtsEnabled(bool e);
    bool isTtsEnabled() const { return _tts_enabled; }

    void setSttModelId(const std::string& id);
    std::string getSttModelId() const { return _stt_model_id; }

    void setTtsModelId(const std::string& id);
    std::string getTtsModelId() const { return _tts_model_id; }

    void setSttUrl(const std::string& url);
    std::string getSttUrl() const { return _stt_url; }

    void setTtsUrl(const std::string& url);
    std::string getTtsUrl() const { return _tts_url; }

    void setWifiSsid(const std::string& s);
    std::string getWifiSsid() const { return _wifi_ssid; }

    void setWifiPassword(const std::string& p);
    std::string getWifiPassword() const { return _wifi_password; }

    void setWifiMode(int m);
    int getWifiMode() const { return _wifi_mode; }

    void setApSsid(const std::string& s);
    std::string getApSsid() const { return _ap_ssid; }

    void setApPassword(const std::string& p);
    std::string getApPassword() const { return _ap_password; }

    void setMcpUrl(const std::string& url);
    std::string getMcpUrl() const { return _mcp_url; }

    void setMcpEnabled(bool e);
    bool isMcpEnabled() const { return _mcp_enabled; }

    void setBingApiKey(const std::string& key);
    std::string getBingApiKey() const { return _bing_api_key; }

    void setTavilyApiKey(const std::string& key);
    std::string getTavilyApiKey() const { return _tavily_api_key; }

    void setToolConfig(const ToolConfig& cfg);
    const ToolConfig& getToolConfig() const { return _tool_config; }

    bool isConfigured() const { return !_api_key.empty(); }
    bool isFirstBootCompleted() const { return _first_boot_done; }
    bool isFirstBootDone() const { return _first_boot_done; }
    void markFirstBootDone();

private:
    ConfigStore() = default;
    static const char* NS;

    AssistantConfig _assistant;
    std::string _api_key;
    std::string _api_url = "https://api.deepseek.com";
    std::string _provider = "deepseek";
    bool _stt_enabled = false;
    bool _tts_enabled = false;
    std::string _stt_model_id;
    std::string _tts_model_id;
    std::string _stt_url;
    std::string _tts_url;
    std::string _wifi_ssid;
    std::string _wifi_password;
    int _wifi_mode = 0;
    std::string _ap_ssid = "CardputerADV";
    std::string _ap_password = "12345678";
    std::string _mcp_url;
    bool _mcp_enabled = false;
    std::string _bing_api_key;
    std::string _tavily_api_key;
    ToolConfig _tool_config;
    bool _first_boot_done = false;
};

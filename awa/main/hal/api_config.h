#pragma once
#include <string>

enum class ApiProvider {
    DEEPSEEK,
    OPENAI,
    CUSTOM
};

struct ApiConfigData {
    bool enabled = false;
    ApiProvider provider = ApiProvider::DEEPSEEK;
    std::string api_key;
    std::string base_url = "https://api.deepseek.com";
    std::string default_model = "deepseek-chat";
};

class ApiConfig {
public:
    static ApiConfig& getInstance();

    void init();
    void load();
    void save();

    void setConfig(const ApiConfigData& cfg);
    const ApiConfigData& getConfig() const { return _config; }

    bool isConfigured() const { return _config.enabled && !_config.api_key.empty(); }
    std::string getApiKey() const { return _config.api_key; }
    std::string getBaseUrl() const { return _config.base_url; }
    std::string getDefaultModel() const { return _config.default_model; }
    ApiProvider getProvider() const { return _config.provider; }

private:
    ApiConfig() = default;
    ~ApiConfig() = default;
    ApiConfig(const ApiConfig&) = delete;

    ApiConfigData _config;
};

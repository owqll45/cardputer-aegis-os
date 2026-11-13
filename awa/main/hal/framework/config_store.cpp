#include "config_store.h"
#include "nvs_helper.h"
#include <mooncake_log.h>

static const std::string TAG = "ConfigStore";
const char* ConfigStore::NS = "asst_cfg";

ConfigStore& ConfigStore::getInstance() {
    static ConfigStore instance;
    return instance;
}

void ConfigStore::init() { load(); }

void ConfigStore::load() {
    NvsHelper::getStr(NS, "name", _assistant.name);
    NvsHelper::getStr(NS, "avatar", _assistant.avatar);
    int tmp;
    if (NvsHelper::getInt(NS, "temperature_x100", tmp, 70)) _assistant.temperature = tmp / 100.0;
    NvsHelper::getStr(NS, "model_id", _assistant.model_id);
    NvsHelper::getInt(NS, "max_context", _assistant.max_context_messages, 0);
    NvsHelper::getInt(NS, "max_tokens", _assistant.max_tokens, 4096);
    _assistant.streaming = NvsHelper::getBool(NS, "streaming", true);
    _assistant.show_thinking = NvsHelper::getBool(NS, "show_thinking", true);
    _assistant.auto_collapse_thinking = NvsHelper::getBool(NS, "auto_collapse", true);
    _assistant.latex_rendering = NvsHelper::getBool(NS, "latex", true);
    _assistant.code_block_highlight = NvsHelper::getBool(NS, "code_hl", true);
    _assistant.auto_scroll = NvsHelper::getBool(NS, "auto_scroll", true);
    NvsHelper::getStr(NS, "system_prompt", _assistant.system_prompt);
    NvsHelper::getStr(NS, "user_name", _assistant.user_name);
    NvsHelper::getStr(NS, "language", _assistant.language);

    NvsHelper::getStr(NS, "api_key", _api_key, 2048);
    NvsHelper::getStr(NS, "api_url", _api_url, 512);
    NvsHelper::getStr(NS, "provider", _provider, 64);

    _stt_enabled = NvsHelper::getBool(NS, "stt_en", false);
    _tts_enabled = NvsHelper::getBool(NS, "tts_en", false);
    NvsHelper::getStr(NS, "stt_model", _stt_model_id);
    NvsHelper::getStr(NS, "tts_model", _tts_model_id);
    NvsHelper::getStr(NS, "stt_url", _stt_url);
    NvsHelper::getStr(NS, "tts_url", _tts_url);

    NvsHelper::getStr(NS, "wifi_ssid", _wifi_ssid);
    NvsHelper::getStr(NS, "wifi_pwd", _wifi_password);
    NvsHelper::getInt(NS, "wifi_mode", _wifi_mode, 0);
    NvsHelper::getStr(NS, "ap_ssid", _ap_ssid);
    NvsHelper::getStr(NS, "ap_pwd", _ap_password);

    NvsHelper::getStr(NS, "mcp_url", _mcp_url);
    _mcp_enabled = NvsHelper::getBool(NS, "mcp_en", false);

    NvsHelper::getStr(NS, "bing_key", _bing_api_key);
    NvsHelper::getStr(NS, "tavily_key", _tavily_api_key);

    _tool_config.ask_user = NvsHelper::getBool(NS, "tool_ask", true);
    _tool_config.get_time = NvsHelper::getBool(NS, "tool_time", true);
    _tool_config.run_js = NvsHelper::getBool(NS, "tool_js", false);
    _tool_config.device_info = NvsHelper::getBool(NS, "tool_dev", true);
    _tool_config.battery_info = NvsHelper::getBool(NS, "tool_bat", true);
    _tool_config.web_search = NvsHelper::getBool(NS, "tool_search", false);

    _first_boot_done = NvsHelper::getBool(NS, "first_boot", false);

    mclog::tagInfo(TAG, "Config loaded: provider={} model={} wifi_mode={}", _provider, _assistant.model_id, _wifi_mode);
}

void ConfigStore::save() {
    NvsHelper::setStr(NS, "name", _assistant.name);
    NvsHelper::setStr(NS, "avatar", _assistant.avatar);
    NvsHelper::setInt(NS, "temperature_x100", (int)(_assistant.temperature * 100));
    NvsHelper::setStr(NS, "model_id", _assistant.model_id);
    NvsHelper::setInt(NS, "max_context", _assistant.max_context_messages);
    NvsHelper::setInt(NS, "max_tokens", _assistant.max_tokens);
    NvsHelper::setBool(NS, "streaming", _assistant.streaming);
    NvsHelper::setBool(NS, "show_thinking", _assistant.show_thinking);
    NvsHelper::setBool(NS, "auto_collapse", _assistant.auto_collapse_thinking);
    NvsHelper::setBool(NS, "latex", _assistant.latex_rendering);
    NvsHelper::setBool(NS, "code_hl", _assistant.code_block_highlight);
    NvsHelper::setBool(NS, "auto_scroll", _assistant.auto_scroll);
    NvsHelper::setStr(NS, "system_prompt", _assistant.system_prompt);
    NvsHelper::setStr(NS, "user_name", _assistant.user_name);
    NvsHelper::setStr(NS, "language", _assistant.language);

    NvsHelper::setStr(NS, "api_key", _api_key);
    NvsHelper::setStr(NS, "api_url", _api_url);
    NvsHelper::setStr(NS, "provider", _provider);

    NvsHelper::setBool(NS, "stt_en", _stt_enabled);
    NvsHelper::setBool(NS, "tts_en", _tts_enabled);
    NvsHelper::setStr(NS, "stt_model", _stt_model_id);
    NvsHelper::setStr(NS, "tts_model", _tts_model_id);
    NvsHelper::setStr(NS, "stt_url", _stt_url);
    NvsHelper::setStr(NS, "tts_url", _tts_url);

    NvsHelper::setStr(NS, "wifi_ssid", _wifi_ssid);
    NvsHelper::setStr(NS, "wifi_pwd", _wifi_password);
    NvsHelper::setInt(NS, "wifi_mode", _wifi_mode);
    NvsHelper::setStr(NS, "ap_ssid", _ap_ssid);
    NvsHelper::setStr(NS, "ap_pwd", _ap_password);

    NvsHelper::setStr(NS, "mcp_url", _mcp_url);
    NvsHelper::setBool(NS, "mcp_en", _mcp_enabled);

    NvsHelper::setStr(NS, "bing_key", _bing_api_key);
    NvsHelper::setStr(NS, "tavily_key", _tavily_api_key);

    NvsHelper::setBool(NS, "tool_ask", _tool_config.ask_user);
    NvsHelper::setBool(NS, "tool_time", _tool_config.get_time);
    NvsHelper::setBool(NS, "tool_js", _tool_config.run_js);
    NvsHelper::setBool(NS, "tool_dev", _tool_config.device_info);
    NvsHelper::setBool(NS, "tool_bat", _tool_config.battery_info);
    NvsHelper::setBool(NS, "tool_search", _tool_config.web_search);

    NvsHelper::setBool(NS, "first_boot", _first_boot_done);
    mclog::tagInfo(TAG, "Config saved");
}

void ConfigStore::setAssistantConfig(const AssistantConfig& cfg) { _assistant = cfg; save(); }
void ConfigStore::setApiKey(const std::string& k) { _api_key = k; save(); }
void ConfigStore::setApiUrl(const std::string& u) { _api_url = u; save(); }
void ConfigStore::setProvider(const std::string& p) { _provider = p; save(); }
void ConfigStore::setSttEnabled(bool e) { _stt_enabled = e; save(); }
void ConfigStore::setTtsEnabled(bool e) { _tts_enabled = e; save(); }
void ConfigStore::setSttModelId(const std::string& id) { _stt_model_id = id; save(); }
void ConfigStore::setTtsModelId(const std::string& id) { _tts_model_id = id; save(); }
void ConfigStore::setSttUrl(const std::string& u) { _stt_url = u; save(); }
void ConfigStore::setTtsUrl(const std::string& u) { _tts_url = u; save(); }
void ConfigStore::setWifiSsid(const std::string& s) { _wifi_ssid = s; save(); }
void ConfigStore::setWifiPassword(const std::string& p) { _wifi_password = p; save(); }
void ConfigStore::setWifiMode(int m) { _wifi_mode = m; save(); }
void ConfigStore::setApSsid(const std::string& s) { _ap_ssid = s; save(); }
void ConfigStore::setApPassword(const std::string& p) { _ap_password = p; save(); }
void ConfigStore::setMcpUrl(const std::string& url) { _mcp_url = url; save(); }
void ConfigStore::setMcpEnabled(bool e) { _mcp_enabled = e; save(); }
void ConfigStore::setBingApiKey(const std::string& key) { _bing_api_key = key; save(); }
void ConfigStore::setTavilyApiKey(const std::string& key) { _tavily_api_key = key; save(); }
void ConfigStore::setToolConfig(const ToolConfig& cfg) { _tool_config = cfg; save(); }
void ConfigStore::markFirstBootDone() { _first_boot_done = true; save(); }

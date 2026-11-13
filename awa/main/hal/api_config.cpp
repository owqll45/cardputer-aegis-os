#include "api_config.h"
#include <mooncake_log.h>
#include <nvs_flash.h>
#include <cstring>

static const std::string TAG = "ApiConfig";
static const std::string NS = "api_cfg";

ApiConfig& ApiConfig::getInstance() {
    static ApiConfig instance;
    return instance;
}

void ApiConfig::init() {
    mclog::tagInfo(TAG, "init");
    load();
}

void ApiConfig::load() {
    nvs_handle_t h;
    if (nvs_open(NS.c_str(), NVS_READONLY, &h) != ESP_OK) {
        mclog::tagInfo(TAG, "No saved config found");
        return;
    }

    uint8_t enabled = 0, provider = 0;
    size_t len;

    nvs_get_u8(h, "en", &enabled);
    nvs_get_u8(h, "prov", &provider);

    _config.enabled = (enabled == 1);
    _config.provider = (ApiProvider)provider;

    char buf[256];
    len = sizeof(buf);
    if (nvs_get_str(h, "key", buf, &len) == ESP_OK) _config.api_key = buf;

    len = sizeof(buf);
    if (nvs_get_str(h, "url", buf, &len) == ESP_OK) _config.base_url = buf;

    len = sizeof(buf);
    if (nvs_get_str(h, "model", buf, &len) == ESP_OK) _config.default_model = buf;

    nvs_close(h);
    mclog::tagInfo(TAG, "Loaded: provider={} model={}",
                   (int)_config.provider, _config.default_model);
}

void ApiConfig::save() {
    nvs_handle_t h;
    if (nvs_open(NS.c_str(), NVS_READWRITE, &h) != ESP_OK) {
        mclog::tagError(TAG, "Failed to open NVS for writing");
        return;
    }

    nvs_set_u8(h, "en", _config.enabled ? 1 : 0);
    nvs_set_u8(h, "prov", (uint8_t)_config.provider);
    nvs_set_str(h, "key", _config.api_key.c_str());
    nvs_set_str(h, "url", _config.base_url.c_str());
    nvs_set_str(h, "model", _config.default_model.c_str());

    nvs_commit(h);
    nvs_close(h);
    mclog::tagInfo(TAG, "Saved to NVS");
}

void ApiConfig::setConfig(const ApiConfigData& cfg) {
    _config = cfg;
    save();
}

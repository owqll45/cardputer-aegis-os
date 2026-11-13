#include "settings.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <cJSON.h>

#define TAG "Settings"
#define SD_BASE_PATH "/sdcard/system"

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write)
{
    nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
    loadFromSD();
}

Settings::~Settings()
{
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            ESP_ERROR_CHECK(nvs_commit(nvs_handle_));
            saveToSD();
        }
        nvs_close(nvs_handle_);
    }
}

std::string Settings::getSDPath() {
    return std::string(SD_BASE_PATH) + "/" + ns_ + ".json";
}

void Settings::loadFromSD() {
    std::string path = getSDPath();
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        ESP_LOGI(TAG, "SD settings not found: %s", path.c_str());
        return;
    }

    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1);
    fread(buffer, 1, size, f);
    buffer[size] = 0;
    fclose(f);

    cJSON* json = cJSON_Parse(buffer);
    if (json) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, json) {
            if (cJSON_IsString(item)) {
                sd_settings_[item->string] = item->valuestring;
            } else if (cJSON_IsNumber(item)) {
                sd_settings_[item->string] = std::to_string((int)item->valuedouble);
            } else if (cJSON_IsBool(item)) {
                sd_settings_[item->string] = cJSON_IsTrue(item) ? "1" : "0";
            }
        }
        cJSON_Delete(json);
        ESP_LOGI(TAG, "Loaded settings from SD: %s", path.c_str());
    }
    free(buffer);
}

void Settings::saveToSD() {
    if (!read_write_) return;

    std::string path = getSDPath();
    cJSON* json = cJSON_CreateObject();
    for (auto const& [key, val] : sd_settings_) {
        cJSON_AddStringToObject(json, key.c_str(), val.c_str());
    }

    char* buffer = cJSON_Print(json);
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fputs(buffer, f);
        fclose(f);
        ESP_LOGI(TAG, "Saved settings to SD: %s", path.c_str());
    }
    cJSON_free(buffer);
    cJSON_Delete(json);
}

void Settings::Sync() {
    if (read_write_ && dirty_) {
        if (nvs_handle_) nvs_commit(nvs_handle_);
        saveToSD();
        dirty_ = false;
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value)
{
    // Try SD first
    if (sd_settings_.count(key)) {
        return sd_settings_[key];
    }

    if (nvs_handle_ == 0) {
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }

    std::string value;
    value.resize(length);
    ESP_ERROR_CHECK(nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length));
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    
    // Cache to SD settings
    sd_settings_[key] = value;
    return value;
}

void Settings::SetString(const std::string& key, const std::string& value)
{
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle_, key.c_str(), value.c_str()));
        sd_settings_[key] = value;
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value)
{
    // Try SD first
    if (sd_settings_.count(key)) {
        return atoi(sd_settings_[key].c_str());
    }

    if (nvs_handle_ == 0) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    
    // Cache to SD settings
    sd_settings_[key] = std::to_string(value);
    return value;
}

void Settings::SetInt(const std::string& key, int32_t value)
{
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_set_i32(nvs_handle_, key.c_str(), value));
        sd_settings_[key] = std::to_string(value);
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

bool Settings::GetBool(const std::string& key, bool default_value)
{
    // Try SD first
    if (sd_settings_.count(key)) {
        return sd_settings_[key] == "1";
    }

    if (nvs_handle_ == 0) {
        return default_value;
    }

    uint8_t value;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    
    // Cache to SD settings
    sd_settings_[key] = value ? "1" : "0";
    return value != 0;
}

void Settings::SetBool(const std::string& key, bool value)
{
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0));
        sd_settings_[key] = value ? "1" : "0";
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseKey(const std::string& key)
{
    if (read_write_) {
        auto ret = nvs_erase_key(nvs_handle_, key.c_str());
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(ret);
        }
        sd_settings_.erase(key);
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseAll()
{
    if (read_write_) {
        ESP_ERROR_CHECK(nvs_erase_all(nvs_handle_));
        sd_settings_.clear();
        dirty_ = true;
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}
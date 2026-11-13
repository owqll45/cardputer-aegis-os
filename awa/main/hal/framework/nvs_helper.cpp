#include "nvs_helper.h"
#include <nvs_flash.h>
#include <cstring>
#include <vector>

bool NvsHelper::exists(const char* ns, const char* key) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_get_blob(h, key, nullptr, &len);
    }
    nvs_close(h);
    return err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH;
}

bool NvsHelper::getStr(const char* ns, const char* key, std::string& out, size_t max_len) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &len);
    if (err != ESP_OK || len == 0) { nvs_close(h); return false; }
    if (len > max_len) { nvs_close(h); return false; }
    std::vector<char> buf(len);
    err = nvs_get_str(h, key, buf.data(), &len);
    if (err == ESP_OK) out = std::string(buf.data());
    nvs_close(h);
    return err == ESP_OK;
}

bool NvsHelper::setStr(const char* ns, const char* key, const std::string& val) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, key, val.c_str());
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool NvsHelper::getInt(const char* ns, const char* key, int& out, int def) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) { out = def; return false; }
    int32_t v = def;
    esp_err_t err = nvs_get_i32(h, key, &v);
    if (err == ESP_OK) out = (int)v;
    else out = def;
    nvs_close(h);
    return err == ESP_OK;
}

bool NvsHelper::setInt(const char* ns, const char* key, int val) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_i32(h, key, (int32_t)val);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool NvsHelper::getBool(const char* ns, const char* key, bool def) {
    int v = def ? 1 : 0;
    getInt(ns, key, v, def ? 1 : 0);
    return v != 0;
}

bool NvsHelper::setBool(const char* ns, const char* key, bool val) {
    return setInt(ns, key, val ? 1 : 0);
}

bool NvsHelper::getBlob(const char* ns, const char* key, uint8_t* buf, size_t* len) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return err == ESP_OK;
}

bool NvsHelper::setBlob(const char* ns, const char* key, const uint8_t* buf, size_t len) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool NvsHelper::erase(const char* ns, const char* key) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool NvsHelper::eraseAll(const char* ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return true;
}

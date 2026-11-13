#pragma once
#include <string>
#include <functional>
#include <vector>
#include <esp_err.h>

class NvsHelper {
public:
    static bool exists(const char* ns, const char* key);
    static bool getStr(const char* ns, const char* key, std::string& out, size_t max_len = 512);
    static bool setStr(const char* ns, const char* key, const std::string& val);
    static bool getInt(const char* ns, const char* key, int& out, int def = 0);
    static bool setInt(const char* ns, const char* key, int val);
    static bool getBool(const char* ns, const char* key, bool def = false);
    static bool setBool(const char* ns, const char* key, bool val);
    static bool getBlob(const char* ns, const char* key, uint8_t* buf, size_t* len);
    static bool setBlob(const char* ns, const char* key, const uint8_t* buf, size_t len);
    static bool erase(const char* ns, const char* key);
    static bool eraseAll(const char* ns);
};

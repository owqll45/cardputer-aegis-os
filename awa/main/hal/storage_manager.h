#pragma once
#include <string>
#include <vector>

class StorageManager {
public:
    struct SdCardProbeResult_t {
        bool is_mounted = false;
        std::string size;
        std::string type;
        std::string name;

        bool operator==(const SdCardProbeResult_t& other) const
        {
            return is_mounted == other.is_mounted && size == other.size && type == other.type && name == other.name;
        }
    };

    StorageManager();
    ~StorageManager();

    void init();
    void update();

    SdCardProbeResult_t sdCardProbe();
    void sd_card_init();
    void spiffs_init();

    /* Memory & Swap */
    void initSdSwap();
    void purgeMemory(bool kill_system_apps = false);
    void* requestSwapBuffer(size_t size);
    void freeSwapBuffer(void* ptr);
    bool loadAppFromSd(const std::string& path);

    /* Virtual RAM / Swap */
    void pushHrHistoryToSd(int hr);
    std::vector<int> getHrHistoryFromSd();

private:
    bool _is_sd_card_mounted = false;
};

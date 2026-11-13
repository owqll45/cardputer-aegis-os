#include "sd_card_manager.h"
#include <mooncake_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/spi_common.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

static const std::string TAG = "SdCard";
static bool _spi_bus_initialized = false;

SdCardManager& SdCardManager::getInstance() {
    static SdCardManager instance;
    return instance;
}

void SdCardManager::init() {
    mclog::tagInfo(TAG, "Initializing SD card...");

    esp_err_t ret;

    if (!_spi_bus_initialized) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;

        spi_bus_config_t bus_cfg = {};
        bus_cfg.mosi_io_num = GPIO_NUM_14;
        bus_cfg.miso_io_num = GPIO_NUM_39;
        bus_cfg.sclk_io_num = GPIO_NUM_40;
        bus_cfg.quadwp_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.max_transfer_sz = 4000;

        ret = spi_bus_initialize(spi_host_device_t(host.slot), &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret == ESP_OK) {
            _spi_bus_initialized = true;
            mclog::tagInfo(TAG, "SPI bus initialized for SD card");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            mclog::tagInfo(TAG, "SPI bus already initialized by display, reusing");
            _spi_bus_initialized = true;
        } else {
            mclog::tagError(TAG, "SPI bus init failed: {}", (int)ret);
            _state = SdState::ERROR;
            return;
        }
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = GPIO_NUM_12;
    slot_cfg.host_id = spi_host_device_t(host.slot);

    sdmmc_card_t* card = nullptr;
    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &card);

    if (ret == ESP_OK) {
        _state = SdState::MOUNTED; ensureDirectories();
        mclog::tagInfo(TAG, "SD card mounted OK");
    } else if (ret == ESP_ERR_NOT_FINISHED) {
        _state = SdState::NEEDS_FORMAT;
        mclog::tagWarn(TAG, "SD card needs format");
    } else {
        _state = SdState::ERROR;
        mclog::tagError(TAG, "SD mount failed: {}", (int)ret);
    }
}

void SdCardManager::update() {}
uint64_t SdCardManager::getTotalSpace() const { return 0; }
uint64_t SdCardManager::getFreeSpace() const { return 0; }

bool SdCardManager::ensureDirectories() {
    const char* dirs[] = { "/sdcard", "/sdcard/system", "/sdcard/conversations",
                           "/sdcard/memory", "/sdcard/tools", "/sdcard/cache", nullptr };
    for (int i = 0; dirs[i]; i++) mkdir(dirs[i], 0755);
    return true;
}

bool SdCardManager::writeFile(const std::string& path, const uint8_t* data, size_t len) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    fwrite(data, 1, len, f); fclose(f); return true;
}

bool SdCardManager::readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(sz); fread(out.data(), 1, sz, f); fclose(f); return true;
}

bool SdCardManager::fileExists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }
bool SdCardManager::deleteFile(const std::string& path) { return remove(path.c_str()) == 0; }

std::vector<std::string> SdCardManager::listFiles(const std::string& dir) {
    std::vector<std::string> result;
    DIR* d = opendir(dir.c_str()); if (!d) return result;
    struct dirent* ent;
    while ((ent = readdir(d))) { if (ent->d_name[0] != '.') result.push_back(ent->d_name); }
    closedir(d); sort(result.begin(), result.end()); return result;
}

FormatResult SdCardManager::formatCard() {
    if (_on_format_prompt && !_on_format_prompt()) return FormatResult::CANCELLED;

    mclog::tagInfo(TAG, "Formatting SD card...");

    if (_state == SdState::MOUNTED) {
        esp_vfs_fat_sdcard_unmount("/sdcard", nullptr);
        mclog::tagInfo(TAG, "Unmounted SD card before format");
        _state = SdState::NOT_MOUNTED;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = GPIO_NUM_12;
    slot_cfg.host_id = spi_host_device_t(host.slot);

    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = true;
    mount_cfg.max_files = 5;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &card);

    if (ret == ESP_OK) {
        _state = SdState::MOUNTED; ensureDirectories();
        mclog::tagInfo(TAG, "SD card formatted and mounted OK");
        return FormatResult::SUCCESS;
    }

    mclog::tagError(TAG, "SD format failed: {}", (int)ret);
    _state = SdState::ERROR;
    return FormatResult::FAILED;
}

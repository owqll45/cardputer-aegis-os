#include "storage_manager.h"
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <esp_vfs_fat.h>
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <esp_spiffs.h>

static const std::string _tag = "Storage";

/* SD Card Globals */
static sdmmc_card_t* _sd_card      = nullptr;
static const char* _sd_mount_point = "/sdcard";

#define MOUNT_POINT "/sdcard"

StorageManager::StorageManager()
{
}

StorageManager::~StorageManager()
{
}

void StorageManager::init()
{
    mclog::tagInfo(_tag, "storage init");
}

void StorageManager::update()
{
}

StorageManager::SdCardProbeResult_t StorageManager::sdCardProbe()
{
    SdCardProbeResult_t result;

    if (!_is_sd_card_mounted) {
        sd_card_init();
        if (!_is_sd_card_mounted) {
            result.is_mounted = false;
            result.size       = "Not Found";
            return result;
        }
    }

    result.is_mounted = true;

    /* Try write to sd card */
    FILE* fp = fopen(MOUNT_POINT "/test.txt", "w");
    if (fp) {
        fwrite("Hello, World!", 1, 13, fp);
        fclose(fp);

        char size_buf[64];
        snprintf(size_buf, sizeof(size_buf), "Size: %.1f GB",
                 ((float)((uint64_t)_sd_card->csd.capacity) * _sd_card->csd.sector_size) / (1024 * 1024 * 1024));
        result.size = std::string(size_buf);
    } else {
        result.size = "Write Failed";
    }

    result.type = "Type: ";
    if (_sd_card->is_sdio) {
        result.type += "SDIO";
    } else if (_sd_card->is_mmc) {
        result.type += "MMC";
    } else {
        result.type += (_sd_card->ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC";
    }

    char name_buf[32];
    snprintf(name_buf, sizeof(name_buf), "Name: %s", std::string(_sd_card->cid.name).c_str());
    result.name = std::string(name_buf);

    return result;
}

void StorageManager::sd_card_init()
{
    mclog::tagInfo(_tag, "SD init via VFS");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024};

    sdmmc_host_t host        = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = GPIO_NUM_14,
        .miso_io_num     = GPIO_NUM_39,
        .sclk_io_num     = GPIO_NUM_40,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "Failed to initialize SPI bus");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = GPIO_NUM_12;
    slot_config.host_id               = (spi_host_device_t)host.slot;

    ret = esp_vfs_fat_sdspi_mount(_sd_mount_point, &host, &slot_config, &mount_config, &_sd_card);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "Failed to mount SD card");
        return;
    }

    mclog::tagInfo(_tag, "SD mounted at {}", _sd_mount_point);
    _is_sd_card_mounted = true;

    /* Initialize Swap */
    initSdSwap();
}

void StorageManager::spiffs_init()
{
    mclog::tagInfo(_tag, "Mounting Internal Flash Storage...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/internal", .partition_label = "storage", .max_files = 5, .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            mclog::tagError(_tag, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            mclog::tagError(_tag, "Failed to find SPIFFS partition");
        } else {
            mclog::tagError(_tag, "Failed to initialize SPIFFS ({})", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "Failed to get SPIFFS partition information ({})", esp_err_to_name(ret));
    } else {
        mclog::tagInfo(_tag, "Partition size: total: {}, used: {}", total, used);
    }
}

/* Memory & Swap */
void StorageManager::initSdSwap()
{
    mclog::tagInfo(_tag, "init SD swap");
    /* Implement SD swap initialization */
}

void StorageManager::purgeMemory(bool kill_system_apps)
{
    mclog::tagInfo(_tag, "purge memory");
    /* Implement memory purge logic */
}

void* StorageManager::requestSwapBuffer(size_t size)
{
    /* Implement swap buffer request logic */
    return nullptr;
}

void StorageManager::freeSwapBuffer(void* ptr)
{
    /* Implement swap buffer free logic */
}

bool StorageManager::loadAppFromSd(const std::string& path)
{
    mclog::tagInfo(_tag, "load app from sd: {}", path);
    /* Implement app loading from SD card */
    return true;
}

void StorageManager::pushHrHistoryToSd(int hr)
{
    /* Implement HR history push to SD */
}

std::vector<int> StorageManager::getHrHistoryFromSd()
{
    /* Implement HR history retrieval from SD */
    return std::vector<int>();
}

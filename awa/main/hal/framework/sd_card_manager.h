#pragma once
#include <string>
#include <functional>

enum class SdState { NOT_MOUNTED, MOUNTED, ERROR, NEEDS_FORMAT };
enum class FormatResult { SUCCESS, FAILED, CANCELLED };

class SdCardManager {
public:
    static SdCardManager& getInstance();

    void init();
    void update();

    SdState getState() const { return _state; }
    bool isReady() const { return _state == SdState::MOUNTED; }
    std::string getMountPath() const { return "/sdcard"; }
    uint64_t getTotalSpace() const;
    uint64_t getFreeSpace() const;

    bool ensureDirectories();
    bool writeFile(const std::string& path, const uint8_t* data, size_t len);
    bool readFile(const std::string& path, std::vector<uint8_t>& out);
    bool fileExists(const std::string& path);
    bool deleteFile(const std::string& path);
    std::vector<std::string> listFiles(const std::string& dir);

    void setOnFormatPrompt(std::function<bool()> cb) { _on_format_prompt = cb; }
    FormatResult formatCard();

private:
    SdCardManager() = default;
    SdState _state = SdState::NOT_MOUNTED;
    std::function<bool()> _on_format_prompt;
};

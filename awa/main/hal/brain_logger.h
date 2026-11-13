#pragma once
#include <string>
#include <mutex>

namespace brain_logger {

    class BrainLogger {
    public:
        static BrainLogger& getInstance();

        bool init(const std::string& mount_point = "/sdcard");
        void deinit();
        void logFrame(const std::string& json_line);
        void logEvent(const std::string& tag, const std::string& message);

    private:
        BrainLogger() = default;
        std::string _log_path;
        std::mutex _log_mutex;
        bool _inited = false;
    };

} // namespace brain_logger

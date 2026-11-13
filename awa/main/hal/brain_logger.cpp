#include "brain_logger.h"
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <mooncake_log.h>

namespace brain_logger {

    BrainLogger& BrainLogger::getInstance() {
        static BrainLogger instance;
        return instance;
    }

    bool BrainLogger::init(const std::string& mount_point) {
        std::lock_guard<std::mutex> lock(_log_mutex);
        
        // Create directory if not exists
        std::string dir = mount_point + "/sys/brain_core";
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            mkdir(dir.c_str(), 0777);
        }

        // Generate log filename based on time
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char buf[64];
        strftime(buf, sizeof(buf), "/raw_stream_%Y%m%d_%H%M.jsonl", tm_info);
        _log_path = dir + buf;

        _inited = true;
        mclog::tagInfo("BrainLogger", "Log file initialized: {}", _log_path);
        return true;
    }

    void BrainLogger::deinit() {
        std::lock_guard<std::mutex> lock(_log_mutex);
        _inited = false;
        mclog::tagInfo("BrainLogger", "Log service stopped.");
    }

    void BrainLogger::logFrame(const std::string& json_line) {
        if (!_inited) return;
        std::lock_guard<std::mutex> lock(_log_mutex);
        
        FILE* f = fopen(_log_path.c_str(), "a");
        if (f) {
            fprintf(f, "%s\n", json_line.c_str());
            fclose(f);
        }
    }

    void BrainLogger::logEvent(const std::string& tag, const std::string& message) {
        if (!_inited) return;
        std::lock_guard<std::mutex> lock(_log_mutex);
        
        // Also log to a general event file
        std::string event_path = "/sdcard/sys/brain_core/events.log";
        FILE* f = fopen(event_path.c_str(), "a");
        if (f) {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_buf[32];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
            fprintf(f, "[%s] [%s] %s\n", time_buf, tag.c_str(), message.c_str());
            fclose(f);
        }
    }

} // namespace brain_logger

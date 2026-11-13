#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <stdio.h>
#include <sys/stat.h>
#include "hal.h"

// A simple file-backed vector to save RAM
// "Infinity Link" Virtual Memory Technology
template <typename T>
class SwapVector {
private:
    std::string _filename;
    size_t _count = 0;
    std::mutex _mutex;

public:
    SwapVector(const std::string& name) {
        // Prefer Internal Flash (SPIFFS) if available for speed
        struct stat st;
        if (stat("/internal", &st) == 0) {
            _filename = "/internal/swap_" + name + ".bin";
        } else {
            _filename = "/sdcard/swap_" + name + ".bin";
        }
        
        // Clear existing
        FILE* f = fopen(_filename.c_str(), "w");
        if (f) fclose(f);
    }

    void push_back(const T& item) {
        std::lock_guard<std::mutex> lock(_mutex);
        FILE* f = fopen(_filename.c_str(), "ab");
        if (f) {
            fwrite(&item, sizeof(T), 1, f);
            fclose(f);
            _count++;
        }
    }

    // This is slow! Only use for UI refresh.
    T get(size_t index) {
        std::lock_guard<std::mutex> lock(_mutex);
        T item = {};
        FILE* f = fopen(_filename.c_str(), "rb");
        if (f) {
            fseek(f, index * sizeof(T), SEEK_SET);
            fread(&item, sizeof(T), 1, f);
            fclose(f);
        }
        return item;
    }

    size_t size() {
        return _count;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(_mutex);
        _count = 0;
        FILE* f = fopen(_filename.c_str(), "w");
        if (f) fclose(f);
    }
};

// Specialization for Strings (store as fixed size chars or length-prefixed? Let's use fixed buffer for simplicity)
struct SwapString {
    char data[128];
    uint16_t color;

    SwapString(const std::string& s, uint16_t c) {
        snprintf(data, sizeof(data), "%s", s.c_str());
        color = c;
    }
    SwapString() : color(0) { data[0] = 0; }
};

// ============================================================================
// SwapBuffer: 通用 SD 卡虚拟内存缓冲区
// 用于将大块数据交换到 SD 卡，弥补无 PSRAM 的不足
// ============================================================================
class SwapBuffer {
private:
    std::string _path;
    size_t _capacity = 0;
    bool _valid = false;
    std::mutex _mutex;

public:
    SwapBuffer(const std::string& name, size_t capacity) : _capacity(capacity) {
        struct stat st;
        if (stat("/sdcard", &st) == 0) {
            _path = "/sdcard/swap_buf_" + name + ".bin";
        } else if (stat("/internal", &st) == 0) {
            _path = "/internal/swap_buf_" + name + ".bin";
        } else {
            _valid = false;
            return;
        }
        
        // 预分配文件空间
        FILE* f = fopen(_path.c_str(), "wb");
        if (f) {
            // 用零填充到目标大小（分块写入，节省栈）
            uint8_t zero[512] = {0};
            size_t remaining = capacity;
            while (remaining > 0) {
                size_t chunk = (remaining > sizeof(zero)) ? sizeof(zero) : remaining;
                fwrite(zero, 1, chunk, f);
                remaining -= chunk;
            }
            fclose(f);
            _valid = true;
        }
    }
    
    ~SwapBuffer() {
        if (_valid && !_path.empty()) {
            remove(_path.c_str());
        }
    }

    // 写入数据到 SD 卡缓冲区
    bool write(const void* data, size_t offset, size_t len) {
        if (!_valid || offset + len > _capacity) return false;
        std::lock_guard<std::mutex> lock(_mutex);
        FILE* f = fopen(_path.c_str(), "r+b");
        if (!f) return false;
        fseek(f, offset, SEEK_SET);
        size_t written = fwrite(data, 1, len, f);
        fclose(f);
        return (written == len);
    }

    // 从 SD 卡缓冲区读取数据到 RAM
    bool read(void* buf, size_t offset, size_t len) {
        if (!_valid || offset + len > _capacity) return false;
        std::lock_guard<std::mutex> lock(_mutex);
        FILE* f = fopen(_path.c_str(), "rb");
        if (!f) return false;
        fseek(f, offset, SEEK_SET);
        size_t read_bytes = fread(buf, 1, len, f);
        fclose(f);
        return (read_bytes == len);
    }

    size_t capacity() const { return _capacity; }
    bool isValid() const { return _valid; }
    const std::string& path() const { return _path; }
};

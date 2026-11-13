#include "memory_system.h"
#include "nvs_helper.h"
#include <mooncake_log.h>
#include <cJSON.h>
#include <ctime>
#include <cstdlib>

static const std::string TAG = "Memory";

MemorySystem& MemorySystem::getInstance() {
    static MemorySystem instance;
    return instance;
}

void MemorySystem::init() { load(); }

void MemorySystem::load() {
    std::string data;
    if (!NvsHelper::getStr("mem_store", "data", data, 4096)) return;
    cJSON* arr = cJSON_Parse(data.c_str());
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return; }
    for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
        cJSON* o = cJSON_GetArrayItem(arr, i);
        MemoryEntry e;
        cJSON* item;
        if ((item = cJSON_GetObjectItem(o, "key")) && item->valuestring) e.key = item->valuestring;
        if ((item = cJSON_GetObjectItem(o, "value")) && item->valuestring) e.value = item->valuestring;
        if ((item = cJSON_GetObjectItem(o, "created"))) e.created_at = (int64_t)item->valuedouble;
        if ((item = cJSON_GetObjectItem(o, "expires"))) e.expires_at = (int64_t)item->valuedouble;
        _memories.push_back(e);
    }
    cJSON_Delete(arr);
}

void MemorySystem::save() {
    cJSON* arr = cJSON_CreateArray();
    for (auto& m : _memories) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", m.key.c_str());
        cJSON_AddStringToObject(o, "value", m.value.c_str());
        cJSON_AddNumberToObject(o, "created", (double)m.created_at);
        cJSON_AddNumberToObject(o, "expires", (double)m.expires_at);
        cJSON_AddItemToArray(arr, o);
    }
    char* out = cJSON_PrintUnformatted(arr);
    NvsHelper::setStr("mem_store", "data", out);
    free(out); cJSON_Delete(arr);
}

void MemorySystem::set(const std::string& key, const std::string& value, int ttl_seconds) {
    for (auto& m : _memories) {
        if (m.key == key) { m.value = value; m.created_at = time(nullptr); m.expires_at = ttl_seconds > 0 ? time(nullptr) + ttl_seconds : 0; save(); return; }
    }
    MemoryEntry e; e.key = key; e.value = value; e.created_at = time(nullptr);
    e.expires_at = ttl_seconds > 0 ? time(nullptr) + ttl_seconds : 0;
    _memories.push_back(e); save();
}

std::string MemorySystem::get(const std::string& key, const std::string& def) {
    int64_t now = time(nullptr);
    for (auto it = _memories.begin(); it != _memories.end(); ++it) {
        if (it->expires_at > 0 && it->expires_at < now) { _memories.erase(it); save(); break; }
    }
    for (auto& m : _memories) { if (m.key == key) return m.value; }
    return def;
}

void MemorySystem::remove(const std::string& key) {
    for (auto it = _memories.begin(); it != _memories.end(); ++it) {
        if (it->key == key) { _memories.erase(it); save(); return; }
    }
}

void MemorySystem::addReminder(const std::string& text, int64_t trigger_time, bool recurring, int interval_min) {
    Reminder r; r.id = std::to_string(time(nullptr)) + "-" + std::to_string(rand() & 0xFFFF);
    r.text = text; r.trigger_time = trigger_time; r.recurring = recurring; r.interval_min = interval_min;
    r.active = true; r.fired = false;
    _reminders.push_back(r);
    mclog::tagInfo(TAG, "Reminder added: {}", text);
}

std::vector<Reminder> MemorySystem::getActiveReminders() const {
    std::vector<Reminder> result;
    int64_t now = time(nullptr);
    for (auto& r : _reminders) { if (r.active && !r.fired && r.trigger_time <= now) result.push_back(r); }
    return result;
}

void MemorySystem::dismissReminder(const std::string& id) {
    for (auto& r : _reminders) { if (r.id == id) { r.active = false; return; } }
}

void MemorySystem::fireReminder(const std::string& id) {
    for (auto& r : _reminders) {
        if (r.id == id) {
            r.fired = true;
            if (r.recurring && r.interval_min > 0) { r.fired = false; r.trigger_time = time(nullptr) + r.interval_min * 60; }
            else { r.active = false; }
            return;
        }
    }
}

void MemorySystem::update() {}

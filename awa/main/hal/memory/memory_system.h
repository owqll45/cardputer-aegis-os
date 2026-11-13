#pragma once
#include <string>
#include <vector>

struct MemoryEntry {
    std::string key;
    std::string value;
    int64_t created_at = 0;
    int64_t expires_at = 0;
};

struct Reminder {
    std::string id;
    std::string text;
    int64_t trigger_time = 0;
    bool recurring = false;
    int interval_min = 0;
    bool active = true;
    bool fired = false;
};

class MemorySystem {
public:
    static MemorySystem& getInstance();

    void init();
    void update();

    void set(const std::string& key, const std::string& value, int ttl_seconds = 0);
    std::string get(const std::string& key, const std::string& def = "");
    void remove(const std::string& key);

    void addReminder(const std::string& text, int64_t trigger_time, bool recurring = false, int interval_min = 0);
    std::vector<Reminder> getActiveReminders() const;
    void dismissReminder(const std::string& id);
    void fireReminder(const std::string& id);

    std::vector<MemoryEntry> getAllMemories() const { return _memories; }

private:
    MemorySystem() = default;
    void save();
    void load();

    std::vector<MemoryEntry> _memories;
    std::vector<Reminder> _reminders;
};

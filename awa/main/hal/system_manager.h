#pragma once
#include <string>

class SystemManager {
public:
    SystemManager();
    ~SystemManager();

    void init();
    void update();

    /* System Services (Android-like IPC) */
    void requestSystemPopup(const std::string& title, const std::string& msg, int type);
    void reportAppHealth();
    void forceKillCurrentApp();
    bool isSystemBusy() const { return _system_popup_active; }

    /* Emergency UI control */
    bool isEmergencyMode() const { return _is_emergency_mode; }
    void setEmergencyMode(bool enable) { _is_emergency_mode = enable; }

    bool isSystemPopupActive() const { return _system_popup_active; }
    void setSystemPopupActive(bool active) { _system_popup_active = active; }

private:
    bool _system_popup_active = false;
    bool _is_emergency_mode = false;
};

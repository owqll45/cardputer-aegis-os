#include "security_manager.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <cstring>

namespace sys_security {

    static const char* TAG = "SecurityManager";
    static const char* NVS_NAMESPACE = "security";

    void SecurityManager::init() {
        loadFromNVS();
        ESP_LOGI(TAG, "Security Manager Initialized");
    }

    bool SecurityManager::verifyAdminPassword(const std::string& password) {
        // TODO: Use Hash
        if (_admin_password_hash.empty()) return true; // No password set = allow? Or block? 
        // If no password set, verify always true? Or user needs to set one.
        return password == _admin_password_hash;
    }

    void SecurityManager::setAdminPassword(const std::string& password) {
        _admin_password_hash = password;
        saveToNVS();
    }
    
    bool SecurityManager::hasAdminPassword() {
        return !_admin_password_hash.empty();
    }

    bool SecurityManager::isPeripheralLocked(Peripheral p) {
        if (p == Peripheral::ALL) return _peripheral_locks != 0;
        return (_peripheral_locks & (1 << (int)p));
    }

    void SecurityManager::setPeripheralLock(Peripheral p, bool locked) {
        if (locked) {
            _peripheral_locks |= (1 << (int)p);
        } else {
            _peripheral_locks &= ~(1 << (int)p);
        }
        saveToNVS();
    }

    bool SecurityManager::checkAppPermission(const std::string& appName, Peripheral p) {
        // System apps (Launcher) always have permission
        if (appName == "Launcher") return true;
        
        // Check if peripheral is globally locked
        if (isPeripheralLocked(p)) {
            // TODO: Prompt for Admin Password?
            // For now, just return false
            return false;
        }
        return true;
    }

    void SecurityManager::loadFromNVS() {
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
            uint32_t locks = 0;
            if (nvs_get_u32(handle, "locks", &locks) == ESP_OK) {
                _peripheral_locks = locks;
            }
            
            size_t len = 0;
            if (nvs_get_blob(handle, "pwd", NULL, &len) == ESP_OK) {
                if (len > 0) {
                    char* buf = new char[len + 1];
                    if (nvs_get_blob(handle, "pwd", buf, &len) == ESP_OK) {
                        buf[len] = 0;
                        _admin_password_hash = std::string(buf);
                    }
                    delete[] buf;
                }
            }
            nvs_close(handle);
        }
    }

    void SecurityManager::saveToNVS() {
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u32(handle, "locks", _peripheral_locks);
            if (!_admin_password_hash.empty()) {
                nvs_set_blob(handle, "pwd", _admin_password_hash.c_str(), _admin_password_hash.length());
            }
            nvs_commit(handle);
            nvs_close(handle);
        }
    }

}

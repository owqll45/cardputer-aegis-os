#pragma once
#include <string>
#include <vector>

namespace sys_security {

    enum class Peripheral {
        WIFI = 0,
        BLUETOOTH,
        SD_CARD,
        USB,
        SPEAKER,
        MICROPHONE,
        CAMERA, // Future proof
        IR,
        ALL
    };

    class SecurityManager {
    public:
        static SecurityManager& get() {
            static SecurityManager instance;
            return instance;
        }

        void init();
        
        // Admin Auth
        bool verifyAdminPassword(const std::string& password);
        void setAdminPassword(const std::string& password);
        bool hasAdminPassword();

        // Peripheral Lock
        bool isPeripheralLocked(Peripheral p);
        void setPeripheralLock(Peripheral p, bool locked);
        
        // App Permission (Simple version)
        bool checkAppPermission(const std::string& appName, Peripheral p);

    private:
        SecurityManager() {}
        std::string _admin_password_hash; // Store hash in reality (plaintext for now for simplicity)
        uint32_t _peripheral_locks = 0; // Bitmask
        
        void loadFromNVS();
        void saveToNVS();
    };

}

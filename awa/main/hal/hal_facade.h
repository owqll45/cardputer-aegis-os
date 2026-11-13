#pragma once
#include <esp_log.h>
#include <esp_system.h>
#include "display_manager.h"
#include "audio_manager.h"
#include "input_manager.h"
#include "power_manager.h"
#include "network_manager.h"
#include "storage_manager.h"
#include "life_guard_manager.h"
#include "system_manager.h"
#include "ui_renderer.h"
#include "system_utils.h"
#include "utils/settings/settings.h"

class HalFacade {
public:
    void init();
    void update();

    /* Module Accessors (New API) */
    DisplayManager& displayManager() { return _display_manager; }
    AudioManager& audioManager() { return _audio_manager; }
    InputManager& inputManager() { return _input_manager; }
    PowerManager& powerManager() { return _power_manager; }
    NetworkManager& networkManager() { return _network_manager; }
    StorageManager& storageManager() { return _storage_manager; }
    LifeGuardManager& lifeGuardManager() { return _life_guard_manager; }
    SystemManager& systemManager() { return _system_manager; }
    UIRenderer& uiRenderer() { return _ui_renderer; }
    SystemUtils& systemUtils() { return _system_utils; }

    Settings& getSettings()
    {
        return *_settings;
    }

    /* ============================================ */
    /* BACKWARD COMPATIBILITY LAYER          */
    /* All original Hal class methods are here */
    /* ============================================ */

    /* --- System --- */
    inline void delay(std::uint32_t ms) { _system_utils.delay(ms); }
    inline std::uint32_t millis() { return _system_utils.millis(); }
    inline void feedTheDog() { _system_utils.feedTheDog(); }
    inline std::vector<uint8_t> getDeviceMac() { return _system_utils.getDeviceMac(); }
    inline std::string getDeviceMacString() { return _system_utils.getDeviceMacString(); }

    /* --- Display (Safe pointer access initialized after M5.begin()) --- */
    M5GFX* _display_ptr = nullptr;
    lgfx::LGFX_Sprite* _canvas_ptr = nullptr;
    lgfx::LGFX_Sprite* _canvas_system_bar_ptr = nullptr;
    lgfx::LGFX_Sprite* _canvas_keyboard_bar_ptr = nullptr;

    M5GFX& display() { return *_display_ptr; }
    lgfx::LGFX_Sprite& canvas() { return *_canvas_ptr; }
    lgfx::LGFX_Sprite& uiCanvas() { return *_canvas_ptr; }
    lgfx::LGFX_Sprite& canvasSystemBar() { return *_canvas_system_bar_ptr; }
    lgfx::LGFX_Sprite& canvasKeyboardBar() { return *_canvas_keyboard_bar_ptr; }

    inline void pushCanvasSystemBar() { _display_manager.pushSystemBar(); }
    inline void pushCanvasKeyboardBar() { _display_manager.pushKeyboardBar(); }
    inline void pushCanvas() { _display_manager.pushCanvas(); }
    inline bool isCanvasValid() { return _display_manager.isCanvasValid(); }
    inline void resetCanvas() { _display_manager.resetCanvas(); }

    /* --- Audio --- */
    m5::Speaker_Class& speaker = M5.Speaker;
    m5::Mic_Class& mic = M5.Mic;
    inline void setGlobalMute(bool mute) { _audio_manager.setGlobalMute(mute); }
    inline bool isGlobalMute() const { return _audio_manager.isGlobalMute(); }

    /* --- Input --- */
    /* 全局统一返回键：G0 侧键（M5.BtnA / GPIO0）
     *
     * M5.update() 由 desktop_task 高频调用，wasClicked() 只在检测到点击的那一次
     * update 里为真；刷新率低的应用（10Hz 等）会直接错过，表现为“按了退不出去”。
     * 这里做一层锁存：点击事件保留到应用取走为止。
     */
    class GoButton {
    public:
        void poll()
        {
            if (M5.BtnA.wasClicked()) {
                _clicked = true;
                // 按下瞬间就落日志: 证明"键真的被按到了", 与后续 close/返回链路解耦
                ESP_LOGI("HAL", "G0 click latched heap=%u",
                         (unsigned)esp_get_free_heap_size());
            }
            if (M5.BtnA.wasPressed()) _pressed = true;
        }
        bool wasClicked()
        {
            bool v = _clicked;
            _clicked = false;
            return v;
        }
        bool wasPressed()
        {
            bool v = _pressed;
            _pressed = false;
            return v;
        }
        bool isPressed() const { return M5.BtnA.isPressed(); }
        bool wasHold() const { return M5.BtnA.wasHold(); }
        void clear()
        {
            _clicked = false;
            _pressed = false;
        }

    private:
        volatile bool _clicked = false;
        volatile bool _pressed = false;
    };

    GoButton& homeButton() { return _go_button; }
    GoButton& goButton() { return _go_button; }
    Keyboard& keyboard = _input_manager.getKeyboard();

    /* --- Power --- */
    inline uint8_t getBatLevel() { return M5.Power.getBatteryLevel(); }
    using PowerStatus_t = PowerManager::PowerStatus_t;
    inline PowerStatus_t getPowerStatus() { return _power_manager.getPowerStatus(); }
    inline void updatePowerSafety() { _power_manager.updatePowerSafety(); }
    inline uint32_t getLastAppHeartbeat() const { return _power_manager.getLastAppHeartbeat(); }

    /* --- WiFi --- */
    using ScanResult_t = NetworkManager::ScanResult_t;
    inline void wifiInit() { _network_manager.wifiInit(); }
    inline void wifiApInit(const std::string& ssid, const std::string& password) { _network_manager.wifiApInit(ssid, password); }
    inline void wifiApStaInit(const std::string& apSsid, const std::string& apPassword,
                               const std::string& staSsid = "", const std::string& staPassword = "") {
        _network_manager.wifiApStaInit(apSsid, apPassword, staSsid, staPassword);
    }
    inline bool isApActive() const { return _network_manager.isApActive(); }
    inline void wifiDeinit() { _network_manager.wifiDeinit(); }
    inline void wifiScan(std::vector<ScanResult_t>& scanResult) { _network_manager.wifiScan(scanResult); }
    inline bool wifiConnect(const std::string& ssid, const std::string& password) { return _network_manager.wifiConnect(ssid, password); }
    inline bool isWifiConnected() const { return _network_manager.isWifiConnected(); }
    inline void wifiDisconnect() { _network_manager.wifiDisconnect(); }
    inline NetworkManager& getNetwork() { return _network_manager; }

    /* --- EspNow --- */
    inline void espNowInit() { _network_manager.espNowInit(); }
    inline void espNowDeinit() { _network_manager.espNowDeinit(); }
    inline void espNowSend(const std::string& data) { _network_manager.espNowSend(data); }
    inline bool espNowAvailable() { return _network_manager.espNowAvailable(); }
    inline const std::string& espNowGetReceivedData() { return _network_manager.espNowGetReceivedData(); }
    inline void espNowClearReceivedData() { _network_manager.espNowClearReceivedData(); }

    /* --- IR --- */
    inline void irInit() { _network_manager.irInit(); }
    inline void irSend(uint8_t addr, uint8_t cmd) { _network_manager.irSend(addr, cmd); }

    /* --- BLE --- */
    inline void bleKeyboardInit() { _network_manager.bleKeyboardInit(); }
    inline bool bleKeyboardIsConnected() const { return _network_manager.bleKeyboardIsConnected(); }

    /* --- USB --- */
    inline void usbKeyboardInit() { _network_manager.usbKeyboardInit(); }
    inline bool usbKeyboardIsConnected() const { return _network_manager.usbKeyboardIsConnected(); }

    /* --- IMU --- */
    m5::IMU_Class& imu = M5.Imu;

    /* --- SD Card --- */
    using SdCardProbeResult_t = StorageManager::SdCardProbeResult_t;
    inline SdCardProbeResult_t sdCardProbe() { return _storage_manager.sdCardProbe(); }
    inline void sd_card_init() { _storage_manager.sd_card_init(); }
    inline void spiffs_init() { _storage_manager.spiffs_init(); }

    /* --- System Services --- */
    inline void requestSystemPopup(const std::string& title, const std::string& msg, int type) { _system_manager.requestSystemPopup(title, msg, type); }
    inline void reportAppHealth() { _power_manager.setLastAppHeartbeat(millis()); }
    inline void forceKillCurrentApp() { _system_manager.forceKillCurrentApp(); }
    inline bool isSystemBusy() const { return _system_manager.isSystemBusy(); }
    inline bool isEmergencyMode() const { return _system_manager.isEmergencyMode(); }
    inline void setEmergencyMode(bool enable) { _system_manager.setEmergencyMode(enable); _display_manager.setEmergencyMode(enable); }
    inline bool isSystemPopupActive() const { return _system_manager.isSystemPopupActive(); }
    inline void setSystemPopupActive(bool active) { _system_manager.setSystemPopupActive(active); }

    /* --- Life Guard --- */
    using LifeState_t = LifeGuardManager::LifeState_t;
    using MedEffect_t = LifeGuardManager::MedEffect_t;
    using LifeGuardStatus_t = LifeGuardManager::LifeGuardStatus_t;
    using GuardData_t = LifeGuardManager::GuardData_t;

    /* Export enum values for backward compatibility */
    static constexpr LifeState_t STATE_NORMAL = LifeGuardManager::STATE_NORMAL;
    static constexpr LifeState_t STATE_GHOST_PRESS = LifeGuardManager::STATE_GHOST_PRESS;
    static constexpr LifeState_t STATE_SAD_DREAM = LifeGuardManager::STATE_SAD_DREAM;
    static constexpr LifeState_t STATE_DAY_STRESS = LifeGuardManager::STATE_DAY_STRESS;
    static constexpr LifeState_t STATE_NIGHTMARE = LifeGuardManager::STATE_NIGHTMARE;
    static constexpr LifeState_t STATE_PANIC = LifeGuardManager::STATE_PANIC;
    static constexpr LifeState_t STATE_ANXIETY = LifeGuardManager::STATE_ANXIETY;
    static constexpr LifeState_t STATE_DEPRESSION = LifeGuardManager::STATE_DEPRESSION;

    static constexpr MedEffect_t MED_NONE = LifeGuardManager::MED_NONE;
    static constexpr MedEffect_t MED_COVERED = LifeGuardManager::MED_COVERED;
    static constexpr MedEffect_t MED_WEARING_OFF = LifeGuardManager::MED_WEARING_OFF;
    static constexpr MedEffect_t MED_EXPIRED = LifeGuardManager::MED_EXPIRED;
    static constexpr MedEffect_t MED_WAITING_VERIFY = LifeGuardManager::MED_WAITING_VERIFY;

    GuardData_t* _gd = nullptr;

    inline void setSafeMode(bool enable) { _life_guard_manager.setSafeMode(enable); }
    inline bool isSafeMode() const { return _life_guard_manager.isSafeMode(); }
    inline void renderSafeModeMonitor() { _life_guard_manager.renderSafeModeMonitor(); }
    inline void updateMedicationStatus() { _life_guard_manager.updateMedicationStatus(); }
    inline void estimateMoodState() { _life_guard_manager.estimateMoodState(); }
    inline void confirmMedicationTaken(bool morning) { _life_guard_manager.confirmMedicationTaken(morning); }
    inline LifeGuardStatus_t getLifeGuardStatus() { return _life_guard_manager.getStatus(); }
    inline void triggerEmergencySOS() { _life_guard_manager.triggerEmergencySOS(); }
    inline void triggerLifeAlert(LifeState_t state) { _life_guard_manager.triggerLifeAlert(state); }
    inline void updateLifeGuard() { _life_guard_manager.update(); }
    inline float calculateRiskScore() { return _life_guard_manager.calculateRiskScore(); }

    /* BLE Heart Rate Service */
    inline void initHeartRateService() { _life_guard_manager.initHeartRateService(); }
    inline void stopHeartRateService() { _life_guard_manager.stopHeartRateService(); }
    inline bool isHeartRateServiceInited() const { return _life_guard_manager.isHeartRateServiceInited(); }
    inline void setTargetHeartRateDevice(const std::string& addr) { _life_guard_manager.setTargetHeartRateDevice(addr); }
    inline void setHeartRate(int hr) { _life_guard_manager.setHeartRate(hr); }
    inline void addRRInterval(uint16_t rr_ms) { _life_guard_manager.addRRInterval(rr_ms); }
    inline int getHeartRate() const { return _life_guard_manager.getHeartRate(); }
    inline int getHRV() const { return _life_guard_manager.getHRV(); }
    inline int getRespirationRate() const { return _life_guard_manager.getRespirationRate(); }

    /* --- Memory & Storage --- */
    inline void initSdSwap() { _storage_manager.initSdSwap(); }
    inline void purgeMemory(bool kill_system_apps = false) { _storage_manager.purgeMemory(kill_system_apps); }
    inline void* requestSwapBuffer(size_t size) { return _storage_manager.requestSwapBuffer(size); }
    inline void freeSwapBuffer(void* ptr) { _storage_manager.freeSwapBuffer(ptr); }
    inline bool loadAppFromSd(const std::string& path) { return _storage_manager.loadAppFromSd(path); }
    inline void pushHrHistoryToSd(int hr) { _storage_manager.pushHrHistoryToSd(hr); }
    inline std::vector<int> getHrHistoryFromSd() { return _storage_manager.getHrHistoryFromSd(); }

    /* --- UI Rendering --- */
    inline void renderHardwareStatus() { _ui_renderer.renderHardwareStatus(); }
    inline void renderPowerDashboard() { _ui_renderer.renderPowerDashboard(); }
    inline void renderLifeDashboard() { _ui_renderer.renderLifeDashboard(); }
    inline void drawDashboardChart(int x, int y, int w, int h, const std::vector<float>& data, uint16_t color, const char* label)
    { _ui_renderer.drawDashboardChart(x, y, w, h, data, color, label); }

    /* --- UI Utils --- */
    struct UIContextGuard {
        const lgfx::IFont* font;
        textdatum_t datum;
        uint16_t textColor;
        uint16_t textBgColor;
        int32_t cursorX, cursorY;
        float textScaleX, textScaleY;

        UIContextGuard();
        ~UIContextGuard();
    };

private:
    DisplayManager _display_manager;
    AudioManager _audio_manager;
    InputManager _input_manager;
    GoButton _go_button;
    PowerManager _power_manager;
    NetworkManager _network_manager;
    StorageManager _storage_manager;
    LifeGuardManager _life_guard_manager;
    SystemManager _system_manager;
    UIRenderer _ui_renderer;
    SystemUtils _system_utils;

    Settings* _settings = nullptr;
};

HalFacade& GetHAL();

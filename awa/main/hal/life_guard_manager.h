#pragma once
#include <deque>
#include <string>
#include <vector>
#include <cstdint>

class LifeGuardManager {
public:
    enum LifeState_t {
        STATE_NORMAL = 0,
        STATE_GHOST_PRESS,
        STATE_SAD_DREAM,
        STATE_DAY_STRESS,
        STATE_NIGHTMARE,
        STATE_PANIC,
        STATE_ANXIETY,
        STATE_DEPRESSION,
        STATE_NUM
    };

    enum MedEffect_t {
        MED_NONE = 0,
        MED_COVERED,
        MED_WEARING_OFF,
        MED_EXPIRED,
        MED_WAITING_VERIFY
    };

    struct LifeGuardStatus_t {
        int heart_rate;
        int hrv;
        int respiration_rate;
        float risk_score;
        LifeState_t current_state;
        bool motion_detected;
        uint32_t last_alert_ts;
        uint32_t last_sos_ts;

        /* Advanced 8D Metrics */
        float hr_slope;
        float lf_hf_ratio;
        uint32_t state_duration_ms;
        int delta_hr;

        /* Med & Mood Metrics */
        MedEffect_t morning_med_status;
        MedEffect_t evening_med_status;
        float emotional_stability;
        const char* current_mood_cn;
        bool med_reminder_active;
    };

    struct GuardData_t {
        std::deque<uint16_t> rr_intervals;
        std::deque<int> hr_history;
        uint32_t last_motion_ts;
        float last_accel_sum;
        float cooling_period_min = 10.0f;

        /* Calibration & Learning */
        float hrv_baseline = 45.0f;
        float hr_baseline = 68.0f;
        uint32_t samples_collected = 0;

        /* Med Timing (User Schedule) */
        uint32_t morning_dose_ts = 0;
        uint32_t evening_dose_ts = 0;
        bool morning_dose_taken = false;
        bool evening_dose_taken = false;
    };

    LifeGuardManager();
    ~LifeGuardManager();

    void init();
    void update();

    LifeGuardStatus_t getStatus() const { return _life_guard; }
    void setSafeMode(bool enable);
    bool isSafeMode() const { return _system_safe_mode; }

    void renderSafeModeMonitor();
    void updateMedicationStatus();
    void estimateMoodState();
    void confirmMedicationTaken(bool morning);

    void triggerEmergencySOS();
    void triggerLifeAlert(LifeState_t state);
    float calculateRiskScore();

    /* BLE Heart Rate Service */
    void initHeartRateService();
    void stopHeartRateService();
    bool isHeartRateServiceInited() const { return _hr_service_inited; }
    void setTargetHeartRateDevice(const std::string& addr);
    void setHeartRate(int hr);
    void addRRInterval(uint16_t rr_ms);
    int getHeartRate() const;
    int getHRV() const;
    int getRespirationRate() const;

    void setMotionDetected(bool detected);
    void setLastAppHeartbeat(uint32_t timestamp);

private:
    LifeGuardStatus_t _life_guard;
    GuardData_t* _guard_data;
    uint32_t _last_app_heartbeat;
    bool _system_safe_mode;
    std::string _target_hr_addr;
    bool _hr_service_inited;

    void updateRiskScore();
    void updateState();
};

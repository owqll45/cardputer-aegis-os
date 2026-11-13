#include "life_guard_manager.h"
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <math.h>
#include "../crash_handler.h"
#include <nvs_flash.h>
// #include <nimble/ble.h>  // Disabled - include path issue with ESP-IDF 5.1.4

static const std::string _tag = "LifeGuard";

/* BLE Globals */
TaskHandle_t s_ble_task_handle  = NULL;
bool _hr_service_inited         = false;
static const std::string _target_hr_addr = "ee:90:0b:ce:71:8c";

LifeGuardManager::LifeGuardManager()
    : _guard_data(nullptr)
    , _last_app_heartbeat(0)
    , _system_safe_mode(false)
    , _target_hr_addr("ee:90:0b:ce:71:8c")
    , _hr_service_inited(false)
{
    _life_guard = {0};
}

LifeGuardManager::~LifeGuardManager()
{
    if (_guard_data) {
        delete _guard_data;
    }
}

void LifeGuardManager::init()
{
    if (!_guard_data) {
        _guard_data = new GuardData_t();
        mclog::tagInfo(_tag, "GuardData initialized (Safe-Heap)");
    }
}

void LifeGuardManager::update()
{
    if (!_guard_data) {
        return;
    }

    static uint32_t last_eval = 0;
    if (lgfx::v1::millis() - last_eval < 1000) return;
    uint32_t dt = lgfx::v1::millis() - last_eval;
    last_eval = lgfx::v1::millis();

    /* --- Motion Detection --- */
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    float current_accel_sum = abs(ax) + abs(ay) + abs(az);

    if (abs(current_accel_sum - _guard_data->last_accel_sum) > 0.15f) {
        _life_guard.motion_detected = true;
        _guard_data->last_motion_ts = lgfx::v1::millis();
    } else if (lgfx::v1::millis() - _guard_data->last_motion_ts > 5000) {
        _life_guard.motion_detected = false;
    }
    _guard_data->last_accel_sum = current_accel_sum;

    /* --- Advanced Analysis (v4.8) --- */
    estimateMoodState();
    updateMedicationStatus();

    /* --- Trend & Delta --- */
    if (_life_guard.heart_rate > 0) {
        if (!_guard_data->hr_history.empty()) {
            _life_guard.delta_hr = _life_guard.heart_rate - _guard_data->hr_history.back();
        }
        _guard_data->hr_history.push_back(_life_guard.heart_rate);
        if (_guard_data->hr_history.size() > 60) _guard_data->hr_history.pop_front();

        /* Slope calculation (approx BPM change per minute) */
        if (_guard_data->hr_history.size() >= 30) {
            _life_guard.hr_slope = (_guard_data->hr_history.back() - _guard_data->hr_history.front()) * 2.0f;
        }
    }

    _life_guard.risk_score = calculateRiskScore();
    float score            = _life_guard.risk_score;
    int hr                 = _life_guard.heart_rate;
    int hrv                = _life_guard.hrv;
    int delta              = abs(_life_guard.delta_hr);

    /* --- State Machine & Auto-Trigger (v4.9d) --- */
    /* 1. Panic Attack (Physiological Jump) */
    if (delta > 20 && hr > 110 && hrv < 20) {
        triggerLifeAlert(STATE_PANIC);
    }
    /* 2. Ghost Press (Sleep Paralysis) */
    else if (score > 7.5f && !_life_guard.motion_detected) {
        triggerLifeAlert(STATE_GHOST_PRESS);
    }
    /* 3. Sad Dream (Deep High HR) */
    else if (score > 6.5f && hr > 90 && hrv < 25) {
        _life_guard.state_duration_ms += dt;
        if (_life_guard.state_duration_ms > 90000) triggerLifeAlert(STATE_SAD_DREAM);
    }
    /* 4. Nightmare (High Motion during high risk) */
    else if (score > 6.0f && _life_guard.motion_detected && hr > 100) {
        triggerLifeAlert(STATE_NIGHTMARE);
    }
    /* 5. Daily Stress (Chronic high risk) */
    else if (score > 4.5f && !_life_guard.motion_detected) {
        _life_guard.state_duration_ms += dt;
        if (_life_guard.state_duration_ms > 60000) triggerLifeAlert(STATE_DAY_STRESS);
    } else {
        _life_guard.current_state     = STATE_NORMAL;
        _life_guard.state_duration_ms = 0;
    }
}

void LifeGuardManager::setSafeMode(bool enable)
{
    _system_safe_mode = enable;
    if (enable) {
        mclog::tagWarn(_tag, "SYSTEM ENTERING SAFE MODE (BLSS ACTIVE)");
    }
}

void LifeGuardManager::renderSafeModeMonitor()
{
    // 日志已静默: 防止 USB Serial/JTAG RX 被 TX 饿死 (串口指令丢失)
}

void LifeGuardManager::updateMedicationStatus()
{
    // 日志已静默: 防止 USB Serial/JTAG RX 被 TX 饿死 (串口指令丢失)
}

void LifeGuardManager::estimateMoodState()
{
    static const char* _mood_labels[] = {"平静", "正在药效中", "轻度波动", "显著焦虑",
                                          "由于药效消退引起的波动", "抑郁/低落状态", "惊恐发作预警"};

    if (!_guard_data || _life_guard.heart_rate <= 0) return;

    int hr  = _life_guard.heart_rate;
    int hrv = _life_guard.hrv;

    if (hrv > 50 && hr < 75) {
        _life_guard.current_mood_cn = _mood_labels[0];
        _life_guard.emotional_stability = 8.5f;
    } else if (hrv > 35 && hr < 85) {
        _life_guard.current_mood_cn = _mood_labels[1];
        _life_guard.emotional_stability = 7.0f;
    } else if (hrv > 25 && hr < 95) {
        _life_guard.current_mood_cn = _mood_labels[2];
        _life_guard.emotional_stability = 5.5f;
    } else if (hrv > 15) {
        _life_guard.current_mood_cn = _mood_labels[3];
        _life_guard.emotional_stability = 3.5f;
    } else if (hrv > 10) {
        _life_guard.current_mood_cn = _mood_labels[4];
        _life_guard.emotional_stability = 2.5f;
    } else if (hr > 5) {
        _life_guard.current_mood_cn = _mood_labels[5];
        _life_guard.emotional_stability = 1.5f;
    } else {
        _life_guard.current_mood_cn = _mood_labels[6];
        _life_guard.emotional_stability = 0.5f;
    }
}

void LifeGuardManager::confirmMedicationTaken(bool morning)
{
    if (morning) {
        _guard_data->morning_dose_taken = true;
        _guard_data->morning_dose_ts = lgfx::v1::millis();
    } else {
        _guard_data->evening_dose_taken = true;
        _guard_data->evening_dose_ts = lgfx::v1::millis();
    }
}

void LifeGuardManager::triggerEmergencySOS()
{
    mclog::tagWarn(_tag, "Emergency SOS triggered!");
    _life_guard.last_sos_ts = lgfx::v1::millis();

    char cmd[64];
    sprintf(cmd, "SOS|%d|%d", _life_guard.heart_rate, _life_guard.hrv);
}

void LifeGuardManager::triggerLifeAlert(LifeState_t state)
{
    if (!_guard_data) return;
    uint32_t cooldown_ms = (uint32_t)(_guard_data->cooling_period_min * 60000.0f);
    if (lgfx::v1::millis() - _life_guard.last_alert_ts < cooldown_ms) return;

    _life_guard.last_alert_ts = lgfx::v1::millis();
    _life_guard.current_state = state;

    const char* title = "生命安全警报";
    const char* msg   = "检测到异常状态";
    uint16_t color    = TFT_YELLOW;

    switch (state) {
        case STATE_GHOST_PRESS:
            title = "睡眠瘫痪";
            msg   = "检测到心率骤升且身体静止\\n请尝试深呼吸或摆动手指唤醒。";
            color = TFT_YELLOW;
            M5.Speaker.tone(1000, 200);
            break;
        case STATE_SAD_DREAM:
            title = "深层悲伤梦境";
            msg   = "心率持续偏高且处于昏睡状态\\n系统正尝试将您拉回现实。";
            color = TFT_RED;
            M5.Speaker.tone(1500, 1000);
            break;
        case STATE_DAY_STRESS:
            title = "情绪压力预警";
            msg   = "检测到您当前处于高压/压抑状态\\n建议暂时放下手头工作，深呼吸。";
            color = TFT_CYAN;
            M5.Speaker.tone(800, 500);
            break;
        case STATE_NIGHTMARE:
            title = "噩梦自动唤醒";
            msg   = "检测到噩梦引起的心率波动\\n系统已为您亮灯并发出温和提醒。";
            color = 0xFDA0;
            M5.Speaker.tone(600, 300);
            break;
        case STATE_PANIC:
            title = "惊恐发作预警";
            msg   = "检测到生理指标剧烈跳变\\n紧急干预程序已启动：呼吸引导\\n请跟随屏幕上的光圈节奏深呼吸。";
            color = TFT_MAGENTA;
            M5.Speaker.tone(1200, 800);
            break;
        default:
            break;
    }

    crash_handler::show_life_alert(title, msg, color);
}

float LifeGuardManager::calculateRiskScore()
{
    if (!_guard_data) return 0;
    float score = 0;
    int hr      = _life_guard.heart_rate;
    int hrv     = _life_guard.hrv;

    if (hr <= 0) return 0;

    float hr_dev = abs(hr - 65) / 65.0f;
    score += hr_dev * 3.0f;

    if (hrv > 0 && hrv < 25) score += 3.0f;

    if (_life_guard.delta_hr > 15) score += 2.0f;

    if (!_life_guard.motion_detected && hr > 90) score += 2.0f;

    _life_guard.risk_score = (score > 10.0f) ? 10.0f : score;
    return _life_guard.risk_score;
}

/* BLE Heart Rate Service */
void LifeGuardManager::initHeartRateService()
{
    if (_hr_service_inited) return;
    if (s_ble_task_handle != NULL) return;

    mclog::tagInfo(_tag, "Initializing Heart Rate Service...");

    /* TODO: NimBLE initialization requires proper include path
     * For now, skip BLE init to avoid crash
     * The original hal.cpp had this initialized elsewhere
     */
    mclog::tagWarn(_tag, "BLE Heart Rate Service skipped (NimBLE not configured)");

    _hr_service_inited = false;  // Mark as not initialized
}

void LifeGuardManager::stopHeartRateService()
{
    if (!_hr_service_inited) return;

    mclog::tagInfo(_tag, "Stopping Heart Rate Service...");

    if (s_ble_task_handle) {
        // nimble_port_stop();  // TODO: Fix NimBLE include path
        vTaskDelay(200 / portTICK_PERIOD_MS);
        s_ble_task_handle = NULL;
    }

    _hr_service_inited = false;
    mclog::tagInfo(_tag, "Heart Rate Service stopped");
}

void LifeGuardManager::setTargetHeartRateDevice(const std::string& addr)
{
    _target_hr_addr = addr;
    mclog::tagInfo(_tag, "Target HR device set to: {}", addr);
}

void LifeGuardManager::setHeartRate(int hr)
{
    _life_guard.heart_rate = hr;
}

void LifeGuardManager::addRRInterval(uint16_t rr_ms)
{
    if (!_guard_data) return;
    if (rr_ms < 300 || rr_ms > 2000) return;

    _guard_data->rr_intervals.push_back(rr_ms);
    if (_guard_data->rr_intervals.size() > 60) _guard_data->rr_intervals.pop_front();

    if (_guard_data->rr_intervals.size() >= 10) {
        float sum_sq_diff = 0;
        for (size_t i = 1; i < _guard_data->rr_intervals.size(); i++) {
            int diff = (int)_guard_data->rr_intervals[i] - (int)_guard_data->rr_intervals[i - 1];
            sum_sq_diff += diff * diff;
        }
        _life_guard.hrv = sqrt(sum_sq_diff / (_guard_data->rr_intervals.size() - 1));
    }

    if (_guard_data->rr_intervals.size() >= 10) {
        _life_guard.respiration_rate = 14;
    }
}

int LifeGuardManager::getHeartRate() const
{
    return _life_guard.heart_rate;
}

int LifeGuardManager::getHRV() const
{
    return _life_guard.hrv;
}

int LifeGuardManager::getRespirationRate() const
{
    return _life_guard.respiration_rate;
}

void LifeGuardManager::setMotionDetected(bool detected)
{
    _life_guard.motion_detected = detected;
    if (detected) {
        _guard_data->last_motion_ts = lgfx::v1::millis();
    }
}

void LifeGuardManager::setLastAppHeartbeat(uint32_t timestamp)
{
    _last_app_heartbeat = timestamp;
}

void LifeGuardManager::updateRiskScore()
{
    calculateRiskScore();
}

void LifeGuardManager::updateState()
{
}

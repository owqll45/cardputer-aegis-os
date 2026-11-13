#include "brain_coordinator.h"
#include "brain_server.h"
#include <cmath>
#include <algorithm>
#include <mooncake_log.h>

static const char* TAG = "Coordinator";

namespace brain_coordinator {

    BrainCoordinator& BrainCoordinator::getInstance() {
        static BrainCoordinator instance;
        return instance;
    }

    HealthVerdict BrainCoordinator::processData(cJSON* data) {
        HealthVerdict verdict;
        
        // 1. Extract Data
        int hr = cJSON_GetObjectItem(data, "hr") ? cJSON_GetObjectItem(data, "hr")->valueint : 0;
        float rmssd = cJSON_GetObjectItem(data, "rmssd") ? (float)cJSON_GetObjectItem(data, "rmssd")->valuedouble : 0.0f;
        std::string med_state = cJSON_GetObjectItem(data, "med_state") ? cJSON_GetObjectItem(data, "med_state")->valuestring : "未知";
        int med_progress = cJSON_GetObjectItem(data, "med_progress") ? cJSON_GetObjectItem(data, "med_progress")->valueint : 0;
        bool motion = cJSON_GetObjectItem(data, "motion_detected") ? cJSON_GetObjectItem(data, "motion_detected")->valueint : false;

        // 2. Anomaly Filter
        if (!_validateData(hr, rmssd, motion)) {
            verdict.risk_score = 0.0f;
            verdict.summary = "信号质量波动中，正在尝试重新锁定...";
            verdict.thinking.push_back("过滤模块: 发现高频噪波，数据暂不进入推演链路");
            return verdict;
        }

        // 3. Medication Expert
        verdict.med_phase = _analyzeMedication(med_state, med_progress);
        verdict.thinking.push_back("药效模块: 当前处于 [" + verdict.med_phase + "]");

        // 4. Risk Calculation (SDC Logic)
        verdict.risk_score = _calculateRisk(hr, rmssd, motion, verdict.med_phase);
        
        // 5. Generate Summary (Based on AI_JUDGMENT_PROTOCOL.md)
        if (verdict.risk_score < 4.0f) {
            verdict.summary = "OPTIMAL_EQUILIBRIUM: 系统处于良性循环。";
        } else if (verdict.risk_score < 7.5f) {
            verdict.summary = "STABLE: 风险在可控范围内波动。";
        } else if (verdict.risk_score < 9.0f) {
            verdict.summary = "WARNING_ALERT: 系统发出代偿警告。建议立即休息。";
            verdict.thinking.push_back("会诊模块: 检测到代偿性摆动，风险系数触发黄色预警。");
        } else {
            verdict.summary = "COLLAPSE_INTERCEPT: 触发最高主权拦截指令！系统面临坍缩风险。";
            verdict.thinking.push_back("会诊模块: 生理指标共振失调，强制执行紧急干预。");
        }

        if (verdict.med_phase == "代谢末期") {
            verdict.summary = "分析结论：由于药效进入代谢末期，神经系统出现代偿性摆动。 " + verdict.summary;
        }

        return verdict;
    }

    bool BrainCoordinator::_validateData(int hr, float rmssd, bool motion) {
        if (hr > 180 || hr < 40) return false;
        return true;
    }

    std::string BrainCoordinator::_analyzeMedication(const std::string& med_state, int med_progress) {
        if (med_state == "药效覆盖中") return "黄金覆盖期";
        if (med_state == "药效消退中") return "代谢末期";
        return med_state;
    }

    float BrainCoordinator::_calculateRisk(int hr, float rmssd, bool motion, const std::string& med_phase) {
        float score = 0.0f;
        
        // HR contribution
        if (hr > 100) score += (hr - 100) * 0.2f;
        
        // HRV contribution (RMSSD)
        if (rmssd < 20) score += 3.0f;
        else if (rmssd < 40) score += 1.5f;

        // Motion Context
        if (!motion && hr > 90) score += 2.0f; // Stationary high HR is risky

        // Med Context
        if (med_phase == "代谢末期") score += 1.5f;

        return std::min(10.0f, score);
    }

} // namespace brain_coordinator

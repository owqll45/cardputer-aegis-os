#pragma once
#include <string>
#include <vector>
#include <deque>
#include "cJSON.h"

namespace brain_coordinator {

    struct HealthVerdict {
        float risk_score;
        std::string summary;
        std::vector<std::string> thinking;
        std::string med_phase;
    };

    class BrainCoordinator {
    public:
        static BrainCoordinator& getInstance();

        // Main processing entry
        HealthVerdict processData(cJSON* data);

    private:
        BrainCoordinator() = default;

        // Internal Experts
        float _calculateRisk(int hr, float rmssd, bool motion, const std::string& med_phase);
        std::string _analyzeMedication(const std::string& med_state, int med_progress);
        bool _validateData(int hr, float rmssd, bool motion);

        // State
        std::deque<int> _hr_history;
        std::deque<float> _risk_history;
    };

} // namespace brain_coordinator

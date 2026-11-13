#pragma once
#include <vector>
#include <cstdint>

class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

    void init();
    void update();

    void renderHardwareStatus();
    void renderPowerDashboard();
    void renderLifeDashboard();
    void drawDashboardChart(int x, int y, int w, int h, const std::vector<float>& data, uint16_t color,
                            const char* label);

private:
};

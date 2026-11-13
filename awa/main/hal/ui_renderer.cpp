#include "ui_renderer.h"
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include "hal_facade.h"

static const std::string _tag = "UIRenderer";

UIRenderer::UIRenderer()
{
}

UIRenderer::~UIRenderer()
{
}

void UIRenderer::init()
{
    mclog::tagInfo(_tag, "ui renderer init");
}

void UIRenderer::update()
{
}

void UIRenderer::renderHardwareStatus()
{
    /* Implement hardware status rendering logic (from hal.cpp:928) */
    /* This is a complex UI rendering function - keeping placeholder for now */
    mclog::tagInfo(_tag, "render hardware status");
}

void UIRenderer::renderPowerDashboard()
{
    auto& hal = GetHAL();

    M5.Display.fillScreen(TFT_BLACK);

    /* --- Header (Sensors Style) --- */
    M5.Display.fillRect(0, 0, 240, 20, 0x0842);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::efontCN_12);
    M5.Display.drawString("动力资源管理 (Power v2.2)", 120, 10);

    /* --- Content --- */
    float v       = hal.powerManager().getPowerStatus().voltage;
    float current = M5.Power.getBatteryCurrent();

    static std::vector<float> v_history;
    v_history.push_back(v);
    if (v_history.size() > 50) v_history.erase(v_history.begin());

    M5.Display.setTextDatum(top_left);
    M5.Display.setFont(&fonts::efontCN_12);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(10, 25);
    M5.Display.printf("%.2f V", v);

    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(10, 45);
    M5.Display.printf("电流: %.1f mA  功率: %.2f W", current, abs(v * current / 1000.0f));

    /* Draw Chart */
    drawDashboardChart(10, 60, 220, 55, v_history, 0x07FF, "电压趋势 (1min)");

    /* --- Footer --- */
    M5.Display.fillRect(0, 120, 240, 15, 0x0842);
    M5.Display.setTextColor(0x7BEF);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("<- [Ctrl+P] 返回桌面 ->", 120, 127);

    while (1) {
        M5.update();
        hal.inputManager().update();
        auto keyEvent = hal.inputManager().getKeyboard().getLatestKeyEvent();
        bool is_ctrl = hal.inputManager().getKeyboard().getModifierMask() & KEY_MOD_LCTRL;
        if (is_ctrl && keyEvent.keyCode == KEY_P && keyEvent.state == false) break;
        if (keyEvent.keyCode == KEY_ESC) break;
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    M5.Display.fillScreen(TFT_BLACK);
}

void UIRenderer::renderLifeDashboard()
{
    /* Implement life dashboard rendering logic (from hal.cpp:751) */
    mclog::tagInfo(_tag, "render life dashboard");
}

void UIRenderer::drawDashboardChart(int x, int y, int w, int h, const std::vector<float>& data, uint16_t color,
                                    const char* label)
{
    M5.Display.drawRect(x, y, w, h, 0x7BEF);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7BEF);
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString(label, x + w - 2, y + 2);

    if (data.size() < 2) return;

    float minVal = 9999.0f, maxVal = -9999.0f;
    for (float v : data) {
        if (v < minVal) minVal = v;
        if (v > maxVal) maxVal = v;
    }
    if (maxVal <= minVal) {
        maxVal = minVal + 1.0f;
        minVal = minVal - 1.0f;
    }

    int step = w / (data.size() > 1 ? data.size() - 1 : 1);
    for (size_t i = 1; i < data.size(); i++) {
        int x1 = x + (i - 1) * step;
        int y1 = y + h - (int)((data[i - 1] - minVal) / (maxVal - minVal) * (h - 2));
        int x2 = x + i * step;
        int y2 = y + h - (int)((data[i] - minVal) / (maxVal - minVal) * (h - 2));
        if (x2 > x + w) break;
        M5.Display.drawLine(x1, y1, x2, y2, color);
    }
}

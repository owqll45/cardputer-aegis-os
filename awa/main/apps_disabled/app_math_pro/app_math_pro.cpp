#include "app_math_pro.h"
#include <hal/hal.h>
#include <apps/utils/theme.h>
#include <math.h>
#include <algorithm>

AppMathPro::AppMathPro() { setAppInfo().name = "MathPro"; }

void AppMathPro::onOpen() {
    // 动态申请小尺寸 Canvas 以节省内存，并使用 8-bit 色深
    _canvas = new LGFX_Sprite(&M5.Display);
    _canvas->setColorDepth(8); 
    _canvas->createSprite(240, 135);
    
    analyzeFunction();
    
    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (e.state) {
            if (e.keyCode == KEY_ESC) close();
            else if (e.keyCode == KEY_UP) _zoom_y *= 1.2f;
            else if (e.keyCode == KEY_DOWN) _zoom_y /= 1.2f;
            else if (e.keyCode == KEY_LEFT) _zoom_x /= 1.2f;
            else if (e.keyCode == KEY_RIGHT) _zoom_x *= 1.2f;
            else if (e.keyCode == KEY_ENTER) {
                // Future: Add text input popup
            }
        }
    });
}

float AppMathPro::evaluate(const std::string& expr, float x) {
    // Simple parser for high school math: x^2, sin(x), etc.
    if (expr == "x^2") return x * x;
    if (expr == "-x^2") return -x * x;
    if (expr == "x^3") return x * x * x;
    if (expr == "sin(x)") return sin(x);
    if (expr == "cos(x)") return cos(x);
    if (expr == "ln(x)") return (x > 0) ? log(x) : NAN;
    if (expr == "exp(x)") return exp(x);
    if (expr == "1/x") return (x != 0) ? 1.0f/x : NAN;
    return x; // Fallback
}

void AppMathPro::analyzeFunction() {
    // Parity check
    float f1 = evaluate(_input_expr, 1.0f);
    float f_1 = evaluate(_input_expr, -1.0f);
    if (abs(f1 - f_1) < 0.001f) _analysis_parity = "Even (偶函数)";
    else if (abs(f1 + f_1) < 0.001f) _analysis_parity = "Odd (奇函数)";
    else _analysis_parity = "None (非奇非偶)";
    
    // Concavity check at x=0 (Direction)
    float f0 = evaluate(_input_expr, 0.0f);
    float f_eps = evaluate(_input_expr, -0.1f);
    float f_peps = evaluate(_input_expr, 0.1f);
    float second_deriv = (f_peps + f_eps - 2*f0) / (0.1f * 0.1f);
    if (second_deriv > 0.1f) _analysis_direction = "Upward (开口向上)";
    else if (second_deriv < -0.1f) _analysis_direction = "Downward (开口向下)";
    else _analysis_direction = "Linear/Inflection";
}

void AppMathPro::plotFunction() {
    int w = 240, h = 135;
    int mid_x = w / 2;
    int mid_y = h / 2;
    
    // Draw Axis
    _canvas->drawLine(0, mid_y, w, mid_y, 0x8410); // Grey
    _canvas->drawLine(mid_x, 0, mid_x, h, 0x8410);
    
    // Plot points
    int prev_px = -1, prev_py = -1;
    for (int i = 0; i < w; i++) {
        float x = (i - mid_x) / _zoom_x;
        float y = evaluate(_input_expr, x);
        
        if (!isnan(y)) {
            int py = mid_y - (int)(y * _zoom_y);
            if (py >= 0 && py < h) {
                if (prev_px != -1) {
                    _canvas->drawLine(prev_px, prev_py, i, py, TFT_YELLOW);
                }
                prev_px = i; prev_py = py;
            } else {
                prev_px = -1;
            }
        } else {
            prev_px = -1;
        }
    }
}

void AppMathPro::onRunning() {
    // [FUTURE FIX] UI Context Sandbox for App
    Hal::UIContextGuard guard;
    
    _canvas->fillScreen(TFT_BLACK);
    
    // --- Function Presets Logic (Sentinel v3.9) ---
    auto& key = GetHAL().keyboard.getLatestKeyEvent();
    if (key.state) {
        bool changed = false;
        if (key.keyCode == '1') { _input_expr = "x^2"; changed = true; }
        if (key.keyCode == '2') { _input_expr = "-x^2"; changed = true; }
        if (key.keyCode == '3') { _input_expr = "sin(x)"; changed = true; }
        if (key.keyCode == '4') { _input_expr = "cos(x)"; changed = true; }
        if (key.keyCode == '5') { _input_expr = "1/x"; changed = true; }
        
        if (changed) {
            analyzeFunction();
            GetHAL().keyboard.clearKeyEvent();
        }
    }

    plotFunction();
    
    // Info Overlay
    _canvas->setFont(&fonts::efontCN_12);
    _canvas->setTextColor(TFT_CYAN);
    _canvas->setCursor(5, 5);
    _canvas->printf("f(x) = %s", _input_expr.c_str());
    
    // Fix: Move layout up to avoid clipping
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setCursor(5, 105);
    _canvas->printf("性质: %s", _analysis_parity.c_str());
    _canvas->setCursor(110, 105);
    _canvas->printf("方向: %s", _analysis_direction.c_str());
    
    // Add Help Hints
    _canvas->setFont(&fonts::Font0);
    _canvas->setTextColor(0x7BEF);
    _canvas->drawCenterString("1-5: Presets  Arrows: Zoom  [BtnA]: Back", 102, 120);
    
    _canvas->pushSprite(0, 0);
    
    GetHAL().reportAppHealth();
    if (M5.BtnA.wasPressed()) close();
}

void AppMathPro::onClose() {
    if (_key_slot != -1) GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
    if (_canvas) {
        _canvas->deleteSprite();
        delete _canvas;
        _canvas = nullptr;
    }
}

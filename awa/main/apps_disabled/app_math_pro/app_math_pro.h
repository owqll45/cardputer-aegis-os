#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <string>
#include <vector>

class AppMathPro : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _key_slot = -1;
    
    std::string _input_expr = "x^2";
    float _zoom_x = 10.0f;
    float _zoom_y = 10.0f;
    float _offset_x = 0;
    float _offset_y = 0;
    
    // Analysis results
    std::string _analysis_parity = "Unknown";
    std::string _analysis_direction = "Unknown";
    
    void drawUI();
    void plotFunction();
    void analyzeFunction();
    float evaluate(const std::string& expr, float x);
    
public:
    AppMathPro();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

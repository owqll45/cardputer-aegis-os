#pragma once
#include <M5GFX.h>
#include "lgfx/v1/LGFX_Sprite.hpp"

class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();

    void init();
    void update();

    void setDisplay(M5GFX* display) { _display = display; }
    M5GFX& getDisplay() { return *_display; }
    lgfx::LGFX_Sprite& getCanvas() { return *_canvas; }
    lgfx::LGFX_Sprite& getUiCanvas() { return *_canvas_ui; }
    lgfx::LGFX_Sprite& getSystemBarCanvas() { return *_canvas_system_bar; }
    lgfx::LGFX_Sprite& getKeyboardBarCanvas() { return *_canvas_keyboard_bar; }

    void pushSystemBar();
    void pushKeyboardBar();
    void pushCanvas();

    bool isCanvasValid() const;
    void resetCanvas();

    /* 系统栏叠加层：全局通知条在推屏前画到系统栏画布上（不额外分配画布）。
     * 用函数指针注册，避免 hal 反向依赖 apps。 */
    using SystemBarOverlay = void (*)(lgfx::LGFX_Sprite& bar);
    void setSystemBarOverlay(SystemBarOverlay fn) { _bar_overlay = fn; }

    void setEmergencyMode(bool enable) { _is_emergency_mode = enable; }
    bool isEmergencyMode() const { return _is_emergency_mode; }

private:
    M5GFX* _display = nullptr;
    lgfx::LGFX_Sprite* _canvas = nullptr;
    lgfx::LGFX_Sprite* _canvas_ui = nullptr;
    lgfx::LGFX_Sprite* _canvas_system_bar = nullptr;
    lgfx::LGFX_Sprite* _canvas_keyboard_bar = nullptr;
    SystemBarOverlay _bar_overlay = nullptr;
    bool _is_emergency_mode = false;
};

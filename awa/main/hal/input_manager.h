#pragma once
#include "keyboard/keyboard.h"
#include <M5Unified.hpp>

class InputManager {
public:
    InputManager();
    ~InputManager();

    void init();
    void update();

    m5::Button_Class& getHomeButton() { return *_home_button; }
    Keyboard& getKeyboard() { return _keyboard; }

private:
    m5::Button_Class* _home_button = nullptr;
    Keyboard _keyboard;
};

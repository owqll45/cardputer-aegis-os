#pragma once
#include <mooncake.h>
#include "../../hal/hal.h"

class AppSentinelTest : public mooncake::AppAbility {
public:
    AppSentinelTest();
    ~AppSentinelTest();
    void onOpen() override;
    void onClose() override;
    void onRunning() override;

private:
    LGFX_Sprite* _canvas = nullptr;
    int _cursor_idx = 0;
    void _draw_stat(int x, int y, const char* label, const char* value, uint16_t color);
    void _handle_input(int& selected);
};

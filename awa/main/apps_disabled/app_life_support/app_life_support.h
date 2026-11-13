#pragma once
#include <mooncake.h>
#include "../../hal/hal.h"

class AppLifeSupport : public mooncake::AppAbility {
public:
    AppLifeSupport();
    ~AppLifeSupport();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void _draw_ui();

    LGFX_Sprite* _canvas = nullptr;
    int _selected_index  = 0;
};

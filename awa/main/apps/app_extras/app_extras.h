#pragma once
#include <mooncake.h>
#include <apps/utils/common.h>

// Helper macro for declaring simple apps
#define DECLARE_EXTRA_APP(ClassName) \
class ClassName : public mooncake::AppAbility { \
public: \
    ClassName(); \
    void onOpen() override; \
    void onRunning() override; \
    void onClose() override; \
};

DECLARE_EXTRA_APP(AppDino)
DECLARE_EXTRA_APP(AppCube3D)
DECLARE_EXTRA_APP(AppDice)
DECLARE_EXTRA_APP(AppTimer)
DECLARE_EXTRA_APP(AppStopwatch)
DECLARE_EXTRA_APP(AppFlashlight)
DECLARE_EXTRA_APP(AppMatrix)
DECLARE_EXTRA_APP(AppDrum)
DECLARE_EXTRA_APP(AppPiano)
DECLARE_EXTRA_APP(AppSketch)
DECLARE_EXTRA_APP(AppRuler)
DECLARE_EXTRA_APP(AppCoin)
DECLARE_EXTRA_APP(AppBreathing)
DECLARE_EXTRA_APP(AppMusicBox)
// AppMathPro is declared in its own header
DECLARE_EXTRA_APP(AppXiangqi)
DECLARE_EXTRA_APP(AppJunqi)

void install_extra_apps(mooncake::Mooncake* mooncake);

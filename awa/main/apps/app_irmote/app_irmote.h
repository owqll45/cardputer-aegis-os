#pragma once
#include <mooncake.h>
#include <cstdint>
#include <hal/hal.h>

/*
 * 红外遥控器（内置 NEC 码表）
 * 硬件只有 IR 发射（GPIO44），没有接收头，因此无法学习；提供预设品牌码 + 自定义 addr/cmd。
 */
class AppIrMote : public mooncake::AppAbility {
public:
    AppIrMote();
    ~AppIrMote();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    static const int NKEY = 6;
    int _key_idx = 0;
    int _brand = 0;
    bool _custom = false;
    int _addr = 0x00;
    int _cmd = 0x0C;
    volatile bool _send_req = false;
    int _key_slot = -1;
    bool _want_close = false;
    uint32_t _last_send_ms = 0;
    char _toast[32] = {0};
    uint32_t _toast_until = 0;

    LGFX_Sprite* _canvas = nullptr;

    void sendCurrent();
    void draw();
};

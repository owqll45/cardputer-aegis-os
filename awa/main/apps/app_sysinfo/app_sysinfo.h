#pragma once
#include <mooncake.h>
#include <cstdint>

/**
 * @brief 系统信息: 芯片/内存/Flash/电池/运行时间/MAC/固件版本, 可滚动查看
 */
class AppSysInfo : public mooncake::AppAbility {
public:
    AppSysInfo();
    ~AppSysInfo();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    int      _scroll   = 0;
    uint32_t _last     = 0;
    bool     _dirty    = true;
    bool     _exit_req = false;
    int      _key_slot = -1;

    void draw();
};

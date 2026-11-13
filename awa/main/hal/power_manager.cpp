#include "power_manager.h"
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include "../crash_handler.h"
#include <esp_timer.h>

static const std::string _tag = "Power";

PowerManager::PowerManager()
{
}

PowerManager::~PowerManager()
{
}

void PowerManager::init()
{
    mclog::tagInfo(_tag, "power init");
    _boot_time = lgfx::v1::millis();
}

void PowerManager::update()
{
    updatePowerSafety();
}

void PowerManager::updatePowerSafety()
{
    static uint32_t last_check = 0;
    if (lgfx::v1::millis() - last_check < 500) return;  // 2Hz check frequency (reduce overhead)
    last_check = lgfx::v1::millis();

    float v       = M5.Power.getBatteryVoltage() / 1000.0f;
    bool charging = (M5.Power.isCharging() == m5::Power_Class::is_charging);

    /* Cardputer ADV 走 pmic_adc, 没有充电状态寄存器, isCharging() 恒为 charge_unknown,
     * 电池端电压最高也只有 4.2V 左右。所以既不能靠 charging, 也不能靠 >4.8V 判断。
     * 唯一可靠的物理信号是插拔瞬间电池端电压的台阶(约 0.1~0.3V)。 */
    static float last_v     = 0;
    static float v_at_plug  = 0.0f;
    static uint32_t plug_time = 0;
    static bool pending_popup = false;
    static bool usb_in        = false;

    bool jump = (last_v > 2.0f && (v - last_v) > 0.12f);
    bool drop = (last_v > 2.0f && (last_v - v) > 0.12f);

    if (!usb_in && jump) {
        usb_in        = true;
        v_at_plug     = v;
        plug_time     = lgfx::v1::millis();
        pending_popup = true;
        mclog::tagInfo("Kernel", "USB plug detected, v={:.2f}V", v);
    } else if (usb_in && drop) {
        usb_in        = false;
        pending_popup = false;
        mclog::tagInfo("Kernel", "USB unplug detected, v={:.2f}V", v);
    }
    last_v = v;

    _power_status.voltage     = v;
    _power_status.is_charging = charging;
    _power_status.usb_plugged = usb_in;

    /* --- Boot Protection: Skip USB popups during first 30 seconds --- */
    if (lgfx::v1::millis() - _boot_time < 30000) {
        return;
    }

    /* 插入 30 秒后评估: 真在充电时电压会缓慢上升; 开关 ON 时电压基本不动。
     * 接近满电(>=4.15V)时本来就不会上升, 不提示, 避免误报。 */
    if (pending_popup && (lgfx::v1::millis() - plug_time > 30000)) {
        pending_popup = false;
        float rise    = v - v_at_plug;
        bool near_full = (v >= 4.15f);
        if (!near_full && rise < 0.01f) {
            mclog::tagWarn("Kernel", "USB in but not charging (v {:.2f} -> {:.2f}, rise {:.3f})",
                           v_at_plug, v, rise);
            crash_handler::show_system_popup(
                "充电路径异常",
                "检测到 USB 已插入但电池电压没有上升\n\n原因: 底部开关可能处于 ON\n建议: 拨至 OFF 以开启充电",
                crash_handler::POPUP_TYPE_WARNING, true);
        } else {
            mclog::tagInfo("Kernel", "USB power normal (v {:.2f} -> {:.2f}, rise {:.3f})",
                           v_at_plug, v, rise);
        }
    }
}

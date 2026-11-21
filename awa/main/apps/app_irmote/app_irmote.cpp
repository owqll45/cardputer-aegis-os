#include "app_irmote.h"
#include <apps/utils/app_canvas.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <hal/hal_config.h>
#include <utils/ir_nec/ir_helper.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 键盘回调只入队，状态和绘制只在 app 任务里做
#define IRQ_N 8
static int s_codes[IRQ_N];
static volatile int s_qh = 0, s_qt = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void irq_push(int code)
{
    taskENTER_CRITICAL(&s_mux);
    int n = (s_qh + 1) % IRQ_N;
    if (n != s_qt) {
        s_codes[s_qh] = code;
        s_qh = n;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static bool irq_pop(int& code)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_qt != s_qh) {
        code = s_codes[s_qt];
        s_qt = (s_qt + 1) % IRQ_N;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

static const char* kKeyNames[] = { "POWER", "VOL+", "VOL-", "CH+", "CH-", "MUTE" };

struct IrPreset {
    const char* name;
    uint8_t addr;
    uint8_t cmd[6];
};

// 通用 NEC 码表（不同品牌编码不同；无效时请切到自定义码）
static const IrPreset kPresets[] = {
    { "NEC-00", 0x00, { 0x0C, 0x18, 0x5A, 0x08, 0x1C, 0x10 } },
    { "NEC-04", 0x04, { 0x08, 0x02, 0x03, 0x07, 0x06, 0x09 } },
    { "NEC-10", 0x10, { 0x0C, 0x18, 0x5A, 0x08, 0x1C, 0x10 } },
    { "NEC-40", 0x40, { 0x0C, 0x18, 0x5A, 0x08, 0x1C, 0x10 } },
    { "NEC-7F", 0x7F, { 0x0C, 0x18, 0x5A, 0x08, 0x1C, 0x10 } },
};
static const int kPresetN = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

static bool s_ir_inited = false;

AppIrMote::AppIrMote() { setAppInfo().name = "IR Remote"; }
AppIrMote::~AppIrMote() {}

void AppIrMote::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "opened");

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    if (!s_ir_inited) {
        ir_helper_init((gpio_num_t)HAL_PIN_IR_TX);
        s_ir_inited = true;
        mclog::tagInfo(getAppInfo().name, "IR TX init on GPIO%d", HAL_PIN_IR_TX);
    }

    _key_idx = 0;
    _brand   = 0;
    _custom  = false;
    _addr    = 0x00;
    _cmd     = 0x0C;
    _toast[0] = 0;

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& k) {
        if (k.isModifier || !k.state) return;
        irq_push(k.keyCode);
    });

    draw();
}

void AppIrMote::onRunning()
{
    if (_want_close) {
        close();
        return;
    }
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }

    int code = 0;
    while (irq_pop(code)) {
        switch (code) {
            case KEY_ESC: _want_close = true; break;
            case KEY_LEFT: _key_idx = (_key_idx + NKEY - 1) % NKEY; break;
            case KEY_RIGHT: _key_idx = (_key_idx + 1) % NKEY; break;
            case KEY_UP:
                if (_custom) _cmd = (_cmd + 1) & 0xFF;
                else _brand = (_brand + 1) % kPresetN;
                break;
            case KEY_DOWN:
                if (_custom) _cmd = (_cmd + 255) & 0xFF;
                else _brand = (_brand + kPresetN - 1) % kPresetN;
                break;
            case KEY_C:
                _custom = !_custom;
                if (_custom) {
                    _addr = kPresets[_brand].addr;
                    _cmd  = kPresets[_brand].cmd[_key_idx];
                }
                break;
            case KEY_ENTER:
            case KEY_SPACE: _send_req = true; break;
            default: break;
        }
    }

    if (_send_req) {
        _send_req = false;
        sendCurrent();
    }

    draw();
}

void AppIrMote::onClose()
{
    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    appCanvasRestore(getAppInfo().name);
    _canvas = nullptr;
}

void AppIrMote::sendCurrent()
{
    uint32_t now = GetHAL().millis();
    if (now - _last_send_ms < 120) return;  // 防连发把 RMT 队列打满
    _last_send_ms = now;

    uint8_t addr, cmd;
    if (_custom) {
        addr = (uint8_t)_addr;
        cmd  = (uint8_t)_cmd;
    } else {
        addr = kPresets[_brand].addr;
        cmd  = kPresets[_brand].cmd[_key_idx];
    }

    ir_helper_send(addr, cmd);
    snprintf(_toast, sizeof(_toast), "SENT %02X/%02X", addr, cmd);
    _toast_until = now + 900;
    mclog::tagInfo(getAppInfo().name, "ir send %02X %02X", addr, cmd);
}

void AppIrMote::draw()
{
    if (!_canvas) return;
    auto& cv = *_canvas;

    cv.fillScreen(0x0841);
    cv.fillRect(0, 0, 240, 13, 0x1082);
    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(TFT_WHITE);
    cv.setCursor(4, 3);
    cv.print("IR REMOTE  (TX only)");
    cv.setTextColor(0x7BEF);
    cv.setCursor(192, 3);
    cv.print("Esc/G0");

    const IrPreset& pr = kPresets[_brand];
    cv.setTextColor(0x7BEF);
    cv.setCursor(6, 18);
    cv.print(_custom ? "CUSTOM  addr/cmd" : "BRAND");
    cv.setTextColor(TFT_YELLOW);
    cv.setFont(&fonts::Font2);
    cv.setCursor(6, 27);
    if (_custom) {
        char b[24];
        snprintf(b, sizeof(b), "%02X / %02X", _addr, _cmd);
        cv.print(b);
    } else {
        cv.print(pr.name);
    }

    cv.setFont(&fonts::efontCN_10);
    cv.setTextColor(0x7BEF);
    cv.setCursor(150, 18);
    cv.print("UP/DOWN 切换");
    cv.setCursor(150, 30);
    cv.print("C 自定义码");

    // 六个按键
    for (int i = 0; i < NKEY; i++) {
        int col = i % 3;
        int row = i / 3;
        int x = 8 + col * 76;
        int y = 44 + row * 32;
        int w = 68, h = 28;
        bool sel = (i == _key_idx);
        cv.fillRoundRect(x, y, w, h, 4, sel ? 0x2C7F : 0x10A2);
        cv.drawRoundRect(x, y, w, h, 4, sel ? TFT_YELLOW : 0x4A69);
        cv.setTextColor(sel ? TFT_WHITE : 0xBDD7);
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(middle_center);
        cv.drawString(kKeyNames[i], x + w / 2, y + h / 2);
        cv.setFont(&fonts::efontCN_10);
        cv.setTextColor(0x7BEF);
        cv.drawString(pr.name, x + w / 2, y + h - 4);
        cv.setTextDatum(top_left);
    }

    // 发送提示
    cv.setTextColor(TFT_GREEN);
    cv.setCursor(8, 106);
    if (_toast[0] && (int32_t)(_toast_until - GetHAL().millis()) > 0) {
        cv.print(_toast);
    } else {
        cv.setTextColor(0x7BEF);
        cv.print("ENTER 发射   只有发射头，无法学习红外码");
    }

    appCanvasPush(&cv);
}

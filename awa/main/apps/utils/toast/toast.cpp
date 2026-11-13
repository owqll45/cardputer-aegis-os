#include <apps/utils/toast/toast.h>
#include <hal/hal.h>
#include <M5GFX.h>
#include <esp_log.h>
#include <esp_system.h>
#include <atomic>

namespace ui_toast {

// ---------------- 提示条外观 ----------------
// 贴着屏幕最底部：整帧推屏是自上而下写的，提示条所在的行最后才被覆盖，
// 所以每帧"提示被 app 帧盖掉"的时间最短，肉眼几乎看不到闪烁。
static constexpr int kToastW = 120;
static constexpr int kToastH = 20;
static constexpr int kToastX = (240 - kToastW) / 2;
static constexpr int kToastY = 113;   // 113..132，最底部
static constexpr uint32_t kShowMs = 2000;   // 停留 2 秒

// ---------------- 任务间只传一个原子标志 ----------------
// -1 = 无请求，0 = 有声，1 = 静音
static std::atomic<int> s_pending{-1};

// 下面这些只在 app_task 里读写
static bool s_active = false;
static bool s_muted = false;
static uint32_t s_start_ms = 0;
static uint32_t s_last_push_ms = 0;
static lgfx::LGFX_Sprite* s_sprite = nullptr;

static bool ensureSprite()
{
    if (s_sprite) return true;

    auto* sp = new lgfx::LGFX_Sprite(&GetHAL().display());
    if (!sp) return false;
    sp->setPsram(false);
    sp->setColorDepth(16);
    if (!sp->createSprite(kToastW, kToastH)) {
        delete sp;
        ESP_LOGE("TOAST", "createSprite failed, heap=%u", (unsigned)esp_get_free_heap_size());
        return false;
    }
    s_sprite = sp;
    return true;
}

static void releaseSprite()
{
    if (!s_sprite) return;
    s_sprite->deleteSprite();
    delete s_sprite;
    s_sprite = nullptr;
}

static void paint(bool muted)
{
    // 静音: 红底白字; 有声: 绿底黑字 —— 一眼就能分清
    uint16_t bg = muted ? TFT_RED : TFT_GREEN;
    uint16_t fg = muted ? TFT_WHITE : TFT_BLACK;

    s_sprite->fillScreen(bg);
    s_sprite->drawRect(0, 0, kToastW, kToastH, TFT_WHITE);
    s_sprite->setFont(&fonts::efontCN_12);
    s_sprite->setTextDatum(middle_center);
    s_sprite->setTextColor(fg, bg);
    s_sprite->drawString(muted ? "静音" : "有声", kToastW / 2, kToastH / 2 + 1);
}

// 提示消失后把被盖住的内容刷回去。
// 提示是直接压在屏幕上的，没动过 canvas，所以把 canvas 重推一次就是原始画面。
static void restoreScreen()
{
    auto& dsp = GetHAL().display();
    auto& canvas = GetHAL().canvas();
    if (!canvas.getBuffer() || canvas.width() <= 0 || canvas.height() <= 0) return;

    // 画布贴屏位置：全高画布贴 (0,0)；240x109 画布贴在系统栏下面 (0,26)
    int y = (canvas.height() >= dsp.height()) ? 0 : (dsp.height() - canvas.height());
    canvas.pushSprite(0, y);
}

void notify(bool muted)
{
    // 只在键盘回调里执行：不画、不推屏、不分配内存
    s_pending.store(muted ? 1 : 0);
}

void update()
{
    uint32_t now = GetHAL().millis();

    int req = s_pending.exchange(-1);
    if (req >= 0) {
        if (ensureSprite()) {
            s_muted = (req == 1);
            s_active = true;
            s_start_ms = now;
            s_last_push_ms = 0;
            paint(s_muted);
            ESP_LOGI("TOAST", "show %s heap=%u", s_muted ? "MUTE" : "SOUND",
                     (unsigned)esp_get_free_heap_size());
        }
    }

    if (!s_active) return;

    if (now - s_start_ms >= kShowMs) {
        s_active = false;
        releaseSprite();     // 提示用的 4.7KB 也还回去
        restoreScreen();     // 重推当前画布，清掉提示、不留残影
        ESP_LOGI("TOAST", "hide heap=%u", (unsigned)esp_get_free_heap_size());
        return;
    }

    // 每个 app 帧都重压一次。限制最小间隔，防止静态界面空转时把 SPI 刷爆。
    if ((uint32_t)(now - s_last_push_ms) >= 8) {
        s_last_push_ms = now;
        s_sprite->pushSprite(kToastX, kToastY);
    }
}

}  // namespace ui_toast

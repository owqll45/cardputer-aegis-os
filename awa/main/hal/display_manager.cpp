#include "display_manager.h"
#include <M5GFX.h>
#include <M5Unified.hpp>
#include <mooncake_log.h>

DisplayManager::DisplayManager()
{
}

DisplayManager::~DisplayManager()
{
    if (_canvas) {
        _canvas->deleteSprite();
        delete _canvas;
        _canvas = nullptr;
    }
    if (_canvas_ui) {
        _canvas_ui->deleteSprite();
        delete _canvas_ui;
        _canvas_ui = nullptr;
    }
    if (_canvas_system_bar) {
        _canvas_system_bar->deleteSprite();
        delete _canvas_system_bar;
        _canvas_system_bar = nullptr;
    }
    if (_canvas_keyboard_bar) {
        _canvas_keyboard_bar->deleteSprite();
        delete _canvas_keyboard_bar;
        _canvas_keyboard_bar = nullptr;
    }
}

void DisplayManager::init()
{
    resetCanvas();
}

void DisplayManager::update()
{
}

void DisplayManager::pushSystemBar()
{
    if (_is_emergency_mode) return;
    if (_canvas_system_bar && _canvas_keyboard_bar)
        _canvas_system_bar->pushSprite(0, 0);  // x=0 全屏
}

void DisplayManager::pushKeyboardBar()
{
    if (_is_emergency_mode) return;
    if (_canvas_keyboard_bar)
        _canvas_keyboard_bar->pushSprite(0, 0);
}

void DisplayManager::pushCanvas()
{
    if (_canvas && _canvas_keyboard_bar && _canvas_system_bar)
        _canvas->pushSprite(0, _canvas_system_bar->height());  // x=0 全屏
}

bool DisplayManager::isCanvasValid() const
{
    return (_canvas && _canvas->width() > 0 && _canvas->height() > 0);
}

void DisplayManager::resetCanvas()
{
    if (!_display) {
        mclog::tagError("DisplayManager", "resetCanvas called but _display is null!");
        return;
    }

    // CRITICAL FIX: Reset display driver state before creating new sprites
    // This fixes LGFX_Sprite internal _panel being nullptr after boot_anim
    _display->startWrite();
    _display->endWrite();

    // Small delay to ensure display hardware is ready
    vTaskDelay(10 / portTICK_PERIOD_MS);

    // 只释放旧的缓冲区，保留 sprite 对象本身。
    // HalFacade 持有这些对象的指针，若 delete 后重建会导致悬空指针（第二次退出后卡死）
    if (_canvas) {
        _canvas->deleteSprite();
    } else {
        _canvas = new lgfx::LGFX_Sprite(_display);
    }
    if (_canvas_ui) {
        _canvas_ui->deleteSprite();
    } else {
        _canvas_ui = new lgfx::LGFX_Sprite(_display);
    }
    if (_canvas_system_bar) {
        _canvas_system_bar->deleteSprite();
    } else {
        _canvas_system_bar = new lgfx::LGFX_Sprite(_display);
    }
    if (_canvas_keyboard_bar) {
        _canvas_keyboard_bar->deleteSprite();
    } else {
        _canvas_keyboard_bar = new lgfx::LGFX_Sprite(_display);
    }

    // Create sprite buffers (this initializes internal _panel)
    // 恢复默认内存来源（应用可能把共享画布切到 PSRAM）
    _canvas->setPsram(false);
    _canvas_ui->setPsram(false);
    _canvas_keyboard_bar->setPsram(false);
    _canvas_system_bar->setPsram(false);

    // 关键: 先定色深再分配.
    // LGFX_Sprite::setColorDepth 在已有缓冲时会"删掉再按同样尺寸重分配",
    // 如果像以前那样先 createSprite(默认16位) 再 setColorDepth(16), 每次 reset 都要
    // 多分配/释放一次 52KB, 堆一碎片就会 create 失败且没人检查 -> 拿空画布继续画 -> 闪退.
    _canvas->setColorDepth(16);
    _canvas_ui->setColorDepth(16);
    _canvas_keyboard_bar->setColorDepth(16);
    _canvas_system_bar->setColorDepth(16);

    bool canvas_ok = _canvas->createSprite(_display->width(), 109);  // FULL WIDTH
    bool ui_ok = _canvas_ui->createSprite(1, 1);  // UNUSED: minimized (was 240x135 = 64KB waste)
    bool keyboard_ok = _canvas_keyboard_bar->createSprite(1, 1);  // Minimized (hidden)
    bool system_ok = _canvas_system_bar->createSprite(_display->width(), _display->height() - 109);  // FULL WIDTH

    // 主画布失败时再试一次(碎片整理后往往能成功), 避免拿到空缓冲继续绘图
    if (!canvas_ok) {
        vTaskDelay(20 / portTICK_PERIOD_MS);
        canvas_ok = _canvas->createSprite(_display->width(), 109);
    }

    // Verify creation succeeded
    if (!canvas_ok || !keyboard_ok || !system_ok || !ui_ok) {
        mclog::tagError("DisplayManager", "Failed to create sprites: canvas={} keyboard={} system={} ui={}",
                       canvas_ok, keyboard_ok, system_ok, ui_ok);
        // Don't return - try to continue anyway
    }

    // Initialize sprite contents (only if creation succeeded)
    if (canvas_ok) {
        _canvas->fillScreen(TFT_BLACK);
        _canvas->setClipRect(0, 0, _canvas->width(), _canvas->height());
    }
    if (ui_ok) {
        _canvas_ui->fillScreen(TFT_BLACK);
        _canvas_ui->setClipRect(0, 0, _canvas_ui->width(), _canvas_ui->height());
    }
    if (system_ok) {
        _canvas_system_bar->fillScreen(TFT_BLACK);
        _canvas_system_bar->setClipRect(0, 0, _canvas_system_bar->width(), _canvas_system_bar->height());
    }
    if (keyboard_ok) {
        _canvas_keyboard_bar->fillScreen(TFT_BLACK);
        _canvas_keyboard_bar->setClipRect(0, 0, _canvas_keyboard_bar->width(), _canvas_keyboard_bar->height());
    }

    mclog::tagInfo("DisplayManager", "Canvas reset complete: valid={}", isCanvasValid());
}

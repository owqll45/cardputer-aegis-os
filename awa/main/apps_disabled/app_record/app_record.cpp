#include "app_record.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

AppRecord::AppRecord() { setAppInfo().name = "Recorder"; }
AppRecord::~AppRecord() {}

void AppRecord::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _time_count = 0; _is_recording = true; _rec_idx = 0;
    _rec_data = (int16_t*)heap_caps_malloc(RECORD_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (_rec_data) memset(_rec_data, 0, RECORD_SIZE * sizeof(int16_t));
}

void AppRecord::onRunning() {
    auto* c = &GetHAL().canvas();
    c->fillScreen(TFT_BLACK);
    render_page_recording();
    render_waveform();
    handle_input();
    c->pushSprite(0, 0);
}

void AppRecord::onClose() {
    mclog::tagInfo(getAppInfo().name, "closed");
    if (_rec_data) { heap_caps_free(_rec_data); _rec_data = nullptr; }
}

void AppRecord::render_page_recording() {
    auto* c = &GetHAL().canvas();
    c->setTextColor(_is_recording ? TFT_RED : TFT_GREEN); c->setTextSize(1);
    c->setCursor(5, 5); c->print(_is_recording ? "REC ●" : "PLAY ▶");
}

void AppRecord::render_waveform() {
    auto* c = &GetHAL().canvas();
    int y_base = 30, height = 80;
    c->drawFastHLine(0, y_base + height / 2, 240, TFT_DARKGREY);
    for (int x = 0; x < 240 && x < RECORD_LENGTH; x++) {
        size_t idx = (_rec_idx * RECORD_LENGTH + x) % RECORD_SIZE;
        if (_rec_data && idx < RECORD_SIZE) {
            int y = y_base + height / 2 + (_rec_data[idx] * height / 2 / 32768);
            if (y < y_base) y = y_base;
            if (y >= y_base + height) y = y_base + height - 1;
            c->drawPixel(x, y, TFT_GREEN);
        }
    }
}

void AppRecord::handle_input() {
    // Unified exit: ESC or home button
    if (GetHAL().goButton().wasClicked()) { close(); return; }
    auto& kb = GetHAL().keyboard;
    if (kb.getLatestKeyEvent().state && (kb.getLatestKeyEvent().keyCode == KEY_ESC || kb.getLatestKeyEvent().keyCode == KEY_ENTER)) {
        kb.clearKeyEvent(); close(); return;
    }
}

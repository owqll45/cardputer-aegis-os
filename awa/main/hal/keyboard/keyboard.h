/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "keymap.h"
#include "../utils/adafruit_tca8418/Adafruit_TCA8418.h"
#include <mooncake_log_signal.h>

class Keyboard {
public:
    struct KeyEventRaw_t {
        bool state  = false;
        uint8_t row = 0;
        uint8_t col = 0;
    };

    struct KeyEvent_t {
        bool state           = false;
        bool isModifier      = false;
        KeScanCode_t keyCode = KEY_NONE;
        const char* keyName  = "";
    };

    mclog::Signal<const KeyEventRaw_t&> onKeyEventRaw;
    mclog::Signal<const KeyEvent_t&> onKeyEvent;

    bool init();
    void update();
    inline uint8_t getModifierMask()
    {
        return _modifier_mask;
    }
    inline bool isCapsLocked()
    {
        return _is_capslock_locked;
    }
    inline void setCapsLocked(bool locked)
    {
        _is_capslock_locked = locked;
    }
    inline const KeyEvent_t& getLatestKeyEvent()
    {
        return _key_event_buffer;
    }
    inline const KeyEventRaw_t& getLatestKeyEventRaw()
    {
        return _key_event_raw_buffer;
    }
    inline bool hasQueuedEvent() const
    {
        return _event_queue_idx < _event_queue.size();
    }
    inline const KeyEvent_t& dequeueEvent()
    {
        return _event_queue[_event_queue_idx++];
    }
    void clearKeyEvent();
    KeyEvent_t convertToKeyEvent(const KeyEventRaw_t& key);

private:
    Adafruit_TCA8418* _tca8418 = nullptr;
    uint8_t _modifier_mask     = 0;
    bool _capslock_state       = false;
    bool _is_capslock_locked   = false;
    KeyEventRaw_t _key_event_raw_buffer;
    KeyEvent_t _key_event_buffer;
    std::vector<KeyEvent_t> _event_queue;
    size_t _event_queue_idx = 0;

    KeyEventRaw_t get_key_event_raw(const uint8_t& eventRaw);
    void remap(KeyEventRaw_t& key);
    void update_modifier_mask(const KeyEventRaw_t& key);
};

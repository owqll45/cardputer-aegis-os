#pragma once
#include <hal/hal.h>
#include <freertos/FreeRTOS.h>

/*
 * 键盘事件跨任务搬运队列。
 * onKeyEvent 回调运行在 desktop_task，画布/棋盘只允许在 app 任务里碰。
 * 回调里只 push，onRunning 里 pop 后处理，杜绝跨任务画屏。
 */
class KeyEventQueue {
public:
    void push(const Keyboard::KeyEvent_t& e)
    {
        portENTER_CRITICAL(&_mux);
        int nt = (_tail + 1) % kCap;
        if (nt == _head) {
            _head = (_head + 1) % kCap;  // 满了丢最旧，绝不阻塞输入任务
        }
        _q[_tail] = e;
        _tail = nt;
        portEXIT_CRITICAL(&_mux);
    }

    bool pop(Keyboard::KeyEvent_t& out)
    {
        portENTER_CRITICAL(&_mux);
        if (_head == _tail) {
            portEXIT_CRITICAL(&_mux);
            return false;
        }
        out = _q[_head];
        _head = (_head + 1) % kCap;
        portEXIT_CRITICAL(&_mux);
        return true;
    }

    void clear()
    {
        portENTER_CRITICAL(&_mux);
        _head = _tail = 0;
        portEXIT_CRITICAL(&_mux);
    }

private:
    static constexpr int kCap = 8;
    Keyboard::KeyEvent_t _q[kCap];
    int _head = 0;
    int _tail = 0;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

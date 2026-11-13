/*
 * SerialPump - 系统级串口指令泵实现
 */
#include "serial_pump.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <mutex>
#include <esp_log.h>
#include <driver/usb_serial_jtag.h>
#include "hal/usb_serial_jtag_ll.h"

static const char* TAG = "SerialPump";

namespace SerialPump {

static Handler s_handler;
static std::mutex s_mutex;
static std::deque<std::string> s_pending;
static std::string s_line_buf;
static bool s_stdin_nonblock_set = false;

void setHandler(Handler h)
{
    std::lock_guard<std::mutex> lk(s_mutex);
    s_handler = h;
}

void clearHandler()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    s_handler = nullptr;
}

static void dispatch_line(const std::string& line)
{
    if (line.empty()) return;
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_handler) {
        s_handler(line);
    } else {
        // 排队等 LegacyApp 打开 (最多 8 条)
        if (s_pending.size() >= 8) s_pending.pop_front();
        s_pending.push_back(line);
        ESP_LOGI(TAG, "cmd queued (no handler): %zu", s_pending.size());
    }
}

void pump()
{
    // 确保 stdin 非阻塞
    if (!s_stdin_nonblock_set) {
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        int setret = fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        ESP_LOGI(TAG, "stdin nonblock: flags=0x%x setret=%d", flags, setret);
        s_stdin_nonblock_set = true;
    }

    uint8_t buf[128];
    // 终极方案: 直接读硬件 RXFIFO (绕过驱动/ISR/M5 污染, ROM 级硬件访问)
    // 同时使能 RX 中断让硬件持续接收
    static bool hw_inited = false;
    if (!hw_inited) {
        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        hw_inited = true;
    }
    int loops = 16; // 上限 2KB/帧 防饿死
    while (loops-- > 0) {
        int r = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf) - 1);
        if (r <= 0) return; // FIFO 空
        ESP_LOGI(TAG, "RX %d bytes", (int)r);

        for (int i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                dispatch_line(s_line_buf);
                s_line_buf.clear();
            } else {
                if (s_line_buf.size() < 512) s_line_buf += c;
            }
        }
    }
}

std::string popPending()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_pending.empty()) return "";
    std::string cmd = s_pending.front();
    s_pending.pop_front();
    return cmd;
}

size_t pendingCount()
{
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_pending.size();
}

} // namespace SerialPump

// 业务级日志: 直接写 USB 硬件, 绕过驱动 printf (只记关键事件)
static const char* PUMP_TAG = "CmdLine";
void pumpNote(const char* fmt, ...)
{
    char buf[192];
    int n = snprintf(buf, sizeof(buf), "[CMD] ");
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
    va_end(args);
    if (n > 0 && n < (int)sizeof(buf) - 2) {
        buf[n++] = '\n';
        usb_serial_jtag_write_bytes(buf, n, 20 / portTICK_PERIOD_MS);
    }
}

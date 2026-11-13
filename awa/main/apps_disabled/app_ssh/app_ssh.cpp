#include "app_ssh.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <cstring>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

static uint32_t app_millis() { return esp_timer_get_time() / 1000; }

AppSSH::AppSSH() { setAppInfo().name = "TCP Client"; }
AppSSH::~AppSSH() {}

void AppSSH::onOpen() {
    mclog::tagInfo(getAppInfo().name, "opened");
    _sock = -1; _connected = false; _input_mode = true; _input_buffer = "192.168.1.1"; _terminal_buffer.clear();
}

void AppSSH::onRunning() {
    // Unified exit: ESC or home button
    if (GetHAL().goButton().wasClicked()) { close(); return; }
    auto& kb = GetHAL().keyboard;
    if (kb.getLatestKeyEvent().state && (kb.getLatestKeyEvent().keyCode == KEY_ESC || kb.getLatestKeyEvent().keyCode == KEY_ENTER)) {
        kb.clearKeyEvent(); close(); return;
    }
    
    auto* c = &GetHAL().canvas();
    c->fillScreen(TFT_BLACK);
    if (_connected) receiveData();
    drawUI();
    handleInput();
    c->pushSprite(0, 0);
}

void AppSSH::onClose() { mclog::tagInfo(getAppInfo().name, "closed"); disconnect(); }

void AppSSH::drawUI() {
    auto* c = &GetHAL().canvas();
    c->setTextSize(1);
    if (_input_mode) {
        c->setTextColor(TFT_CYAN); c->setCursor(5, 5); c->print("TCP Client");
        c->setTextColor(TFT_WHITE); c->setCursor(5, 25); c->print("Host: "); c->print(_input_buffer.c_str());
        c->setCursor(5, 50); c->print("[ENTER] Connect  [ESC] Back");
    } else {
        c->setTextColor(TFT_GREEN); c->setCursor(5, 3); c->printf("Connected to %s", _host_ip.c_str());
        c->setTextColor(TFT_WHITE);
        int y = 18, lines = 0; int len = _terminal_buffer.length(); int start = len > 500 ? len - 500 : 0;
        for (int i = start; i < len && lines < 7; i++) {
            if (_terminal_buffer[i] == '\n' || i == start) {
                c->setCursor(5, y);
                int end = _terminal_buffer.find('\n', i + 1); if (end < 0) end = len;
                c->print(_terminal_buffer.substr(i + (i != start ? 1 : 0), end - i - (i != start ? 1 : 0)).c_str());
                y += 14; lines++;
            }
        }
    }
}

void AppSSH::handleInput() {
    if (_input_mode) {
        // Use signal-based keyboard
        // For simplicity, just check latest event
    } else {
        // Terminal mode - check ESC
    }
}

void AppSSH::connectToServer() {
    _host_ip = _input_buffer; _port = 23;
    _sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_sock < 0) { _terminal_buffer = "Socket failed"; return; }
    struct hostent *he = gethostbyname(_host_ip.c_str());
    if (!he) { _terminal_buffer = "DNS failed"; ::close(_sock); _sock = -1; return; }
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(_port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    if (connect(_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        _terminal_buffer = "Connect failed"; ::close(_sock); _sock = -1; return;
    }
    _connected = true; _input_mode = false; _terminal_buffer = "Connected!\r\n";
}

void AppSSH::disconnect() { if (_sock >= 0) { ::close(_sock); _sock = -1; } _connected = false; }
void AppSSH::sendData(const char* data, int len) { if (_connected && _sock >= 0) send(_sock, data, len, 0); }
void AppSSH::receiveData() {
    if (_sock < 0) return;
    char buf[256]; int len = recv(_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (len > 0) { buf[len] = '\0'; _terminal_buffer += buf; if (_terminal_buffer.length() > 2000) _terminal_buffer = _terminal_buffer.substr(_terminal_buffer.length() - 1000); }
    else if (len == 0) { disconnect(); _input_mode = true; _terminal_buffer += "\r\n[Disconnected]"; }
}

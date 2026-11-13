#pragma once
#include <mooncake.h>
#include <apps/utils/common.h>
#include <lwip/sockets.h>
#include <string>

class AppSSH : public mooncake::AppAbility {
public:
    AppSSH();
    ~AppSSH();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    int _sock = -1;
    bool _connected = false;
    std::string _host_ip = "192.168.1.1";
    int _port = 22; // Or 23 for Telnet, default 22 just for show (though we do raw TCP)
    
    // UI State
    bool _input_mode = true; // True=Input IP, False=Terminal
    std::string _input_buffer;
    std::string _terminal_buffer;
    int _cursor_pos = 0;
    
    void drawUI();
    void handleInput();
    void connectToServer();
    void disconnect();
    void sendData(const char* data, int len);
    void receiveData();
};

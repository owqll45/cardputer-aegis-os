#pragma once

class SystemInit {
public:
    static SystemInit& getInstance();

    void init();
    void update();

    bool isSetupComplete() const { return _setup_done; }
    bool needsFormatPrompt() const { return _needs_format; }

    void setFormatHandled() { _needs_format = false; }

private:
    SystemInit() = default;
    void showSetupScreen();
    void showFormatPrompt();

    bool _setup_done = false;
    bool _needs_format = false;
};

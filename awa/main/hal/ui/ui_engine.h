#pragma once
#include <string>
#include <vector>
#include <functional>
#include <M5GFX.h>
#include <lgfx/v1/LGFX_Sprite.hpp>
#include "../chat/message.h"

enum class UiScreen { CHAT, MENU, SETTINGS, HISTORY, SETUP, INPUT };

struct MenuItem {
    std::string label;
    std::string value;
    bool selected = false;
    bool is_header = false;
};

class UiEngine {
public:
    static UiEngine& getInstance();

    void init();
    void update();

    void setScreen(UiScreen s);
    UiScreen getCurrentScreen() const { return _screen; }

    void showChat();
    void showMenu(const std::vector<MenuItem>& items, std::function<void(int)> on_select);
    void showSettings();
    void showHistory();
    void showSetupPrompt();

    void setWaitingForResponse(bool waiting) { _waiting = waiting; }
    bool isWaiting() const { return _waiting; }

    void notify(const std::string& msg, int duration_ms = 2000);

    void renderChatMessages(const std::vector<ChatMessage>& messages, int scroll_offset = 0);
    void renderInputBar(const std::string& text, int cursor_pos);
    void renderStatusBar(int tokens, int context_count);
    void renderThinking(const std::string& thinking_text);
    void pushDisplay();

    void scrollUp() { if (_scroll > 0) _scroll--; }
    void scrollDown() { _scroll++; }
    int getScroll() const { return _scroll; }
    void resetScroll() { _scroll = 0; }
    void setAutoScroll(bool v) { _auto_scroll = v; }

private:
    UiEngine() = default;

    void drawBlinkingDot();
    void drawBatteryIcon(int x, int y, int level);
    void drawWifiIcon(int x, int y, bool connected);
    void drawMenuItems(const std::vector<MenuItem>& items, int sel_idx);
    int wrapText(const std::string& text, int max_chars, std::vector<std::string>& out_lines);
    int renderMessageBlock(const ChatMessage& m, int y, int max_y, int scroll_skip);

    UiScreen _screen = UiScreen::CHAT;
    bool _waiting = false;
    bool _auto_scroll = true;
    int _scroll = 0;
    int _menu_sel = 0;
    uint32_t _blink_timer = 0;
    bool _blink_state = false;
    std::string _notify_msg;
    uint32_t _notify_until = 0;
    int _last_msg_count = 0;

    std::vector<MenuItem> _menu_items;
    std::function<void(int)> _menu_callback;
};

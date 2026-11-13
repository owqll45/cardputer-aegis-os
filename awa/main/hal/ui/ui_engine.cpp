#include "ui_engine.h"
#include "config_store.h"
#include "chat_engine.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <M5Unified.h>
#include <cstring>

static const std::string TAG = "UiEngine";
static const int SCREEN_W = 240;
static const int SCREEN_H = 135;
static const int STATUS_H = 13;
static const int INPUT_H = 16;
static const int CHAT_TOP = STATUS_H + 1;
static const int CHAT_BOTTOM = SCREEN_H - INPUT_H - 1;
static const int CHAT_H = CHAT_BOTTOM - CHAT_TOP;
static const int LINE_H = 11;
static const int CHARS_PER_LINE = 28;

static bool hasChinese(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0xC0) return true;
    }
    return false;
}

static void setFontForText(M5GFX& d, const std::string& text) {
    if (hasChinese(text)) {
        d.setFont(&fonts::efontCN_12);
    } else {
        d.setFont(&fonts::Font0);
    }
}

UiEngine& UiEngine::getInstance() {
    static UiEngine instance;
    return instance;
}

void UiEngine::init() {
    mclog::tagInfo(TAG, "init");
    _screen = UiScreen::CHAT;
    _auto_scroll = true;
}

void UiEngine::update() {
    uint32_t now = esp_timer_get_time() / 1000;
    if (_blink_state) { if (now - _blink_timer > 400) { _blink_timer = now; _blink_state = false; } }
    else              { if (now - _blink_timer > 400) { _blink_timer = now; _blink_state = true;  } }
    if (_notify_until > 0 && now > _notify_until) { _notify_msg.clear(); _notify_until = 0; }
}

void UiEngine::setScreen(UiScreen s) { _screen = s; _scroll = 0; _menu_sel = 0; }
void UiEngine::showChat() { setScreen(UiScreen::CHAT); }
void UiEngine::showHistory() { setScreen(UiScreen::HISTORY); }
void UiEngine::showSetupPrompt() { setScreen(UiScreen::SETUP); }
void UiEngine::showSettings() { setScreen(UiScreen::SETTINGS); }

void UiEngine::showMenu(const std::vector<MenuItem>& items, std::function<void(int)> on_select) {
    _menu_items = items; _menu_callback = on_select; _menu_sel = 0;
    setScreen(UiScreen::MENU);
}

void UiEngine::notify(const std::string& msg, int duration_ms) {
    _notify_msg = msg; _notify_until = (esp_timer_get_time() / 1000) + duration_ms;
}

void UiEngine::drawBatteryIcon(int x, int y, int level) {
    auto& d = GetHAL().display();
    d.drawRect(x, y + 1, 16, 8, TFT_WHITE);
    d.fillRect(x + 16, y + 3, 2, 4, TFT_WHITE);
    if (level > 75) {
        d.fillRect(x + 1, y + 2, 14, 6, TFT_GREEN);
    } else if (level > 50) {
        d.fillRect(x + 1, y + 2, 10, 6, TFT_GREEN);
    } else if (level > 25) {
        d.fillRect(x + 1, y + 2, 7, 6, TFT_YELLOW);
    } else if (level > 10) {
        d.fillRect(x + 1, y + 2, 4, 6, TFT_ORANGE);
    } else {
        d.fillRect(x + 1, y + 2, 2, 6, TFT_RED);
    }
}

void UiEngine::drawWifiIcon(int x, int y, bool connected) {
    auto& d = GetHAL().display();
    uint16_t col = connected ? TFT_GREEN : TFT_DARKGREY;
    d.fillRect(x, y, 2, 2, col);
    d.fillRect(x - 2, y - 2, 6, 2, col);
    d.fillRect(x - 4, y - 4, 10, 2, col);
    d.fillRect(x - 6, y - 6, 14, 2, col);
}

void UiEngine::drawBlinkingDot() {
    if (_waiting && _blink_state) {
        auto& d = GetHAL().display();
        d.fillCircle(SCREEN_W - 30, 6, 3, TFT_GREEN);
    }
}

void UiEngine::renderStatusBar(int tokens, int context_count) {
    auto& d = GetHAL().display();
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x0D1B2A);
    d.drawLine(0, STATUS_H, SCREEN_W, STATUS_H, 0x1B3A5C);

    auto& cfg = ConfigStore::getInstance().getAssistantConfig();
    d.setTextColor(TFT_CYAN, 0x0D1B2A);
    d.setTextSize(1);
    d.setCursor(2, 2);
    std::string name = cfg.name;
    if (name.length() > 8) name = name.substr(0, 8) + "..";
    setFontForText(d, name);
    d.print(name.c_str());

    d.setTextColor(TFT_DARKGREY, 0x0D1B2A);
    d.setCursor(70, 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "T:%d C:%d", tokens, context_count);
    d.print(buf);

    bool wifi_conn = GetHAL().isWifiConnected();
    drawWifiIcon(SCREEN_W - 50, 8, wifi_conn);

    int bat = M5.Power.getBatteryLevel();
    drawBatteryIcon(SCREEN_W - 22, 2, bat);

    drawBlinkingDot();
}

int UiEngine::wrapText(const std::string& text, int max_chars, std::vector<std::string>& out_lines) {
    out_lines.clear();
    if (text.empty()) { out_lines.push_back(""); return 1; }

    size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == '\n') {
            out_lines.push_back("");
            pos++;
            continue;
        }

        int line_len = 0;
        int char_count = 0;
        size_t start = pos;
        size_t last_space = std::string::npos;
        size_t last_space_pos = pos;

        while (pos < text.size() && char_count < max_chars) {
            unsigned char c = text[pos];
            if (c == '\n') break;

            if (c >= 0xC0) {
                int bytes = 1;
                if (c >= 0xF0) bytes = 3;
                else if (c >= 0xE0) bytes = 2;
                line_len += 2;
                char_count += 2;
                pos += bytes + 1;
                last_space = pos;
                last_space_pos = pos;
            } else if (c == ' ') {
                line_len++;
                char_count++;
                last_space = pos;
                last_space_pos = pos;
                pos++;
            } else {
                line_len++;
                char_count++;
                pos++;
            }
        }

        if (pos < text.size() && text[pos] != '\n' && last_space != std::string::npos && char_count >= max_chars) {
            out_lines.push_back(text.substr(start, last_space_pos - start));
            pos = last_space_pos + 1;
        } else {
            out_lines.push_back(text.substr(start, pos - start));
        }
    }
    return (int)out_lines.size();
}

int UiEngine::renderMessageBlock(const ChatMessage& m, int y, int max_y, int scroll_skip) {
    auto& d = GetHAL().display();
    auto& cfg = ConfigStore::getInstance().getAssistantConfig();

    if (m.role == MsgRole::THINKING && !cfg.show_thinking) return y;

    uint16_t role_bg, role_fg;
    const char* role_label;
    uint16_t text_fg;

    if (m.role == MsgRole::USER) {
        role_bg = 0x210F; role_fg = TFT_WHITE; role_label = "你"; text_fg = 0x367F;
    } else if (m.role == MsgRole::THINKING) {
        role_bg = 0x05E0; role_fg = TFT_GREEN; role_label = "思考"; text_fg = 0x4BBD;
    } else {
        role_bg = 0x1A25; role_fg = TFT_WHITE; role_label = "AI"; text_fg = TFT_WHITE;
        if (m.state == MsgState::STREAMING) text_fg = TFT_YELLOW;
        else if (m.state == MsgState::ERROR) text_fg = TFT_RED;
    }

    if (y >= max_y) return y;

    d.setTextColor(role_fg, role_bg);
    d.setTextSize(1);
    d.setCursor(2, y);
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "[%s]", role_label);
    setFontForText(d, std::string(role_label));
    int hdr_w = strlen(hdr) * 6 + 4;
    d.fillRect(1, y, hdr_w, LINE_H - 1, role_bg);
    d.print(hdr);

    if (m.token_count > 0) {
        d.setTextColor(TFT_DARKGREY, TFT_BLACK);
        d.setCursor(SCREEN_W - 30, y);
        char tk[16];
        snprintf(tk, sizeof(tk), "%dt", m.token_count);
        d.print(tk);
    }

    y += LINE_H;

    std::string display_text = m.content;
    if (m.role == MsgRole::THINKING) display_text = m.thinking_content;

    if (m.role == MsgRole::ASSISTANT && !m.thinking_content.empty() && cfg.show_thinking) {
        if (m.state == MsgState::STREAMING && display_text.empty()) {
            std::vector<std::string> think_lines;
            wrapText(m.thinking_content, CHARS_PER_LINE - 2, think_lines);
            int think_show = std::min((int)think_lines.size(), 3);
            for (int i = 0; i < think_show && y < max_y; i++) {
                d.setTextColor(0x4CAF50, 0x0A1A0A);
                d.setCursor(6, y);
                d.fillRect(4, y, SCREEN_W - 8, LINE_H - 1, 0x0A1A0A);
                std::string line = think_lines[i];
                if (line.length() > (size_t)(CHARS_PER_LINE - 4))
                    line = line.substr(0, CHARS_PER_LINE - 4) + "..";
                setFontForText(d, line);
                d.print(line.c_str());
                y += LINE_H;
            }
            if (think_lines.size() > 3 && y < max_y) {
                d.setTextColor(0x2E7D32, TFT_BLACK);
                d.setCursor(6, y);
                d.setFont(&fonts::efontCN_12);
                d.printf("...+%d行", (int)(think_lines.size() - 3));
                y += LINE_H;
            }
            d.drawLine(4, y, SCREEN_W - 4, y, 0x1B5E20);
            y += 2;
            return y;
        }
    }

    if (display_text.empty() && m.state == MsgState::STREAMING) {
        d.setTextColor(TFT_YELLOW, TFT_BLACK);
        d.setCursor(6, y);
        if (_blink_state) d.print("...");
        y += LINE_H;
        return y;
    }

    std::vector<std::string> lines;
    wrapText(display_text, CHARS_PER_LINE, lines);

    int lines_to_skip = scroll_skip;
    for (auto& line : lines) {
        if (lines_to_skip > 0) { lines_to_skip--; continue; }
        if (y >= max_y) break;
        d.setTextColor(text_fg, TFT_BLACK);
        d.setCursor(4, y);
        std::string disp = line;
        if (disp.length() > (size_t)CHARS_PER_LINE)
            disp = disp.substr(0, CHARS_PER_LINE);
        setFontForText(d, disp);
        d.print(disp.c_str());
        y += LINE_H;
    }

    y += 2;
    return y;
}

void UiEngine::renderChatMessages(const std::vector<ChatMessage>& messages, int scroll_offset) {
    auto& d = GetHAL().display();
    auto& cfg = ConfigStore::getInstance().getAssistantConfig();

    d.fillScreen(TFT_BLACK);
    renderStatusBar(ChatEngine::getInstance().getTotalTokens(), ChatEngine::getInstance().getContextCount());

    if (_auto_scroll && (int)messages.size() != _last_msg_count) {
        _scroll = 0;
        _last_msg_count = messages.size();
    }

    int y = CHAT_TOP;
    int max_y = CHAT_BOTTOM;

    int total_lines = 0;
    for (auto& m : messages) {
        if (m.role == MsgRole::THINKING && !cfg.show_thinking) continue;
        std::string txt = m.content;
        if (m.role == MsgRole::THINKING) txt = m.thinking_content;
        std::vector<std::string> lines;
        wrapText(txt, CHARS_PER_LINE, lines);
        total_lines += (int)lines.size() + 2;
    }

    int max_scroll = std::max(0, total_lines - (CHAT_H / LINE_H));
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;

    int visible_lines = CHAT_H / LINE_H;
    int lines_remaining = visible_lines;

    for (size_t i = 0; i < messages.size() && y < max_y; i++) {
        auto& m = messages[i];
        if (m.role == MsgRole::THINKING && !cfg.show_thinking) continue;
        y = renderMessageBlock(m, y, max_y, 0);
    }

    if (!_notify_msg.empty()) {
        int tw = _notify_msg.length() * 6 + 16;
        int nx = (SCREEN_W - tw) / 2;
        int ny = CHAT_TOP + CHAT_H / 2 - 6;
        d.fillRect(nx, ny, tw, LINE_H + 4, 0x065F46);
        d.drawRect(nx, ny, tw, LINE_H + 4, TFT_GREEN);
        d.setTextColor(TFT_GREEN, 0x065F46);
        d.setTextSize(1);
        d.setCursor(nx + 8, ny + 2);
        setFontForText(d, _notify_msg);
        d.print(_notify_msg.c_str());
    }
}

void UiEngine::renderInputBar(const std::string& text, int cursor_pos) {
    auto& d = GetHAL().display();
    d.fillRect(0, SCREEN_H - INPUT_H, SCREEN_W, INPUT_H, 0x1C2833);
    d.drawLine(0, SCREEN_H - INPUT_H, SCREEN_W, SCREEN_H - INPUT_H, 0x2C3E50);

    d.setTextColor(TFT_GREEN, 0x1C2833);
    d.setTextSize(1);
    d.setCursor(2, SCREEN_H - INPUT_H + 3);

    std::string visible = text;
    int max_chars = 30;
    if ((int)visible.length() > max_chars) {
        visible = visible.substr(visible.length() - max_chars);
    }
    d.print("> ");
    setFontForText(d, visible);
    d.print(visible.c_str());

    if (_blink_state) {
        int cx = 14 + visible.length() * 6;
        if (cx < SCREEN_W - 10) {
            d.drawLine(cx, SCREEN_H - INPUT_H + 3, cx, SCREEN_H - INPUT_H + 11, TFT_GREEN);
        }
    }

    d.setTextColor(0x4A5568, 0x1C2833);
    d.setCursor(SCREEN_W - 36, SCREEN_H - INPUT_H + 3);
    d.print("ENT");
}

void UiEngine::renderThinking(const std::string& thinking_text) {
    auto& d = GetHAL().display();
    d.fillRect(4, CHAT_BOTTOM - LINE_H * 3, SCREEN_W - 8, LINE_H * 3, 0x0A1A0A);
    d.setTextColor(0x4CAF50, 0x0A1A0A);
    d.setTextSize(1);
    d.setCursor(6, CHAT_BOTTOM - LINE_H * 3);

    std::string display = thinking_text;
    if (display.length() > 80) display = "..." + display.substr(display.length() - 77);
    std::vector<std::string> lines;
    wrapText(display, CHARS_PER_LINE - 2, lines);
    int show = std::min((int)lines.size(), 2);
    for (int i = 0; i < show; i++) {
        d.setCursor(6, CHAT_BOTTOM - LINE_H * 3 + i * LINE_H);
        setFontForText(d, lines[i]);
        d.print(lines[i].c_str());
    }
}

void UiEngine::pushDisplay() {
}

void UiEngine::drawMenuItems(const std::vector<MenuItem>& items, int sel_idx) {
    auto& d = GetHAL().display();
    d.setTextSize(1);
    int y = CHAT_TOP;
    for (size_t i = 0; i < items.size(); i++) {
        int cy = y + (int)i * 14;
        if (cy >= CHAT_BOTTOM) break;
        bool selected = ((int)i == sel_idx);
        setFontForText(d, items[i].label);
        if (items[i].is_header) {
            d.setTextColor(TFT_CYAN, TFT_BLACK);
            d.setCursor(4, cy);
            d.print(items[i].label.c_str());
        } else if (selected) {
            d.fillRect(0, cy, SCREEN_W, 13, 0x1565C0);
            d.setTextColor(TFT_WHITE, 0x1565C0);
            d.setCursor(8, cy + 2);
            d.print("> ");
            d.print(items[i].label.c_str());
        } else {
            d.setTextColor(TFT_LIGHTGRAY, TFT_BLACK);
            d.setCursor(12, cy + 2);
            d.print(items[i].label.c_str());
        }
    }
}

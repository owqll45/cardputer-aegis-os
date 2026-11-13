// Minimal Chat View implementation
#include "chat_view.h"

ChatView::~ChatView() {}

void ChatView::init() {}
void ChatView::update() {}
void ChatView::addMessage(const std::string& message, bool is_sent) {}
void ChatView::handle_key_event(const Keyboard::KeyEvent_t& keyEvent) {}
void ChatView::render_chat_interface() {}
void ChatView::render_input_panel() {}
void ChatView::send_message() {
    if (!_input_buffer.empty()) {
        if (onSendMsg) onSendMsg(_input_buffer);
        _input_buffer.clear();
    }
}
void ChatView::update_cursor() {}

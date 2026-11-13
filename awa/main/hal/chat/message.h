#pragma once
#include <string>
#include <vector>

enum class MsgRole { USER, ASSISTANT, SYSTEM, THINKING, TOOL };
enum class MsgState { SENDING, STREAMING, COMPLETE, ERROR };

struct ChatMessage {
    std::string id;
    MsgRole role = MsgRole::USER;
    std::string content;
    std::string thinking_content;
    MsgState state = MsgState::COMPLETE;
    int64_t timestamp = 0;
    int token_count = 0;
    std::string model_used;
    bool collapsed = false;

    std::string toJson() const;
    static ChatMessage fromJson(const std::string& json);
};

struct Conversation {
    std::string id;
    std::string title;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::vector<ChatMessage> messages;
    int total_tokens = 0;

    std::string toJson() const;
    static Conversation fromJson(const std::string& json);
};

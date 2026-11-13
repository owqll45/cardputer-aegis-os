#pragma once
#include <string>
#include <functional>
#include "../chat/message.h"

struct StreamChunk {
    std::string content;
    std::string thinking_content;
    bool is_thinking = false;
    bool is_done = false;
    bool is_error = false;
    std::string error_msg;
    int tokens_used = 0;
};

class AiEngine {
public:
    static AiEngine& getInstance();

    void init();
    bool isReady() const { return _ready; }

    std::string chat(const std::string& user_message, const Conversation* context = nullptr);
    bool chatStream(const std::string& user_message, const Conversation* context,
                    std::function<void(const StreamChunk&)> on_chunk);

    std::string stt(const uint8_t* audio_data, size_t len);
    std::string tts(const std::string& text, uint8_t** out_audio, size_t* out_len);

    std::string searchBing(const std::string& query);
    std::string searchTavily(const std::string& query);
    std::string executeJs(const std::string& code);

private:
    AiEngine() = default;
    bool _ready = false;

    bool callApi(const std::string& endpoint, const char* body, size_t body_len,
                 std::string& response, int timeout_ms = 30000);

    std::string buildChatBody(const std::string& prompt, const Conversation* ctx);
    std::string buildSystemPrompt();
};

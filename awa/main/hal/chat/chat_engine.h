#pragma once
#include <string>
#include <vector>
#include <functional>
#include "message.h"

class ChatEngine {
public:
    static ChatEngine& getInstance();

    void init();
    void update();

    void newConversation();
    Conversation* getCurrentConversation();
    const Conversation* getCurrentConversation() const;
    const std::vector<Conversation>& getConversations() const { return _conversations; }
    bool loadConversation(const std::string& id);
    bool deleteConversation(const std::string& id);

    void sendMessage(const std::string& text);
    bool isProcessing() const { return _processing; }

    void setOnMessageUpdate(std::function<void()> cb) { _on_update = cb; }
    void setOnNewMessage(std::function<void(const ChatMessage&)> cb) { _on_new_msg = cb; }
    void setOnThinkingUpdate(std::function<void(const std::string&)> cb) { _on_thinking = cb; }
    void setOnComplete(std::function<void(bool)> cb) { _on_complete = cb; }
    void setOnAskUser(std::function<void(const std::string&, const std::vector<std::string>&)> cb) { _on_ask_user = cb; }

    int getTotalTokens() const;
    int getContextCount() const;

    bool _processing = false;
    std::function<void()> _on_update;
    std::function<void(const ChatMessage&)> _on_new_msg;
    std::function<void(const std::string&)> _on_thinking;
    std::function<void(bool)> _on_complete;

private:
    ChatEngine() = default;

    std::string generateId();
    void saveCurrentConversation();
    void saveAllMetadata();
    bool saveConversationToSd(const Conversation& conv);
    bool saveMetadataToSd();
    void loadAllFromStorage();

    std::vector<Conversation> _conversations;
    int _current_idx = -1;

    std::function<void(const std::string&, const std::vector<std::string>&)> _on_ask_user;
};

#include "chat_engine.h"
#include "ai_engine.h"
#include "config_store.h"
#include "sd_card_manager.h"
#include "nvs_helper.h"
#include <mooncake_log.h>
#include <cJSON.h>
#include <ctime>
#include <cstdlib>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>

static const std::string TAG = "ChatEngine";

ChatEngine& ChatEngine::getInstance() {
    static ChatEngine instance;
    return instance;
}

void ChatEngine::init() {
    mclog::tagInfo(TAG, "init");
    loadAllFromStorage();
    if (_conversations.empty()) {
        newConversation();
    } else {
        _current_idx = 0;
        mclog::tagInfo(TAG, "Loaded {} conversations", _conversations.size());
    }
}

std::string ChatEngine::generateId() {
    char buf[20];
    snprintf(buf, sizeof(buf), "%08x%04x", (unsigned)time(nullptr), (unsigned)(rand() & 0xFFFF));
    return std::string(buf);
}

void ChatEngine::newConversation() {
    saveCurrentConversation();

    Conversation conv;
    conv.id = generateId();
    conv.title = "新对话";
    conv.created_at = time(nullptr);
    conv.updated_at = time(nullptr);

    _conversations.insert(_conversations.begin(), conv);
    _current_idx = 0;

    saveConversationToSd(conv);
    saveMetadataToSd();

    if (_on_update) _on_update();
    mclog::tagInfo(TAG, "New conversation: {}", conv.id);
}

Conversation* ChatEngine::getCurrentConversation() {
    if (_current_idx >= 0 && _current_idx < (int)_conversations.size())
        return &_conversations[_current_idx];
    return nullptr;
}

const Conversation* ChatEngine::getCurrentConversation() const {
    if (_current_idx >= 0 && _current_idx < (int)_conversations.size())
        return &_conversations[_current_idx];
    return nullptr;
}

bool ChatEngine::loadConversation(const std::string& id) {
    for (size_t i = 0; i < _conversations.size(); i++) {
        if (_conversations[i].id == id) {
            _current_idx = i;
            if (_on_update) _on_update();
            return true;
        }
    }
    return false;
}

bool ChatEngine::deleteConversation(const std::string& id) {
    for (size_t i = 0; i < _conversations.size(); i++) {
        if (_conversations[i].id == id) {
            auto& sd = SdCardManager::getInstance();
            if (sd.isReady()) {
                std::string path = sd.getMountPath() + "/chat/" + id + ".json";
                sd.deleteFile(path);
            }
            _conversations.erase(_conversations.begin() + i);
            if ((int)i <= _current_idx) _current_idx--;
            if (_current_idx < 0 && !_conversations.empty()) _current_idx = 0;
            saveMetadataToSd();
            if (_on_update) _on_update();
            return true;
        }
    }
    return false;
}

void ChatEngine::sendMessage(const std::string& text) {
    auto* conv = getCurrentConversation();
    if (!conv || text.empty()) return;

    if (_processing) return;

    ChatMessage user_msg;
    user_msg.id = generateId();
    user_msg.role = MsgRole::USER;
    user_msg.content = text;
    user_msg.state = MsgState::COMPLETE;
    user_msg.timestamp = time(nullptr);
    conv->messages.push_back(user_msg);

    if (_on_new_msg) _on_new_msg(user_msg);

    _processing = true;
    ChatMessage asst_msg;
    asst_msg.id = generateId();
    asst_msg.role = MsgRole::ASSISTANT;
    asst_msg.state = MsgState::STREAMING;
    asst_msg.timestamp = time(nullptr);
    conv->messages.push_back(asst_msg);

    std::string* msg_text = new std::string(text);
    xTaskCreate([](void* arg) -> void {
        esp_task_wdt_add(NULL);
        std::string* msg_ptr = (std::string*)arg;
        std::string msg = *msg_ptr;
        delete msg_ptr;

        auto& ai = AiEngine::getInstance();
        auto& chat = ChatEngine::getInstance();
        auto& cfg = ConfigStore::getInstance().getAssistantConfig();
        auto* conv = chat.getCurrentConversation();

        if (!conv) {
            chat._processing = false;
            esp_task_wdt_delete(NULL);
            vTaskDelete(NULL);
            return;
        }

        if (cfg.streaming) {
            ai.chatStream(msg, conv, [&chat, conv](const StreamChunk& chunk) -> void {
                auto& last = conv->messages.back();
                if (!chunk.content.empty()) {
                    last.content += chunk.content;
                    last.state = MsgState::STREAMING;
                }
                if (!chunk.thinking_content.empty()) {
                    last.thinking_content += chunk.thinking_content;
                    if (chat._on_thinking) chat._on_thinking(last.thinking_content);
                }
                if (chunk.tokens_used > 0) last.token_count = chunk.tokens_used;
                if (chunk.is_done || chunk.is_error) {
                    last.state = chunk.is_error ? MsgState::ERROR : MsgState::COMPLETE;
                    if (chunk.is_error && !chunk.error_msg.empty() && last.content.empty()) {
                        last.content = "[!] " + chunk.error_msg;
                    }
                    last.model_used = ConfigStore::getInstance().getAssistantConfig().model_id;
                    conv->total_tokens += last.token_count;
                    conv->updated_at = time(nullptr);
                    if (conv->title == "新对话" && !last.content.empty()) {
                        conv->title = last.content.substr(0, std::min((size_t)30, last.content.size()));
                        for (auto& c : conv->title) if (c == '\n') c = ' ';
                    }
                    chat._processing = false;
                    chat.saveCurrentConversation();
                    chat.saveMetadataToSd();
                    if (chat._on_complete) chat._on_complete(!chunk.is_error);
                }
                if (chat._on_update) chat._on_update();
            });
        } else {
            std::string response = ai.chat(msg, conv);
            auto& last = conv->messages.back();

            cJSON* j = cJSON_Parse(response.c_str());
            if (j) {
                cJSON* err_item = cJSON_GetObjectItem(j, "error");
                if (err_item && err_item->valuestring) {
                    last.content = std::string("\xe2\x9d\x8c ") + err_item->valuestring;
                    last.state = MsgState::ERROR;
                    cJSON_Delete(j);
                    chat._processing = false;
                    chat.saveCurrentConversation();
                    if (chat._on_complete) chat._on_complete(false);
                    if (chat._on_update) chat._on_update();
                    esp_task_wdt_delete(NULL);
                    vTaskDelete(NULL);
                    return;
                }
                cJSON* choices = cJSON_GetObjectItem(j, "choices");
                if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                    cJSON* msg_obj = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message");
                    if (msg_obj) {
                        cJSON* content = cJSON_GetObjectItem(msg_obj, "content");
                        if (content && content->valuestring) last.content = content->valuestring;
                        cJSON* reasoning = cJSON_GetObjectItem(msg_obj, "reasoning_content");
                        if (reasoning && reasoning->valuestring) last.thinking_content = reasoning->valuestring;
                    }
                }
                cJSON* usage = cJSON_GetObjectItem(j, "usage");
                if (usage) {
                    cJSON* ct = cJSON_GetObjectItem(usage, "completion_tokens");
                    if (ct) last.token_count = ct->valueint;
                }
                cJSON_Delete(j);
            }

            last.state = last.content.empty() ? MsgState::ERROR : MsgState::COMPLETE;
            last.model_used = cfg.model_id;
            conv->total_tokens += last.token_count;
            conv->updated_at = time(nullptr);
            if (conv->title == "新对话" && !last.content.empty()) {
                conv->title = last.content.substr(0, std::min((size_t)30, last.content.size()));
                for (auto& c : conv->title) if (c == '\n') c = ' ';
            }
            chat._processing = false;
            chat.saveCurrentConversation();
            chat.saveMetadataToSd();
            if (chat._on_complete) chat._on_complete(last.state != MsgState::ERROR);
            if (chat._on_update) chat._on_update();
        }

        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
    }, "chat_req", 8192, msg_text, 5, NULL);
}

void ChatEngine::saveCurrentConversation() {
    auto* conv = getCurrentConversation();
    if (!conv) return;
    NvsHelper::setStr("chat_data", ("conv_" + conv->id).c_str(), conv->toJson());
    saveConversationToSd(*conv);
}

void ChatEngine::saveAllMetadata() {
    saveMetadataToSd();
}

bool ChatEngine::saveConversationToSd(const Conversation& conv) {
    auto& sd = SdCardManager::getInstance();
    if (!sd.isReady()) return false;

    sd.ensureDirectories();
    std::string path = sd.getMountPath() + "/chat/" + conv.id + ".json";
    std::string json = conv.toJson();
    return sd.writeFile(path, (const uint8_t*)json.c_str(), json.size());
}

bool ChatEngine::saveMetadataToSd() {
    auto& sd = SdCardManager::getInstance();
    if (!sd.isReady()) {
        cJSON* arr = cJSON_CreateArray();
        for (auto& c : _conversations) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", c.id.c_str());
            cJSON_AddStringToObject(o, "title", c.title.c_str());
            cJSON_AddNumberToObject(o, "created", (double)c.created_at);
            cJSON_AddNumberToObject(o, "updated", (double)c.updated_at);
            cJSON_AddNumberToObject(o, "tokens", c.total_tokens);
            cJSON_AddItemToArray(arr, o);
        }
        char* out = cJSON_PrintUnformatted(arr);
        NvsHelper::setStr("chat_data", "conversation_list", out);
        free(out); cJSON_Delete(arr);
        return false;
    }

    sd.ensureDirectories();
    cJSON* arr = cJSON_CreateArray();
    for (auto& c : _conversations) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", c.id.c_str());
        cJSON_AddStringToObject(o, "title", c.title.c_str());
        cJSON_AddNumberToObject(o, "created", (double)c.created_at);
        cJSON_AddNumberToObject(o, "updated", (double)c.updated_at);
        cJSON_AddNumberToObject(o, "tokens", c.total_tokens);
        cJSON_AddItemToArray(arr, o);
    }
    char* out = cJSON_PrintUnformatted(arr);
    std::string json(out);
    free(out); cJSON_Delete(arr);

    std::string path = sd.getMountPath() + "/chat/metadata.json";
    bool ok = sd.writeFile(path, (const uint8_t*)json.c_str(), json.size());
    mclog::tagInfo(TAG, "Save metadata to SD: {} bytes ok={}", json.size(), ok);
    return ok;
}

void ChatEngine::loadAllFromStorage() {
    auto& sd = SdCardManager::getInstance();

    if (sd.isReady()) {
        sd.ensureDirectories();
        std::string meta_path = sd.getMountPath() + "/chat/metadata.json";
        std::vector<uint8_t> meta_data;
        if (sd.readFile(meta_path, meta_data) && !meta_data.empty()) {
            std::string meta_json(meta_data.begin(), meta_data.end());
            cJSON* arr = cJSON_Parse(meta_json.c_str());
            if (arr && cJSON_IsArray(arr)) {
                for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
                    cJSON* item = cJSON_GetArrayItem(arr, i);
                    cJSON* id_item = cJSON_GetObjectItem(item, "id");
                    if (!id_item || !id_item->valuestring) continue;

                    std::string conv_id = id_item->valuestring;
                    std::string conv_path = sd.getMountPath() + "/chat/" + conv_id + ".json";
                    std::vector<uint8_t> conv_data;
                    if (sd.readFile(conv_path, conv_data) && !conv_data.empty()) {
                        std::string conv_json(conv_data.begin(), conv_data.end());
                        Conversation conv = Conversation::fromJson(conv_json);
                        if (!conv.id.empty()) {
                            _conversations.push_back(conv);
                            mclog::tagInfo(TAG, "Loaded conv {} ({} msgs)", conv.id, conv.messages.size());
                        }
                    }
                }
            }
            if (arr) cJSON_Delete(arr);
        }
    }

    if (_conversations.empty()) {
        std::string list_json;
        NvsHelper::getStr("chat_data", "conversation_list", list_json);
        if (!list_json.empty()) {
            cJSON* arr = cJSON_Parse(list_json.c_str());
            if (arr && cJSON_IsArray(arr)) {
                for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
                    cJSON* item = cJSON_GetArrayItem(arr, i);
                    cJSON* id_item = cJSON_GetObjectItem(item, "id");
                    if (!id_item || !id_item->valuestring) continue;

                    std::string conv_id = id_item->valuestring;
                    std::string conv_json;
                    NvsHelper::getStr("chat_data", ("conv_" + conv_id).c_str(), conv_json);
                    if (!conv_json.empty()) {
                        Conversation conv = Conversation::fromJson(conv_json);
                        if (!conv.id.empty()) _conversations.push_back(conv);
                    }
                }
            }
            if (arr) cJSON_Delete(arr);
        }
    }

    mclog::tagInfo(TAG, "loadAllFromStorage: {} conversations", _conversations.size());
}

int ChatEngine::getTotalTokens() const {
    auto* c = getCurrentConversation(); return c ? c->total_tokens : 0;
}
int ChatEngine::getContextCount() const {
    auto* c = getCurrentConversation(); return c ? c->messages.size() : 0;
}

void ChatEngine::update() {}

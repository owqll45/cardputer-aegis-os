#include "message.h"
#include <cJSON.h>
#include <ctime>
#include <sstream>
#include <iomanip>

std::string ChatMessage::toJson() const {
    cJSON* j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", id.c_str());
    const char* role_str = "user";
    switch (role) {
        case MsgRole::USER: role_str = "user"; break;
        case MsgRole::ASSISTANT: role_str = "assistant"; break;
        case MsgRole::SYSTEM: role_str = "system"; break;
        case MsgRole::THINKING: role_str = "thinking"; break;
        case MsgRole::TOOL: role_str = "tool"; break;
    }
    cJSON_AddStringToObject(j, "role", role_str);
    cJSON_AddStringToObject(j, "content", content.c_str());
    if (!thinking_content.empty()) cJSON_AddStringToObject(j, "thinking", thinking_content.c_str());
    cJSON_AddNumberToObject(j, "timestamp", (double)timestamp);
    cJSON_AddNumberToObject(j, "tokens", token_count);
    if (!model_used.empty()) cJSON_AddStringToObject(j, "model", model_used.c_str());
    char* out = cJSON_PrintUnformatted(j);
    std::string result(out);
    free(out); cJSON_Delete(j);
    return result;
}

ChatMessage ChatMessage::fromJson(const std::string& json) {
    ChatMessage m;
    cJSON* j = cJSON_Parse(json.c_str());
    if (!j) return m;
    cJSON* item;
    if ((item = cJSON_GetObjectItem(j, "id")) && item->valuestring) m.id = item->valuestring;
    if ((item = cJSON_GetObjectItem(j, "role")) && item->valuestring) {
        std::string r = item->valuestring;
        if (r == "assistant") m.role = MsgRole::ASSISTANT;
        else if (r == "system") m.role = MsgRole::SYSTEM;
        else if (r == "thinking") m.role = MsgRole::THINKING;
        else if (r == "tool") m.role = MsgRole::TOOL;
    }
    if ((item = cJSON_GetObjectItem(j, "content")) && item->valuestring) m.content = item->valuestring;
    if ((item = cJSON_GetObjectItem(j, "thinking")) && item->valuestring) m.thinking_content = item->valuestring;
    if ((item = cJSON_GetObjectItem(j, "timestamp"))) m.timestamp = (int64_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(j, "tokens"))) m.token_count = item->valueint;
    if ((item = cJSON_GetObjectItem(j, "model")) && item->valuestring) m.model_used = item->valuestring;
    cJSON_Delete(j);
    return m;
}

std::string Conversation::toJson() const {
    cJSON* j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", id.c_str());
    cJSON_AddStringToObject(j, "title", title.c_str());
    cJSON_AddNumberToObject(j, "created", (double)created_at);
    cJSON_AddNumberToObject(j, "updated", (double)updated_at);
    cJSON_AddNumberToObject(j, "total_tokens", total_tokens);
    cJSON* msgs = cJSON_CreateArray();
    for (auto& m : messages) {
        cJSON* mj = cJSON_Parse(m.toJson().c_str());
        if (mj) cJSON_AddItemToArray(msgs, mj);
    }
    cJSON_AddItemToObject(j, "messages", msgs);
    char* out = cJSON_PrintUnformatted(j);
    std::string result(out);
    free(out); cJSON_Delete(j);
    return result;
}

Conversation Conversation::fromJson(const std::string& json) {
    Conversation c;
    cJSON* j = cJSON_Parse(json.c_str());
    if (!j) return c;
    cJSON* item;
    if ((item = cJSON_GetObjectItem(j, "id")) && item->valuestring) c.id = item->valuestring;
    if ((item = cJSON_GetObjectItem(j, "title")) && item->valuestring) c.title = item->valuestring;
    if ((item = cJSON_GetObjectItem(j, "created"))) c.created_at = (int64_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(j, "updated"))) c.updated_at = (int64_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(j, "total_tokens"))) c.total_tokens = item->valueint;
    cJSON* msgs = cJSON_GetObjectItem(j, "messages");
    if (cJSON_IsArray(msgs)) {
        for (int i = 0; i < cJSON_GetArraySize(msgs); i++) {
            cJSON* mi = cJSON_GetArrayItem(msgs, i);
            char* ms = cJSON_PrintUnformatted(mi);
            if (ms) { c.messages.push_back(ChatMessage::fromJson(ms)); free(ms); }
        }
    }
    cJSON_Delete(j);
    return c;
}

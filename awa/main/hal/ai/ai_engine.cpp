#include "ai_engine.h"
#include "config_store.h"
#include "memory_system.h"
#include "tools/tool_manager.h"
#include "nvs_helper.h"
#include "hal_facade.h"
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <esp_http_client.h>
#include <cJSON.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <algorithm>

static void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.length(), to); pos += to.length(); }
}

static std::string base64Encode(const uint8_t* data, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t val = data[i] << 16;
        if (i + 1 < len) val |= data[i + 1] << 8;
        if (i + 2 < len) val |= data[i + 2];
        out += tbl[(val >> 18) & 0x3F];
        out += tbl[(val >> 12) & 0x3F];
        out += (i + 1 < len) ? tbl[(val >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? tbl[val & 0x3F] : '=';
    }
    return out;
}

static const std::string TAG = "AiEngine";

struct StreamCtx {
    std::function<void(const StreamChunk&)> on_chunk;
    std::string line_buf;
    std::string full_content;
    std::string full_thinking;
    bool done = false;
    bool error = false;
    std::string error_msg;
};

AiEngine& AiEngine::getInstance() {
    static AiEngine instance;
    return instance;
}

void AiEngine::init() {
    auto& cfg = ConfigStore::getInstance();
    if (cfg.isConfigured()) _ready = true;
    mclog::tagInfo(TAG, "init: ready={}", _ready);
}

std::string AiEngine::buildSystemPrompt() {
    auto& cfg = ConfigStore::getInstance().getAssistantConfig();
    std::string sp = cfg.system_prompt;
    if (sp.empty()) sp = "你是一个有帮助的AI助手。";

    auto& mem = MemorySystem::getInstance();
    auto memories = mem.getAllMemories();
    if (!memories.empty()) {
        sp += "\n\n[记忆]\n";
        for (auto& m : memories) {
            sp += m.key + ": " + m.value + "\n";
        }
    }

    auto& tc = ConfigStore::getInstance().getToolConfig();
    if (tc.get_time) {
        sp += "\n[当前时间工具可用，你可以告知用户当前时间]";
    }
    if (tc.device_info) {
        sp += "\n[设备信息工具可用，设备为M5Stack Cardputer ADV]";
    }
    if (tc.battery_info) {
        int bat = M5.Power.getBatteryLevel();
        char bat_buf[16]; snprintf(bat_buf, sizeof(bat_buf), "%d%%", bat);
        sp += std::string("\n[当前电池电量: ") + bat_buf + "]";
    }
    if (tc.web_search) {
        sp += "\n[网络搜索工具可用，你可以搜索网络获取实时信息]";
    }
    if (tc.ask_user) {
        sp += "\n[你可以向用户提问以获取更多信息]";
    }

    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char date_buf[32], time_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", t);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", t);
    char weekday_buf[16];
    const char* wdays[] = {"日","一","二","三","四","五","六"};
    snprintf(weekday_buf, sizeof(weekday_buf), "周%s", wdays[t->tm_wday]);

    char bat_buf2[16]; snprintf(bat_buf2, sizeof(bat_buf2), "%ld%%", (long)M5.Power.getBatteryLevel());

    replaceAll(sp, "{date}", std::string(date_buf));
    replaceAll(sp, "{time}", std::string(time_buf));
    replaceAll(sp, "{weekday}", std::string(weekday_buf));
    replaceAll(sp, "{model_id}", cfg.model_id);
    replaceAll(sp, "{model_name}", cfg.model_id);
    replaceAll(sp, "{language}", cfg.language);
    replaceAll(sp, "{system_version}", "1.0.0");
    replaceAll(sp, "{device_info}", "M5Stack Cardputer ADV");
    replaceAll(sp, "{battery}", std::string(bat_buf2));
    replaceAll(sp, "{assistant_name}", cfg.name);
    replaceAll(sp, "{user}", cfg.user_name);

    return sp;
}

std::string AiEngine::buildChatBody(const std::string& prompt, const Conversation* ctx) {
    auto& cfg = ConfigStore::getInstance().getAssistantConfig();

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", cfg.model_id.c_str());
    cJSON_AddNumberToObject(body, "max_tokens", cfg.max_tokens > 0 ? cfg.max_tokens : 4096);
    cJSON_AddNumberToObject(body, "temperature", cfg.temperature);

    if (cfg.streaming) cJSON_AddBoolToObject(body, "stream", true);

    cJSON* messages = cJSON_AddArrayToObject(body, "messages");

    cJSON* sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", buildSystemPrompt().c_str());
    cJSON_AddItemToArray(messages, sys);

    int max_ctx = cfg.max_context_messages;
    int start_idx = 0;
    if (ctx && max_ctx > 0 && (int)ctx->messages.size() > max_ctx * 2) {
        start_idx = ctx->messages.size() - max_ctx * 2;
    }

    if (ctx) {
        for (size_t i = start_idx; i < ctx->messages.size(); i++) {
            auto& m = ctx->messages[i];
            if (m.role == MsgRole::THINKING || m.state == MsgState::ERROR) continue;
            cJSON* msg = cJSON_CreateObject();
            const char* role = m.role == MsgRole::ASSISTANT ? "assistant" : "user";
            cJSON_AddStringToObject(msg, "role", role);
            cJSON_AddStringToObject(msg, "content", m.content.c_str());
            cJSON_AddItemToArray(messages, msg);
        }
    }

    cJSON* user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", prompt.c_str());
    cJSON_AddItemToArray(messages, user_msg);

    char* out = cJSON_PrintUnformatted(body);
    std::string result(out);
    free(out); cJSON_Delete(body);
    return result;
}

bool AiEngine::callApi(const std::string& endpoint, const char* body, size_t body_len,
                       std::string& response, int timeout_ms) {
    auto& cfg = ConfigStore::getInstance();
    std::string url = cfg.getApiUrl();
    if (!endpoint.empty()) {
        if (endpoint.find("http") == 0) url = endpoint;
        else if (!url.empty() && url.back() == '/') url = url + endpoint.substr(1);
        else url = url + endpoint;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = timeout_ms;
    config.buffer_size = 512;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "Content-Type", "application/json");

    std::string auth = "Bearer " + cfg.getApiKey();
    esp_http_client_set_header(client, "Authorization", auth.c_str());

    esp_http_client_set_post_field(client, body, body_len);

    esp_task_wdt_reset();
    esp_err_t err = esp_http_client_perform(client);
    esp_task_wdt_reset();
    int status_code = esp_http_client_get_status_code(client);

    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        int content_len = esp_http_client_get_content_length(client);
        if (content_len > 0 && content_len < 16384) {
            response.resize(content_len, 0);
            int read = esp_http_client_read(client, &response[0], content_len);
            response.resize(read > 0 ? read : 0);
        } else {
            char buf[1024];
            int total = 0;
            int r;
            while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                response.append(buf, r);
                total += r;
                if (total > 16384) break;
                esp_task_wdt_reset();
            }
        }
        mclog::tagInfo(TAG, "API OK: {} bytes status={}", response.size(), status_code);
        esp_http_client_cleanup(client);
        return true;
    }

    mclog::tagError(TAG, "API error: {} status={}", esp_err_to_name(err), status_code);
    esp_http_client_cleanup(client);
    return false;
}

std::string AiEngine::chat(const std::string& user_message, const Conversation* context) {
    if (!_ready) return "{\"error\":\"AI引擎未配置，请在Web面板设置API Key\"}";

    if (!GetHAL().isWifiConnected()) {
        return "{\"error\":\"WiFi未连接，请先连接WiFi\"}";
    }

    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < 20480) {
        return "{\"error\":\"内存不足(" + std::to_string(free_heap/1024) + "KB)，请稍后重试\"}";
    }

    std::string body = buildChatBody(user_message, context);
    std::string response;

    if (callApi("/v1/chat/completions", body.c_str(), body.length(), response, 15000)) {
        return response;
    }
    return "{\"error\":\"请求失败，请检查网络连接和API配置后重试\"}";
}

static esp_err_t streamEventHandler(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<StreamCtx*>(evt->user_data);
    if (!ctx) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA: {
        esp_task_wdt_reset();
        if (esp_http_client_get_status_code(evt->client) != 200) {
            ctx->error = true;
            return ESP_OK;
        }
        std::string data(static_cast<const char*>(evt->data), evt->data_len);
        for (char c : data) {
            if (c == '\n') {
                if (ctx->line_buf.find("data: ") == 0) {
                    std::string payload = ctx->line_buf.substr(6);
                    payload.erase(0, payload.find_first_not_of(" \t"));
                    payload.erase(payload.find_last_not_of(" \t\r") + 1);

                    if (payload == "[DONE]") {
                        StreamChunk ch; ch.is_done = true;
                        ctx->on_chunk(ch);
                        ctx->done = true;
                        ctx->line_buf.clear();
                        return ESP_OK;
                    }

                    cJSON* j = cJSON_Parse(payload.c_str());
                    if (j) {
                        StreamChunk ch;
                        cJSON* choices = cJSON_GetObjectItem(j, "choices");
                        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                            cJSON* first = cJSON_GetArrayItem(choices, 0);
                            cJSON* delta = cJSON_GetObjectItem(first, "delta");
                            if (delta) {
                                cJSON* content = cJSON_GetObjectItem(delta, "content");
                                if (content && content->valuestring) {
                                    ch.content = content->valuestring;
                                    ctx->full_content += ch.content;
                                }
                                cJSON* reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
                                if (reasoning && reasoning->valuestring) {
                                    ch.thinking_content = reasoning->valuestring;
                                    ch.is_thinking = true;
                                    ctx->full_thinking += ch.thinking_content;
                                }
                            }
                            cJSON* finish = cJSON_GetObjectItem(first, "finish_reason");
                            if (finish && finish->valuestring && std::string(finish->valuestring) == "stop") {
                                ch.is_done = true;
                            }
                        }
                        cJSON* usage = cJSON_GetObjectItem(j, "usage");
                        if (usage) {
                            cJSON* ct = cJSON_GetObjectItem(usage, "completion_tokens");
                            if (ct) ch.tokens_used = ct->valueint;
                        }
                        ctx->on_chunk(ch);
                        cJSON_Delete(j);
                    }
                }
                ctx->line_buf.clear();
            } else if (c != '\r') {
                ctx->line_buf += c;
            }
        }
        break;
    }
    default: break;
    }
    return ESP_OK;
}

bool AiEngine::chatStream(const std::string& user_message, const Conversation* context,
                          std::function<void(const StreamChunk&)> on_chunk) {
    if (!_ready || !on_chunk) {
        StreamChunk e; e.is_error=true; e.error_msg="AI引擎未配置，请在Web面板设置API Key"; on_chunk(e); return false;
    }

    if (!GetHAL().isWifiConnected()) {
        StreamChunk e; e.is_error=true; e.error_msg="WiFi未连接，请先在Web面板连接WiFi"; on_chunk(e); return false;
    }

    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < 20480) {
        StreamChunk e; e.is_error=true;
        e.error_msg = "内存不足(" + std::to_string(free_heap/1024) + "KB)，请稍后重试";
        on_chunk(e);
        return false;
    }

    std::string body = buildChatBody(user_message, context);
    auto& store = ConfigStore::getInstance();
    std::string url = store.getApiUrl() + "/v1/chat/completions";

    StreamCtx ctx;
    ctx.on_chunk = on_chunk;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.event_handler = streamEventHandler;
    config.user_data = &ctx;
    config.buffer_size = 512;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        StreamChunk e; e.is_error=true; e.error_msg="HTTP初始化失败"; on_chunk(e);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    std::string auth = "Bearer " + store.getApiKey();
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    esp_http_client_set_post_field(client, body.c_str(), body.length());

    mclog::tagInfo(TAG, "Stream start: heap={}KB body={}B", (int)(esp_get_free_heap_size()/1024), body.size());

    esp_task_wdt_reset();
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_task_wdt_reset();

    if (err != ESP_OK || status != 200) {
        if (!ctx.error && !ctx.done) {
            StreamChunk e; e.is_error=true;
            if (err == ESP_ERR_HTTP_CONNECT) {
                e.error_msg = "无法连接API服务器，请检查网络";
            } else if (status == 401) {
                e.error_msg = "API Key无效，请在Web面板重新设置";
            } else if (status == 429) {
                e.error_msg = "API调用频率超限，请稍后重试";
            } else if (status >= 500) {
                e.error_msg = "API服务器错误(" + std::to_string(status) + ")";
            } else {
                e.error_msg = "请求失败(" + std::to_string(status) + ")";
            }
            on_chunk(e);
        }
        esp_http_client_cleanup(client);
        mclog::tagError(TAG, "Stream failed: err={} status={} heap={}KB", esp_err_to_name(err), status, (int)(esp_get_free_heap_size()/1024));
        return false;
    }

    if (!ctx.done) {
        StreamChunk ch; ch.is_done = true; on_chunk(ch);
    }

    esp_http_client_cleanup(client);
    mclog::tagInfo(TAG, "Stream done: content={} thinking={} heap={}KB", ctx.full_content.size(), ctx.full_thinking.size(), (int)(esp_get_free_heap_size()/1024));
    return true;
}

std::string AiEngine::stt(const uint8_t* audio_data, size_t len) {
    auto& cfg = ConfigStore::getInstance();
    if (!cfg.isSttEnabled()) return "[STT未启用]";

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", cfg.getSttModelId().empty() ? "whisper-1" : cfg.getSttModelId().c_str());
    cJSON* f = cJSON_CreateObject();
    cJSON_AddNumberToObject(f, "size", len);
    std::string b64 = base64Encode(audio_data, len);
    cJSON_AddStringToObject(f, "data", b64.c_str());
    cJSON_AddItemToObject(body, "file", f);
    char* bs = cJSON_PrintUnformatted(body);
    std::string resp;
    bool ok = callApi(cfg.getSttUrl().empty() ? "/v1/audio/transcriptions" : cfg.getSttUrl(), bs, strlen(bs), resp, 30000);
    free(bs); cJSON_Delete(body);
    if (!ok) return "[STT失败]";
    cJSON* j = cJSON_Parse(resp.c_str());
    if (!j) return "[STT解析失败]";
    std::string text;
    cJSON* item = cJSON_GetObjectItem(j, "text");
    if (item && item->valuestring) text = item->valuestring;
    cJSON_Delete(j);
    return text.empty() ? "[无法识别]" : text;
}

std::string AiEngine::tts(const std::string& text, uint8_t** out_audio, size_t* out_len) {
    auto& cfg = ConfigStore::getInstance();
    if (!cfg.isTtsEnabled()) return "";

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", cfg.getTtsModelId().empty() ? "tts-1" : cfg.getTtsModelId().c_str());
    cJSON_AddStringToObject(body, "input", text.c_str());
    cJSON_AddStringToObject(body, "voice", "alloy");
    cJSON_AddStringToObject(body, "response_format", "mp3");
    char* bs = cJSON_PrintUnformatted(body);
    std::string resp;
    bool ok = callApi(cfg.getTtsUrl().empty() ? "/v1/audio/speech" : cfg.getTtsUrl(), bs, strlen(bs), resp, 30000);
    free(bs); cJSON_Delete(body);
    if (!ok || out_audio == nullptr) return "";
    *out_audio = (uint8_t*)malloc(resp.size());
    memcpy(*out_audio, resp.data(), resp.size());
    *out_len = resp.size();
    return resp;
}

std::string AiEngine::searchBing(const std::string& query) {
    auto& store = ConfigStore::getInstance();
    std::string key = store.getBingApiKey();
    if (key.empty()) return "{\"error\":\"Bing API Key未配置\"}";

    std::string url = "https://api.bing.microsoft.com/v7.0/search?q=" + query;
    esp_http_client_config_t cfg = {}; cfg.url = url.c_str(); cfg.method = HTTP_METHOD_GET; cfg.timeout_ms = 15000;
    auto client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Ocp-Apim-Subscription-Key", key.c_str());
    esp_err_t err = esp_http_client_perform(client);
    std::string resp;
    if (err == ESP_OK) {
        int cl = esp_http_client_get_content_length(client);
        if (cl > 0 && cl < 16384) { resp.resize(cl); esp_http_client_read(client, &resp[0], cl); }
    }
    esp_http_client_cleanup(client);
    return resp;
}

std::string AiEngine::searchTavily(const std::string& query) {
    auto& store = ConfigStore::getInstance();
    std::string key = store.getTavilyApiKey();
    if (key.empty()) return "{\"error\":\"Tavily API Key未配置\"}";
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "query", query.c_str());
    cJSON_AddStringToObject(body, "api_key", key.c_str());
    cJSON_AddNumberToObject(body, "max_results", 5);
    char* bs = cJSON_PrintUnformatted(body);
    std::string resp;
    bool ok = callApi("https://api.tavily.com/search", bs, strlen(bs), resp, 15000);
    free(bs); cJSON_Delete(body);
    return ok ? resp : "{\"error\":\"搜索失败\"}";
}

std::string AiEngine::executeJs(const std::string& code) {
    mclog::tagInfo(TAG, "JS execute: {} chars", code.size());
    return "{\"result\":\"JS执行完成\",\"output\":\"嵌入式设备暂不支持完整JS引擎\"}";
}

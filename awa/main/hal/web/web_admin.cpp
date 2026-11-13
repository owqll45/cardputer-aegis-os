#include "web_admin.h"
#include "config_store.h"
#include "sd_card_manager.h"
#include "chat_engine.h"
#include "ai_engine.h"
#include "model_registry.h"
#include "memory_system.h"
#include "tools/tool_manager.h"
#include "mcp/mcp_client.h"
#include "nvs_helper.h"
#include "hal_facade.h"
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <cJSON.h>
#include <cstring>
#include <esp_chip_info.h>
#include <soc/soc_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ctime>

static const std::string TAG = "WebAdmin";

struct HdlCtx { std::function<esp_err_t(httpd_req_t*)> fn; };

static void registerAllApiEndpoints(WebAdmin& web);

WebAdmin& WebAdmin::getInstance() {
    static WebAdmin instance;
    return instance;
}

bool WebAdmin::start(int port) {
    if (_server) stop();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port; cfg.max_uri_handlers = 32; cfg.lru_purge_enable = true;
    esp_err_t ret = httpd_start(&_server, &cfg);
    if (ret != ESP_OK) { _server = nullptr; return false; }
    registerAllApiEndpoints(*this);
    mclog::tagInfo(TAG, "Started on port {}", port);
    return true;
}

void WebAdmin::stop() {
    if (_server) { httpd_stop(_server); _server = nullptr; }
}

void WebAdmin::registerGet(const char* uri, std::function<esp_err_t(httpd_req_t*)> handler) {
    if (!_server || !uri) return;
    auto ctx = new HdlCtx{handler};
    httpd_uri_t u = {.uri=uri,.method=HTTP_GET,
        .handler=[](httpd_req_t* r){return ((HdlCtx*)r->user_ctx)->fn(r);},
        .user_ctx=ctx};
    httpd_register_uri_handler(_server, &u);
}

void WebAdmin::registerPost(const char* uri, std::function<esp_err_t(httpd_req_t*)> handler) {
    if (!_server || !uri) return;
    auto ctx = new HdlCtx{handler};
    httpd_uri_t u = {.uri=uri,.method=HTTP_POST,
        .handler=[](httpd_req_t* r){return ((HdlCtx*)r->user_ctx)->fn(r);},
        .user_ctx=ctx};
    httpd_register_uri_handler(_server, &u);
}

std::string WebAdmin::getPostData(httpd_req_t* req) {
    int len = req->content_len;
    if (len <= 0 || len > 8192) return "";
    std::string buf(len, 0);
    int r = httpd_req_recv(req, &buf[0], len);
    return r > 0 ? std::string(buf.data(), r) : "";
}

esp_err_t WebAdmin::sendJson(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json); return ESP_OK;
}

esp_err_t WebAdmin::sendError(httpd_req_t* req, int code, const char* msg) {
    cJSON* j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "success", false);
    cJSON_AddStringToObject(j, "error", msg);
    char* o = cJSON_PrintUnformatted(j);
    sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
}

#include "web_admin_html.h"

static void registerAllApiEndpoints(WebAdmin& web) {
    auto& reg = ModelRegistry::getInstance(); reg.init();

    web.registerGet("/", [](httpd_req_t* req) -> esp_err_t {
        bool configured = ConfigStore::getInstance().isConfigured();
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        if (!configured) {
            httpd_resp_sendstr(req, HTML_SETUP);
        } else {
            httpd_resp_sendstr(req, HTML_MAIN);
        }
        return ESP_OK;
    });

    web.registerGet("/api/status", [](httpd_req_t* req) -> esp_err_t {
        cJSON* j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "heap", esp_get_free_heap_size());
        cJSON_AddNumberToObject(j, "tokens", ChatEngine::getInstance().getTotalTokens());
        cJSON_AddNumberToObject(j, "context", ChatEngine::getInstance().getContextCount());
        cJSON_AddBoolToObject(j, "sd", SdCardManager::getInstance().isReady());
        cJSON_AddNumberToObject(j, "battery", M5.Power.getBatteryLevel());
        cJSON_AddBoolToObject(j, "wifi", GetHAL().isWifiConnected());
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerGet("/api/config/assistant", [](httpd_req_t* req) -> esp_err_t {
        auto& c = ConfigStore::getInstance().getAssistantConfig();
        cJSON* j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", c.name.c_str());
        cJSON_AddStringToObject(j, "user_name", c.user_name.c_str());
        cJSON_AddStringToObject(j, "language", c.language.c_str());
        cJSON_AddNumberToObject(j, "temperature", c.temperature);
        cJSON_AddNumberToObject(j, "max_tokens", c.max_tokens);
        cJSON_AddNumberToObject(j, "max_context_messages", c.max_context_messages);
        cJSON_AddBoolToObject(j, "streaming", c.streaming);
        cJSON_AddBoolToObject(j, "show_thinking", c.show_thinking);
        cJSON_AddBoolToObject(j, "auto_collapse_thinking", c.auto_collapse_thinking);
        cJSON_AddBoolToObject(j, "auto_scroll", c.auto_scroll);
        cJSON_AddBoolToObject(j, "latex_rendering", c.latex_rendering);
        cJSON_AddBoolToObject(j, "code_block_highlight", c.code_block_highlight);
        cJSON_AddStringToObject(j, "system_prompt", c.system_prompt.c_str());
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/config/assistant", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        AssistantConfig cfg = ConfigStore::getInstance().getAssistantConfig();
        cJSON* item;
        if ((item = cJSON_GetObjectItem(json, "name")) && item->valuestring) cfg.name = item->valuestring;
        if ((item = cJSON_GetObjectItem(json, "user_name")) && item->valuestring) cfg.user_name = item->valuestring;
        if ((item = cJSON_GetObjectItem(json, "language")) && item->valuestring) cfg.language = item->valuestring;
        if ((item = cJSON_GetObjectItem(json, "temperature"))) cfg.temperature = item->valuedouble;
        if ((item = cJSON_GetObjectItem(json, "max_tokens"))) cfg.max_tokens = item->valueint;
        if ((item = cJSON_GetObjectItem(json, "max_context_messages"))) cfg.max_context_messages = item->valueint;
        if ((item = cJSON_GetObjectItem(json, "streaming"))) cfg.streaming = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "show_thinking"))) cfg.show_thinking = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "auto_collapse_thinking"))) cfg.auto_collapse_thinking = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "auto_scroll"))) cfg.auto_scroll = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "latex_rendering"))) cfg.latex_rendering = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "code_block_highlight"))) cfg.code_block_highlight = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "system_prompt")) && item->valuestring) cfg.system_prompt = item->valuestring;
        ConfigStore::getInstance().setAssistantConfig(cfg);
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/config/ai", [](httpd_req_t* req) -> esp_err_t {
        auto& s = ConfigStore::getInstance();
        cJSON* j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "provider", s.getProvider().c_str());
        cJSON_AddStringToObject(j, "api_url", s.getApiUrl().c_str());
        std::string masked_key = s.getApiKey().empty() ? "" : s.getApiKey().substr(0, 8) + "...";
        cJSON_AddStringToObject(j, "api_key", masked_key.c_str());
        cJSON_AddBoolToObject(j, "stt_enabled", s.isSttEnabled());
        cJSON_AddStringToObject(j, "stt_model_id", s.getSttModelId().c_str());
        cJSON_AddStringToObject(j, "stt_url", s.getSttUrl().c_str());
        cJSON_AddBoolToObject(j, "tts_enabled", s.isTtsEnabled());
        cJSON_AddStringToObject(j, "tts_model_id", s.getTtsModelId().c_str());
        cJSON_AddStringToObject(j, "tts_url", s.getTtsUrl().c_str());
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/config/ai", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        auto& s = ConfigStore::getInstance();
        cJSON* item;
        if ((item = cJSON_GetObjectItem(json, "provider")) && item->valuestring) s.setProvider(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "api_url")) && item->valuestring) s.setApiUrl(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "api_key")) && item->valuestring) s.setApiKey(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "stt_enabled"))) s.setSttEnabled(cJSON_IsTrue(item));
        if ((item = cJSON_GetObjectItem(json, "stt_model_id")) && item->valuestring) s.setSttModelId(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "stt_url")) && item->valuestring) s.setSttUrl(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "tts_enabled"))) s.setTtsEnabled(cJSON_IsTrue(item));
        if ((item = cJSON_GetObjectItem(json, "tts_model_id")) && item->valuestring) s.setTtsModelId(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "tts_url")) && item->valuestring) s.setTtsUrl(item->valuestring);
        AiEngine::getInstance().init();
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/models", [](httpd_req_t* req) -> esp_err_t {
        auto& reg = ModelRegistry::getInstance();
        cJSON* j = cJSON_CreateObject();
        cJSON* llm_arr = cJSON_CreateArray();
        for (auto& m : reg.getModelsByType("llm")) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", m.id.c_str());
            cJSON_AddStringToObject(o, "name", m.name.c_str());
            cJSON_AddStringToObject(o, "provider", m.provider.c_str());
            cJSON_AddStringToObject(o, "description", m.description.c_str());
            cJSON_AddNumberToObject(o, "context_window", m.context_window);
            cJSON_AddNumberToObject(o, "max_output", m.max_output);
            cJSON_AddItemToArray(llm_arr, o);
        }
        cJSON_AddItemToObject(j, "llm", llm_arr);
        cJSON* stt_arr = cJSON_CreateArray();
        for (auto& m : reg.getModelsByType("stt")) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", m.id.c_str());
            cJSON_AddStringToObject(o, "name", m.name.c_str());
            cJSON_AddItemToArray(stt_arr, o);
        }
        cJSON_AddItemToObject(j, "stt", stt_arr);
        cJSON* tts_arr = cJSON_CreateArray();
        for (auto& m : reg.getModelsByType("tts")) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", m.id.c_str());
            cJSON_AddStringToObject(o, "name", m.name.c_str());
            cJSON_AddItemToArray(tts_arr, o);
        }
        cJSON_AddItemToObject(j, "tts", tts_arr);
        auto& cur = ConfigStore::getInstance().getAssistantConfig();
        cJSON_AddStringToObject(j, "current", cur.model_id.c_str());
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/model/set", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        cJSON* id = cJSON_GetObjectItem(json, "id");
        if (!id || !id->valuestring) { cJSON_Delete(json); return WebAdmin::sendError(req, 400, "\u9700\u8981model_id"); }
        auto cfg = ConfigStore::getInstance().getAssistantConfig();
        cfg.model_id = id->valuestring;
        ConfigStore::getInstance().setAssistantConfig(cfg);
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/chat/messages", [](httpd_req_t* req) -> esp_err_t {
        cJSON* j = cJSON_CreateObject();
        cJSON* arr = cJSON_CreateArray();
        auto* conv = ChatEngine::getInstance().getCurrentConversation();
        if (conv) {
            for (auto& m : conv->messages) {
                cJSON* o = cJSON_CreateObject();
                const char* role = "assistant";
                if (m.role == MsgRole::USER) role = "user";
                else if (m.role == MsgRole::THINKING) role = "thinking";
                cJSON_AddStringToObject(o, "role", role);
                cJSON_AddStringToObject(o, "content", m.content.c_str());
                if (!m.thinking_content.empty())
                    cJSON_AddStringToObject(o, "thinking", m.thinking_content.c_str());
                cJSON_AddItemToArray(arr, o);
            }
        }
        cJSON_AddItemToObject(j, "messages", arr);
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/chat/send", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        cJSON* msg = cJSON_GetObjectItem(json, "message");
        if (!msg || !msg->valuestring) { cJSON_Delete(json); return WebAdmin::sendError(req, 400, "\u9700\u8981message"); }
        ChatEngine::getInstance().sendMessage(msg->valuestring);
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerPost("/api/chat/new", [](httpd_req_t* req) -> esp_err_t {
        ChatEngine::getInstance().newConversation();
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerPost("/api/chat/load", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        cJSON* id = cJSON_GetObjectItem(json, "id");
        bool ok = id && id->valuestring && ChatEngine::getInstance().loadConversation(id->valuestring);
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", ok);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/history", [](httpd_req_t* req) -> esp_err_t {
        cJSON* j = cJSON_CreateObject();
        cJSON* arr = cJSON_CreateArray();
        auto& convs = ChatEngine::getInstance().getConversations();
        for (auto& c : convs) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", c.id.c_str());
            cJSON_AddStringToObject(o, "title", c.title.c_str());
            cJSON_AddNumberToObject(o, "msg_count", c.messages.size());
            cJSON_AddNumberToObject(o, "total_tokens", c.total_tokens);
            cJSON_AddItemToArray(arr, o);
        }
        cJSON_AddItemToObject(j, "conversations", arr);
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerGet("/api/search/config", [](httpd_req_t* req) -> esp_err_t {
        auto& s = ConfigStore::getInstance();
        cJSON* j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "bing_key", s.getBingApiKey().c_str());
        cJSON_AddStringToObject(j, "tavily_key", s.getTavilyApiKey().c_str());
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/search/config", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        auto& s = ConfigStore::getInstance();
        cJSON* bk = cJSON_GetObjectItem(json, "bing_key"); if (bk && bk->valuestring) s.setBingApiKey(bk->valuestring);
        cJSON* tk = cJSON_GetObjectItem(json, "tavily_key"); if (tk && tk->valuestring) s.setTavilyApiKey(tk->valuestring);
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/memory/list", [](httpd_req_t* req) -> esp_err_t {
        auto& mem = MemorySystem::getInstance();
        cJSON* j = cJSON_CreateObject();
        cJSON* arr = cJSON_CreateArray();
        for (auto& m : mem.getAllMemories()) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "key", m.key.c_str());
            cJSON_AddStringToObject(o, "value", m.value.c_str());
            cJSON_AddNumberToObject(o, "created", (double)m.created_at);
            cJSON_AddItemToArray(arr, o);
        }
        cJSON_AddItemToObject(j, "memories", arr);
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/memory/set", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        cJSON* k = cJSON_GetObjectItem(json, "key");
        cJSON* v = cJSON_GetObjectItem(json, "value");
        if (!k || !k->valuestring || !v || !v->valuestring) {
            cJSON_Delete(json); return WebAdmin::sendError(req, 400, "\u9700\u8981key\u548cvalue");
        }
        MemorySystem::getInstance().set(k->valuestring, v->valuestring);
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerPost("/api/memory/delete", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        cJSON* k = cJSON_GetObjectItem(json, "key");
        if (!k || !k->valuestring) { cJSON_Delete(json); return WebAdmin::sendError(req, 400, "\u9700\u8981key"); }
        MemorySystem::getInstance().remove(k->valuestring);
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/tools/config", [](httpd_req_t* req) -> esp_err_t {
        auto& tc = ConfigStore::getInstance().getToolConfig();
        cJSON* j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ask_user", tc.ask_user);
        cJSON_AddBoolToObject(j, "get_time", tc.get_time);
        cJSON_AddBoolToObject(j, "run_js", tc.run_js);
        cJSON_AddBoolToObject(j, "device_info", tc.device_info);
        cJSON_AddBoolToObject(j, "battery_info", tc.battery_info);
        cJSON_AddBoolToObject(j, "web_search", tc.web_search);
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/tools/config", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        ToolConfig tc = ConfigStore::getInstance().getToolConfig();
        cJSON* item;
        if ((item = cJSON_GetObjectItem(json, "ask_user"))) tc.ask_user = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "get_time"))) tc.get_time = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "run_js"))) tc.run_js = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "device_info"))) tc.device_info = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "battery_info"))) tc.battery_info = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(json, "web_search"))) tc.web_search = cJSON_IsTrue(item);
        ConfigStore::getInstance().setToolConfig(tc);
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerPost("/api/setup/save", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        auto& s = ConfigStore::getInstance();
        cJSON* item;
        if ((item = cJSON_GetObjectItem(json, "api_key")) && item->valuestring) s.setApiKey(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "api_url")) && item->valuestring) s.setApiUrl(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "provider")) && item->valuestring) s.setProvider(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "model_id")) && item->valuestring) {
            auto cfg = s.getAssistantConfig();
            cfg.model_id = item->valuestring;
            s.setAssistantConfig(cfg);
        }
        if ((item = cJSON_GetObjectItem(json, "wifi_mode"))) s.setWifiMode(item->valueint);
        if ((item = cJSON_GetObjectItem(json, "wifi_ssid")) && item->valuestring) s.setWifiSsid(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "wifi_pwd")) && item->valuestring) s.setWifiPassword(item->valuestring);
        s.markFirstBootDone();
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        cJSON_AddStringToObject(r, "msg", "rebooting");
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerPost("/api/setup/skip", [](httpd_req_t* req) -> esp_err_t {
        ConfigStore::getInstance().markFirstBootDone();
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/wifi/config", [](httpd_req_t* req) -> esp_err_t {
        auto& s = ConfigStore::getInstance();
        cJSON* j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "wifi_mode", s.getWifiMode());
        cJSON_AddStringToObject(j, "wifi_ssid", s.getWifiSsid().c_str());
        cJSON_AddStringToObject(j, "wifi_pwd", s.getWifiPassword().c_str());
        cJSON_AddStringToObject(j, "ap_ssid", s.getApSsid().c_str());
        cJSON_AddStringToObject(j, "ap_pwd", s.getApPassword().c_str());
        cJSON_AddBoolToObject(j, "connected", GetHAL().isWifiConnected());
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerGet("/api/wifi/scan", [](httpd_req_t* req) -> esp_err_t {
        std::vector<NetworkManager::ScanResult_t> results;
        GetHAL().getNetwork().wifiScan(results);
        cJSON* j = cJSON_CreateObject();
        cJSON* arr = cJSON_CreateArray();
        for (auto& r : results) {
            cJSON* ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", r.second.c_str());
            cJSON_AddNumberToObject(ap, "rssi", r.first);
            cJSON_AddItemToArray(arr, ap);
        }
        cJSON_AddItemToObject(j, "networks", arr);
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/wifi/config", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        auto& s = ConfigStore::getInstance();
        cJSON* item;
        if ((item = cJSON_GetObjectItem(json, "wifi_mode"))) s.setWifiMode(item->valueint);
        if ((item = cJSON_GetObjectItem(json, "wifi_ssid")) && item->valuestring) s.setWifiSsid(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "wifi_pwd")) && item->valuestring) s.setWifiPassword(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "ap_ssid")) && item->valuestring) s.setApSsid(item->valuestring);
        if ((item = cJSON_GetObjectItem(json, "ap_pwd")) && item->valuestring) s.setApPassword(item->valuestring);
        s.save();
        cJSON_Delete(json);

        int mode = s.getWifiMode();
        if (mode == 1 && !s.getWifiSsid().empty()) {
            std::string ssid = s.getWifiSsid();
            std::string pwd = s.getWifiPassword();
            cJSON* r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "success", true);
            cJSON_AddStringToObject(r, "action", "connecting");
            char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r);

            xTaskCreate([](void* arg) -> void {
                esp_task_wdt_add(NULL);
                char* ssid_c = (char*)arg;
                std::string ssid_str(ssid_c);
                free(ssid_c);
                std::string pwd_str = ConfigStore::getInstance().getWifiPassword();
                mclog::tagInfo("web", "Background WiFi connect: {}", ssid_str);
                bool ok = GetHAL().wifiConnect(ssid_str, pwd_str);
                mclog::tagInfo("web", "WiFi connect result: {}", ok ? "success" : "failed");
                esp_task_wdt_delete(NULL);
                vTaskDelete(NULL);
            }, "wifi_conn", 4096, strdup(ssid.c_str()), 5, NULL);
            return ESP_OK;
        }

        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        cJSON_AddStringToObject(r, "action", "saved");
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/mcp/config", [](httpd_req_t* req) -> esp_err_t {
        auto& s = ConfigStore::getInstance();
        cJSON* j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "mcp_enabled", s.isMcpEnabled());
        cJSON_AddStringToObject(j, "mcp_url", s.getMcpUrl().c_str());
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/mcp/config", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebAdmin::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str()); if (!json) return WebAdmin::sendError(req, 400, "JSON\u9519\u8bef");
        auto& s = ConfigStore::getInstance();
        cJSON* item;
        if ((item = cJSON_GetObjectItem(json, "mcp_enabled"))) s.setMcpEnabled(cJSON_IsTrue(item));
        if ((item = cJSON_GetObjectItem(json, "mcp_url")) && item->valuestring) s.setMcpUrl(item->valuestring);
        s.save();
        cJSON_Delete(json);
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });

    web.registerGet("/api/mcp/tools", [](httpd_req_t* req) -> esp_err_t {
        auto& mcp = McpClient::getInstance();
        cJSON* j = cJSON_CreateObject();
        cJSON* arr = cJSON_CreateArray();
        cJSON_AddBoolToObject(j, "connected", mcp.isConnected());
        for (auto& t : mcp.listTools()) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", t.name.c_str());
            cJSON_AddStringToObject(o, "description", t.description.c_str());
            cJSON_AddItemToArray(arr, o);
        }
        cJSON_AddItemToObject(j, "tools", arr);
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerGet("/api/system/info", [](httpd_req_t* req) -> esp_err_t {
        esp_chip_info_t chip_info;
        esp_chip_info(&chip_info);
        cJSON* j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "\u82af\u7247", "ESP32-S3");
        cJSON_AddNumberToObject(j, "\u5185\u6838\u6570", chip_info.cores);
        cJSON_AddStringToObject(j, "Flash", "8MB");
        cJSON_AddNumberToObject(j, "\u5185\u5b58(KB)", (int)(esp_get_free_heap_size() / 1024));
        cJSON_AddNumberToObject(j, "\u7535\u6c60(%)", M5.Power.getBatteryLevel());
        cJSON_AddStringToObject(j, "WiFi", GetHAL().isWifiConnected() ? "\u5df2\u8fde\u63a5" : "\u672a\u8fde\u63a5");
        cJSON_AddStringToObject(j, "TF\u5361", SdCardManager::getInstance().isReady() ? "\u5df2\u63d2\u5165" : "\u672a\u63d2\u5165");
        cJSON_AddStringToObject(j, "\u6a21\u578b", ConfigStore::getInstance().getAssistantConfig().model_id.c_str());
        cJSON_AddNumberToObject(j, "Token", ChatEngine::getInstance().getTotalTokens());
        char* o = cJSON_PrintUnformatted(j);
        WebAdmin::sendJson(req, o); free(o); cJSON_Delete(j); return ESP_OK;
    });

    web.registerPost("/api/system/reset", [](httpd_req_t* req) -> esp_err_t {
        NvsHelper::eraseAll("asst_cfg");
        NvsHelper::eraseAll("chat_data");
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        esp_restart();
        return ESP_OK;
    });

    web.registerPost("/api/reboot", [](httpd_req_t* req) -> esp_err_t {
        cJSON* r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "success", true);
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        esp_restart();
        return ESP_OK;
    });

    web.registerPost("/api/sd/format", [](httpd_req_t* req) -> esp_err_t {
        auto result = SdCardManager::getInstance().formatCard();
        cJSON* r = cJSON_CreateObject();
        if (result == FormatResult::SUCCESS) {
            cJSON_AddBoolToObject(r, "success", true);
        } else {
            cJSON_AddBoolToObject(r, "success", false);
            cJSON_AddStringToObject(r, "error", "\u683c\u5f0f\u5316\u5931\u8d25");
        }
        char* o = cJSON_PrintUnformatted(r); WebAdmin::sendJson(req, o); free(o); cJSON_Delete(r); return ESP_OK;
    });
}

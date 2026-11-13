#include "ai_manager.h"
#include "api_config.h"
#include <mooncake_log.h>
#include <esp_http_client.h>
#include <cJSON.h>

static const std::string TAG = "AiManager";

AiManager& AiManager::getInstance() {
    static AiManager instance;
    return instance;
}

void AiManager::init() {
    mclog::tagInfo(TAG, "init");
    auto& cfg = ApiConfig::getInstance().getConfig();
    if (cfg.enabled) {
        AiModelInfo m;
        m.name = "default";
        m.type = "llm";
        m.endpoint = cfg.base_url + "/v1/chat/completions";
        m.api_key = cfg.api_key;
        m.enabled = true;
        _models.push_back(m);
        mclog::tagInfo(TAG, "Default LLM model registered: {}", cfg.default_model);
    }
}

void AiManager::update() {}

bool AiManager::addModel(const AiModelInfo& model) {
    for (auto& m : _models) {
        if (m.name == model.name) { m = model; return true; }
    }
    _models.push_back(model);
    return true;
}

bool AiManager::removeModel(const std::string& name) {
    for (size_t i = 0; i < _models.size(); i++) {
        if (_models[i].name == name) { _models.erase(_models.begin() + i); return true; }
    }
    return false;
}

std::string AiManager::chat(const std::string& prompt, const std::string* model_name) {
    const AiModelInfo* target = nullptr;

    if (model_name) {
        for (const auto& m : _models) {
            if (m.name == *model_name && m.enabled) { target = &m; break; }
        }
    }

    if (!target) {
        for (const auto& m : _models) {
            if (m.enabled && m.type == "llm") { target = &m; break; }
        }
    }

    if (!target) return "{\"error\":\"no available model\"}";

    std::string response;
    if (callOpenAICompatible(prompt, target, response)) {
        return response;
    }
    return "{\"error\":\"request failed\"}";
}

bool AiManager::callOpenAICompatible(const std::string& prompt, const AiModelInfo* model, std::string& response) {
    auto& api_cfg = ApiConfig::getInstance().getConfig();

    cJSON* body = cJSON_CreateObject();
    cJSON* messages = cJSON_CreateArray();
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", prompt.c_str());
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(body, "messages", messages);
    cJSON_AddStringToObject(body, "model", api_cfg.default_model.c_str());
    cJSON_AddNumberToObject(body, "max_tokens", 1024);
    char* body_str = cJSON_PrintUnformatted(body);

    std::string url = model->endpoint.empty()
        ? (api_cfg.base_url + "/v1/chat/completions")
        : model->endpoint;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.event_handler = nullptr;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    std::string auth = "Bearer " + model->api_key;
    esp_http_client_set_header(client, "Authorization", auth.c_str());

    esp_http_client_set_post_field(client, body_str, strlen(body_str));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if (err == ESP_OK && status_code == 200) {
        int content_len = esp_http_client_get_content_length(client);
        if (content_len > 0 && content_len < 16384) {
            response.resize(content_len, 0);
            int read = esp_http_client_read(client, &response[0], content_len);
            response.resize(read > 0 ? read : 0);
        } else {
            char buf[4096];
            int total = 0;
            int r;
            while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                response.append(buf, r);
                total += r;
                if (total > 8192) break;
            }
        }
        mclog::tagInfo(TAG, "Chat OK: {} bytes", response.size());
    } else {
        mclog::tagError(TAG, "HTTP error: {} status={}", esp_err_to_name(err), status_code);
    }

    free(body_str);
    cJSON_Delete(body);
    esp_http_client_cleanup(client);

    return !response.empty();
}

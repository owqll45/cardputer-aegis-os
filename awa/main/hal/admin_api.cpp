#include "admin_api.h"
#include "api_config.h"
#include "ai_manager.h"
#include "system_init.h"
#include <mooncake_log.h>
#include <cJSON.h>

static const std::string TAG = "AdminAPI";

AdminAPI& AdminAPI::getInstance() {
    static AdminAPI instance;
    return instance;
}

void AdminAPI::init(WebServer& server) {
    mclog::tagInfo(TAG, "Registering admin endpoints...");

    server.registerGet("/api/admin/status", [](httpd_req_t* req) -> esp_err_t {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "device", "Cardputer ADV");
        cJSON_AddStringToObject(r, "fw", "1.0.0");
        cJSON_AddNumberToObject(r, "heap", esp_get_free_heap_size());
        cJSON_AddStringToObject(r, "init_state",
            SystemInit::getInstance().isInitCompleted() ? "done" : "pending");
        auto& api = ApiConfig::getInstance();
        cJSON_AddStringToObject(r, "provider", api.isConfigured() ? "configured" : "none");
        char* out = cJSON_PrintUnformatted(r);
        WebServer::sendJsonResponse(req, 200, out);
        free(out); cJSON_Delete(r); return ESP_OK;
    });

    server.registerGet("/api/admin/config", [](httpd_req_t* req) -> esp_err_t {
        auto& cfg = ApiConfig::getInstance().getConfig();
        cJSON* r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "enabled", cfg.enabled);
        cJSON_AddNumberToObject(r, "provider", (int)cfg.provider);
        cJSON_AddStringToObject(r, "model", cfg.default_model.c_str());
        cJSON_AddStringToObject(r, "url", cfg.base_url.c_str());
        if (!cfg.api_key.empty()) {
            std::string masked = cfg.api_key.substr(0, 8) + "...";
            cJSON_AddStringToObject(r, "key_preview", masked.c_str());
        }
        char* out = cJSON_PrintUnformatted(r);
        WebServer::sendJsonResponse(req, 200, out);
        free(out); cJSON_Delete(r); return ESP_OK;
    });

    server.registerPost("/api/admin/chat", [](httpd_req_t* req) -> esp_err_t {
        std::string data = WebServer::getPostData(req);
        cJSON* json = cJSON_Parse(data.c_str());
        if (!json) return WebServer::sendErrorResponse(req, 400, "Bad JSON");

        cJSON* msg = cJSON_GetObjectItem(json, "message");
        if (!msg || !msg->valuestring) { cJSON_Delete(json); return WebServer::sendErrorResponse(req, 400, "Need 'message'"); }

        std::string result = AiManager::getInstance().chat(msg->valuestring);

        cJSON* resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "success", !result.empty() && result.find("\"error\"") == std::string::npos);
        cJSON_AddStringToObject(resp, "response", result.c_str());
        char* out = cJSON_PrintUnformatted(resp);
        WebServer::sendJsonResponse(req, 200, out);
        free(out); cJSON_Delete(resp); cJSON_Delete(json); return ESP_OK;
    });

    server.registerPost("/api/admin/reboot", [](httpd_req_t* req) -> esp_err_t {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "success", true);
        cJSON_AddStringToObject(r, "message", "Rebooting...");
        char* out = cJSON_PrintUnformatted(r);
        WebServer::sendJsonResponse(req, 200, out);
        free(out); cJSON_Delete(r);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        esp_restart(); return ESP_OK;
    });

    server.registerPost("/api/admin/reset", [](httpd_req_t* req) -> esp_err_t {
        SystemInit::getInstance().reset();
        cJSON* r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "success", true);
        cJSON_AddStringToObject(r, "message", "Factory reset - rebooting...");
        char* out = cJSON_PrintUnformatted(r);
        WebServer::sendJsonResponse(req, 200, out);
        free(out); cJSON_Delete(r);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        esp_restart(); return ESP_OK;
    });

    mclog::tagInfo(TAG, "All admin endpoints registered");
}

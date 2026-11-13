#include "web_server.h"
#include <mooncake_log.h>
#include <cstring>
#include <cJSON.h>

static const std::string TAG = "WebServer";

struct HandlerCtx {
    std::function<esp_err_t(httpd_req_t*)> fn;
};

WebServer& WebServer::getInstance() {
    static WebServer instance;
    return instance;
}

bool WebServer::start(uint16_t port) {
    if (_server) { stop(); }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&_server, &cfg);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "httpd_start failed: {}", (int)ret);
        _server = nullptr;
        return false;
    }
    mclog::tagInfo(TAG, "Started on port {}", port);
    return true;
}

void WebServer::stop() {
    if (_server) {
        httpd_stop(_server);
        _server = nullptr;
        mclog::tagInfo(TAG, "Stopped");
    }
}

void WebServer::registerGet(const char* uri, std::function<esp_err_t(httpd_req_t*)> handler) {
    if (!_server || !uri) return;
    auto ctx = new HandlerCtx{handler};
    httpd_uri_t u = {
        .uri = uri, .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            auto* c = (HandlerCtx*)req->user_ctx;
            return c->fn(req);
        },
        .user_ctx = ctx
    };
    httpd_register_uri_handler(_server, &u);
}

void WebServer::registerPost(const char* uri, std::function<esp_err_t(httpd_req_t*)> handler) {
    if (!_server || !uri) return;
    auto ctx = new HandlerCtx{handler};
    httpd_uri_t u = {
        .uri = uri, .method = HTTP_POST,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            auto* c = (HandlerCtx*)req->user_ctx;
            return c->fn(req);
        },
        .user_ctx = ctx
    };
    httpd_register_uri_handler(_server, &u);
}

std::string WebServer::getPostData(httpd_req_t* req) {
    int len = req->content_len;
    if (len <= 0 || len > 8192) return "";
    std::string buf(len, 0);
    int recv = httpd_req_recv(req, &buf[0], len);
    if (recv <= 0) return "";
    buf.resize(recv);
    return buf;
}

esp_err_t WebServer::sendJsonResponse(httpd_req_t* req, int code, const char* json) {
    httpd_resp_set_status(req, code == 200 ? "200 OK" : "500 Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

esp_err_t WebServer::sendErrorResponse(httpd_req_t* req, int code, const char* msg) {
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", false);
    cJSON_AddStringToObject(r, "error", msg);
    char* out = cJSON_PrintUnformatted(r);
    sendJsonResponse(req, code, out);
    free(out); cJSON_Delete(r); return ESP_OK;
}

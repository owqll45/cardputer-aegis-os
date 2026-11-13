#pragma once
#include <string>
#include <functional>
#include <esp_http_server.h>

class WebAdmin {
public:
    static WebAdmin& getInstance();

    bool start(int port = 80);
    void stop();
    bool isRunning() const { return _server != nullptr; }

    void registerGet(const char* uri, std::function<esp_err_t(httpd_req_t*)> handler);
    void registerPost(const char* uri, std::function<esp_err_t(httpd_req_t*)> handler);

    static std::string getPostData(httpd_req_t* req);
    static esp_err_t sendJson(httpd_req_t* req, const char* json);
    static esp_err_t sendError(httpd_req_t* req, int code, const char* msg);

private:
    WebAdmin() = default;
    httpd_handle_t _server = nullptr;
};

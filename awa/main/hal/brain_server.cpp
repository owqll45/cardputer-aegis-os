#include "brain_server.h"
#include <esp_log.h>
#include <lwip/sockets.h>
#include <mooncake_log.h>

static const char *TAG = "BrainServer";

namespace brain_server {

    BrainCoreServer& BrainCoreServer::getInstance() {
        static BrainCoreServer instance;
        return instance;
    }

    bool BrainCoreServer::start(int port) {
        if (_server_handle) return true;

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = port;
        config.ctrl_port = port + 1;
        // Increase stack size for WS handling
        config.stack_size = 8192;

        mclog::tagInfo(TAG, "Starting BrainCore Server on port {}...", port);

        if (httpd_start(&_server_handle, &config) == ESP_OK) {
            httpd_uri_t ws_uri = {
                .uri        = "/",
                .method     = HTTP_GET,
                .handler    = _ws_handler,
                .user_ctx   = NULL,
                .is_websocket = true
            };
            httpd_register_uri_handler(_server_handle, &ws_uri);
            return true;
        }

        mclog::tagError(TAG, "Failed to start httpd server");
        return false;
    }

    void BrainCoreServer::stop() {
        if (_server_handle) {
            httpd_stop(_server_handle);
            _server_handle = nullptr;
        }
        std::lock_guard<std::mutex> lock(_clients_mutex);
        _clients.clear();
    }

    esp_err_t BrainCoreServer::_ws_handler(httpd_req_t *req) {
        if (req->method == HTTP_GET) {
            // Handshake
            char ip_str[INET_ADDRSTRLEN];
            int fd = httpd_req_to_sockfd(req);
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            getpeername(fd, (struct sockaddr *)&addr, &addr_len);
            inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
            
            getInstance()._add_client(fd, ip_str);
            mclog::tagInfo(TAG, "New WebSocket client connected: {}", ip_str);
            return ESP_OK;
        }

        httpd_ws_frame_t ws_pkt;
        uint8_t *buf = NULL;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        // Get frame length
        esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
        if (ret != ESP_OK) {
            return ret;
        }

        if (ws_pkt.len > 0) {
            buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
            if (buf == NULL) {
                return ESP_ERR_NO_MEM;
            }
            ws_pkt.payload = buf;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret != ESP_OK) {
                free(buf);
                return ret;
            }
            
            getInstance()._handle_ws_message(httpd_req_to_sockfd(req), buf, ws_pkt.len);
            free(buf);
        }

        // Close frame handling
        if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            getInstance()._remove_client(httpd_req_to_sockfd(req));
        }

        return ESP_OK;
    }

    void BrainCoreServer::_handle_ws_message(int fd, uint8_t* data, size_t len) {
        cJSON *root = cJSON_Parse((const char*)data);
        if (root) {
            if (_message_handler) {
                _message_handler(fd, root);
            }
            cJSON_Delete(root);
        }
    }

    void BrainCoreServer::_add_client(int fd, const std::string& ip) {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        _clients.push_back({fd, ip, (uint32_t)esp_log_timestamp()});
    }

    void BrainCoreServer::_remove_client(int fd) {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        for (auto it = _clients.begin(); it != _clients.end(); ++it) {
            if (it->fd == fd) {
                mclog::tagInfo(TAG, "Client disconnected: {}", it->ip);
                _clients.erase(it);
                break;
            }
        }
    }

    void BrainCoreServer::broadcast(const std::string& message) {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t*)message.c_str();
        ws_pkt.len = message.length();
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        for (const auto& client : _clients) {
            httpd_ws_send_frame_async(_server_handle, client.fd, &ws_pkt);
        }
    }

    void BrainCoreServer::broadcast(cJSON* json) {
        char* str = cJSON_PrintUnformatted(json);
        if (str) {
            broadcast(std::string(str));
            free(str);
        }
    }

    void BrainCoreServer::sendTo(int fd, const std::string& message) {
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t*)message.c_str();
        ws_pkt.len = message.length();
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        httpd_ws_send_frame_async(_server_handle, fd, &ws_pkt);
    }

    std::vector<ClientInfo> BrainCoreServer::getConnectedClients() {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        return _clients;
    }

} // namespace brain_server

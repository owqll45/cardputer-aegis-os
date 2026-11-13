#pragma once
#include <esp_http_server.h>
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include "cJSON.h"

namespace brain_server {

    struct ClientInfo {
        int fd;
        std::string ip;
        uint32_t last_seen;
    };

    class BrainCoreServer {
    public:
        static BrainCoreServer& getInstance();

        bool start(int port = 8765);
        void stop();
        bool isRunning() const { return _server_handle != nullptr; }

        // Broadcast to all connected clients
        void broadcast(const std::string& message);
        void broadcast(cJSON* json);
        
        // Send to specific client
        void sendTo(int fd, const std::string& message);

        // Callbacks for external logic (Coordinator)
        using MessageHandler = std::function<void(int fd, cJSON* root)>;
        void setMessageHandler(MessageHandler handler) { _message_handler = handler; }

        std::vector<ClientInfo> getConnectedClients();

    private:
        BrainCoreServer() = default;
        ~BrainCoreServer() { stop(); }

        httpd_handle_t _server_handle = nullptr;
        MessageHandler _message_handler = nullptr;
        std::vector<ClientInfo> _clients;
        std::mutex _clients_mutex;

        static esp_err_t _ws_handler(httpd_req_t *req);
        void _handle_ws_message(int fd, uint8_t* data, size_t len);
        void _add_client(int fd, const std::string& ip);
        void _remove_client(int fd);
    };

} // namespace brain_server

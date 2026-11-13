#pragma once
#include "web_server.h"

class AdminAPI {
public:
    static AdminAPI& getInstance();

    void init(WebServer& server);

private:
    AdminAPI() = default;
};

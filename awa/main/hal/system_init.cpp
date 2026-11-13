#include "system_init.h"
#include "api_config.h"
#include "config_store.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <cJSON.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <lwip/sockets.h>
#include <cstring>

static const std::string TAG = "SystemInit";
static const std::string INIT_NS = "sys_init";
static const std::string INIT_KEY = "done";

SystemInit& SystemInit::getInstance() {
    static SystemInit instance;
    return instance;
}

void SystemInit::init() {
    mclog::tagInfo(TAG, "System init module starting...");

    if (!checkNvsFlag()) {
        _state = InitState::PENDING;
        mclog::tagWarn(TAG, "First boot detected! Starting portal...");
        startPortal();
    } else {
        _state = InitState::COMPLETED;
        mclog::tagInfo(TAG, "Already initialized");
    }
}

void SystemInit::update() {
    if (_state == InitState::IN_PROGRESS && _portal_server) {
        static uint32_t last_log = 0;
        if (GetHAL().millis() - last_log > 5000) {
            last_log = GetHAL().millis();
            mclog::tagInfo(TAG, "Portal active at 192.168.4.1");
        }
    }
}

bool SystemInit::checkNvsFlag() {
    nvs_handle_t h;
    if (nvs_open(INIT_NS.c_str(), NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t flag = 0;
    esp_err_t err = nvs_get_u8(h, INIT_KEY.c_str(), &flag);
    nvs_close(h);
    return (err == ESP_OK && flag == 1);
}

void SystemInit::setNvsFlag() {
    nvs_handle_t h;
    if (nvs_open(INIT_NS.c_str(), NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, INIT_KEY.c_str(), 1);
    nvs_commit(h);
    nvs_close(h);
}

void SystemInit::markCompleted() {
    setNvsFlag();
    _state = InitState::COMPLETED;
    mclog::tagInfo(TAG, "Initialization completed");
}

void SystemInit::reset() {
    nvs_handle_t h;
    if (nvs_open(INIT_NS.c_str(), NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    _state = InitState::PENDING;
    mclog::tagWarn(TAG, "Reset - will prompt on next boot");
}

bool SystemInit::startPortal() {
    _state = InitState::IN_PROGRESS;

    GetHAL().display().fillScreen(TFT_BLACK);
    GetHAL().display().setTextColor(TFT_CYAN, TFT_BLACK);
    GetHAL().display().setTextSize(1);
    GetHAL().display().drawCenterString("=== First Boot Setup ===", 120, 20);
    GetHAL().display().drawCenterString("WiFi: Cardputer-Setup", 120, 45);
    GetHAL().display().drawCenterString("Open: http://192.168.4.1", 120, 65);

    mclog::tagInfo(TAG, "Starting WiFi AP via NetworkManager...");
    GetHAL().wifiApInit("Cardputer-Setup", "");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&_portal_server, &cfg);
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "httpd_start failed: {}", esp_err_to_name(ret));
        _state = InitState::PENDING;
        return false;
    }

    registerEndpoints();
    mclog::tagInfo(TAG, "Portal running at http://192.168.4.1");
    return true;
}

void SystemInit::stopPortal() {
    if (_portal_server) {
        httpd_stop(_portal_server);
        _portal_server = nullptr;
    }
    mclog::tagInfo(TAG, "Portal stopped");
}

std::string SystemInit::getSetupHtml() {
    static const char* html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Cardputer ADV Setup</title>"
        "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:20px;max-width:480px}"
        "h1{color:#00d2ff;border-bottom:2px solid #00d2ff;padding-bottom:10px}"
        ".card{background:#16213e;border-radius:12px;padding:16px;margin:12px 0}"
        "label{display:block;margin-top:10px;font-size:14px;color:#aaa}"
        "input,select{width:100%;padding:10px;border:1px solid #333;border-radius:8px;background:#0f3460;color:#fff;box-sizing:border-box;font-size:14px;margin-top:4px}"
        "button{width:100%;padding:14px;background:#00d2ff;color:#1a1a2e;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;margin-top:16px}"
        ".status{text-align:center;padding:10px;margin:10px 0;border-radius:8px;display:none}"
        "</style></head><body>"
        "<h1>Cardputer ADV</h1><div class=\"card\">"
        "<h3>Initial Setup</h3>"
        "<label>WiFi SSID</label><input id=\"ssid\" placeholder=\"Your WiFi name\">"
        "<label>Password</label><input id=\"pwd\" type=\"password\" placeholder=\"WiFi password\">"
        "<hr style=\"border-color:#333;margin:16px 0\">"
        "<h3>AI API Configuration</h3>"
        "<label>Provider</label><select id=\"atype\"><option value=\"deepseek\">DeepSeek</option><option value=\"openai\">OpenAI Compatible</option></select>"
        "<label>API Key</label><input id=\"akey\" placeholder=\"sk-...\">"
        "<label>API URL (optional)</label><input id=\"aurl\" placeholder=\"https://api.deepseek.com\">"
        "<button onclick=\"submitSetup()\">Save & Reboot</button>"
        "<div class=\"status\" id=\"status\"></div>"
        "</div><script>"
        "function submitSetup(){"
        "var s=document.getElementById('ssid').value;"
        "var p=document.getElementById('pwd').value;"
        "var t=document.getElementById('atype').value;"
        "var k=document.getElementById('akey').value;"
        "var u=document.getElementById('aurl').value;"
        "var st=document.getElementById('status');"
        "if(!s||!k){st.style.display='block';st.style.background='#f44';st.textContent='SSID and API Key required';return;}"
        "st.style.display='block';st.style.background='#28a745';st.textContent='Saving...';"
        "fetch('/api/setup/complete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,password:p,api_type:t,api_key:k,api_url:u})})"
        ".then(function(r){return r.json();}).then(function(d){st.style.background=d.success?'#28a745':'#f44';st.textContent=d.message;if(d.success)setTimeout(function(){location.reload();},3000);})"
        ".catch(function(e){st.style.background='#f44';st.textContent='Error: '+e;});"
        "}"
        "</script></body></html>";
    return std::string(html);
}

void SystemInit::registerEndpoints() {
    if (!_portal_server) return;

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            std::string html = SystemInit::getInstance().getSetupHtml();
            httpd_resp_set_type(req, "text/html");
            httpd_resp_sendstr(req, html.c_str());
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(_portal_server, &root);

    httpd_uri_t setup = {
        .uri = "/api/setup/complete", .method = HTTP_POST,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            char buf[1024];
            int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
            if (ret <= 0) return httpd_resp_send_500(req);

            buf[ret] = 0;
            cJSON* json = cJSON_Parse(buf);
            if (!json) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");

            InitConfig cfg;
            cJSON* item;
            if ((item = cJSON_GetObjectItem(json, "ssid")) && item->valuestring)
                cfg.wifi_ssid = item->valuestring;
            if ((item = cJSON_GetObjectItem(json, "password")) && item->valuestring)
                cfg.wifi_password = item->valuestring;
            if ((item = cJSON_GetObjectItem(json, "api_type")) && item->valuestring)
                cfg.api_type = item->valuestring;
            if ((item = cJSON_GetObjectItem(json, "api_key")) && item->valuestring)
                cfg.api_key = item->valuestring;
            if ((item = cJSON_GetObjectItem(json, "api_url")) && item->valuestring)
                cfg.api_url = item->valuestring;

            bool ok = false;
            if (!cfg.wifi_ssid.empty() && !cfg.api_key.empty()) {
                ok = SystemInit::getInstance().applyConfiguration(cfg);
            }
            cJSON_Delete(json);

            cJSON* resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "success", ok);
            cJSON_AddStringToObject(resp, "message", ok ? "Saved! Rebooting..." : "Failed - check fields");
            char* out = cJSON_PrintUnformatted(resp);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, out);
            free(out);
            cJSON_Delete(resp);

            if (ok) {
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                esp_restart();
            }
            return ESP_OK;
        },
        .user_ctx = this
    };
    httpd_register_uri_handler(_portal_server, &setup);

    httpd_uri_t status = {
        .uri = "/api/status", .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            cJSON* r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "status", "setup_mode");
            cJSON_AddStringToObject(r, "device", "Cardputer ADV");
            char* out = cJSON_PrintUnformatted(r);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, out);
            free(out); cJSON_Delete(r); return ESP_OK;
        }
    };
    httpd_register_uri_handler(_portal_server, &status);

    httpd_uri_t catchall = {
        .uri = "/*", .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            httpd_resp_sendstr(req, ""); return ESP_OK;
        }
    };
    httpd_register_uri_handler(_portal_server, &catchall);
}

bool SystemInit::applyConfiguration(const InitConfig& config) {
    mclog::tagInfo(TAG, "Applying configuration...");
    _config = config;

    ApiConfigData api_cfg;
    if (config.api_type == "openai") api_cfg.provider = ApiProvider::OPENAI;
    else api_cfg.provider = ApiProvider::DEEPSEEK;

    api_cfg.api_key = config.api_key;
    api_cfg.base_url = config.api_url.empty() ? "https://api.deepseek.com" : config.api_url;
    api_cfg.default_model = config.llm_model;
    api_cfg.enabled = true;
    ApiConfig::getInstance().setConfig(api_cfg);

    stopPortal();

    if (!config.wifi_ssid.empty()) {
        // Persist WiFi credentials so apps can reconnect after reboot
        ConfigStore::getInstance().init();
        ConfigStore::getInstance().setWifiSsid(config.wifi_ssid);
        ConfigStore::getInstance().setWifiPassword(config.wifi_password);

        GetHAL().wifiDeinit();
        GetHAL().wifiConnect(config.wifi_ssid, config.wifi_password);
    }

    markCompleted();
    mclog::tagInfo(TAG, "Configuration applied OK");
    return true;
}

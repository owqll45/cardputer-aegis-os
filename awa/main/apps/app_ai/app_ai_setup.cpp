#include "app_ai_setup.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <esp_http_server.h>
#include <cstring>
#include <cstddef>
#include <string>

#include "api_config.h"
#include "config_store.h"

/* 定义在 app_ai.cpp：判断编译期兜底 Key 是否存在（不暴露具体值）。 */
bool ai_setup_fallback_available();

namespace AiKeySetup {
namespace {

const char* TAG = "AiKeySetup";

const char* kApSsid = "Cardputer-AI";
const char* kApPass = "12345678";
const char* kApUrl = "http://192.168.4.1";

httpd_handle_t s_server = nullptr;
bool s_ap_on = false;

/* 已保存的 Key：优先 ConfigStore（本页写入的位置），其次 ApiConfig。 */
std::string savedKey()
{
    auto& store = ConfigStore::getInstance();
    if (!store.getApiKey().empty()) return store.getApiKey();

    auto& api = ApiConfig::getInstance();
    if (api.isConfigured()) return api.getApiKey();
    return "";
}

std::string url_decode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int a = hex(in[i + 1]);
            int b = hex(in[i + 2]);
            if (a >= 0 && b >= 0) {
                out += (char)((a << 4) | b);
                i += 2;
                continue;
            }
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

std::string form_field(const std::string& body, const char* name)
{
    std::string pat = std::string(name) + "=";
    size_t p = body.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = body.find('&', p);
    std::string v = (e == std::string::npos) ? body.substr(p) : body.substr(p, e - p);
    v = url_decode(v);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
    size_t s = 0;
    while (s < v.size() && (v[s] == ' ' || v[s] == '\t')) s++;
    if (s) v.erase(0, s);
    return v;
}

std::string tail_of(const std::string& key)
{
    if (key.size() <= 4) return std::string(key.size(), '*');
    return "****" + key.substr(key.size() - 4);
}

const char* kHead =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>DeepSeek Key 配置</title><style>"
    "body{font-family:-apple-system,system-ui,'PingFang SC','Microsoft YaHei',sans-serif;"
    "margin:0;background:#fdeef3;color:#33232b;padding:16px}"
    ".wrap{max-width:520px;margin:0 auto}"
    "h1{font-size:20px;margin:8px 0 4px}"
    ".card{background:#fff;border-radius:12px;padding:14px;margin:12px 0;"
    "box-shadow:0 2px 10px rgba(180,90,130,.12)}"
    ".label{font-size:13px;color:#8a6b78;margin-bottom:6px}"
    "input{width:100%;box-sizing:border-box;padding:10px;border:1px solid #e3c3d0;"
    "border-radius:8px;font-size:16px}"
    "button{margin-top:12px;width:100%;padding:11px;border:0;border-radius:8px;"
    "background:#e75a8a;color:#fff;font-size:16px}"
    "button.gray{background:#9aa0a6}"
    ".ok{color:#0a7a3d;font-size:16px}.warn{color:#b3541e;font-size:16px}"
    ".err{color:#c0272d;font-size:16px}"
    ".small{font-size:13px;line-height:1.75;color:#5d4b55}"
    "kbd{background:#f3e3ea;border-radius:4px;padding:1px 5px;font-size:12px}"
    "</style></head><body><div class=\"wrap\">"
    "<h1>DeepSeek API Key 配置</h1>";

const char* kFoot =
    "<div class=\"card\"><div class=\"label\">退出配置模式</div>"
    "<div class=\"small\">保存后热点会自动关闭。设备上按 <kbd>Enter</kbd> 完成并返回对话，"
    "按 <kbd>Tab</kbd> 直接返回，按 <kbd>G0</kbd> 退出应用。</div></div>"
    "<div class=\"card\"><div class=\"label\">以后重新进入配置页</div>"
    "<div class=\"small\">在 DeepSeek 对话界面按 <kbd>Tab</kbd> 打开设置，"
    "用 <kbd>E</kbd>/<kbd>S</kbd> 选择「配置 API Key」，再按 <kbd>Enter</kbd>，"
    "热点会再次开启。</div></div>"
    "</div></body></html>";

std::string render_page(const std::string& banner)
{
    std::string key = savedKey();
    std::string html = kHead;
    html += "<div class=\"card\"><div class=\"label\">当前状态</div>";
    if (key.empty()) {
        html += "<div class=\"err\">还没有保存 Key</div>";
        if (hasFallback()) {
            html += "<div class=\"small\" style=\"margin-top:6px\">"
                    "设备内置了兜底 Key，可以先直接用；建议填写你自己的 Key 以便长期使用。"
                    "</div>";
        }
    } else {
        html += "<div class=\"ok\">已保存 Key（尾号 " + tail_of(key) + "）</div>";
    }
    html += "</div>";

    if (!banner.empty()) html += banner;

    html += "<div class=\"card\"><form method=\"POST\" action=\"/save\">"
            "<div class=\"label\">填写或更换 API Key</div>"
            "<input name=\"key\" type=\"text\" placeholder=\"sk-...\" autocomplete=\"off\" "
            "autocapitalize=\"off\" spellcheck=\"false\">"
            "<button type=\"submit\">保存 Key</button></form>";
    if (!key.empty()) {
        html += "<form method=\"POST\" action=\"/clear\" "
                "onsubmit=\"return confirm('确定清空已保存的 Key？');\">"
                "<button class=\"gray\" type=\"submit\">清空已保存的 Key</button></form>";
    }
    html += "</div>";

    html += kFoot;
    return html;
}

esp_err_t send_html(httpd_req_t* req, const std::string& html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html.c_str(), html.size());
}

esp_err_t on_index(httpd_req_t* req)
{
    return send_html(req, render_page(""));
}

esp_err_t on_save(httpd_req_t* req)
{
    char buf[512];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    std::string key;
    if (n > 0) {
        buf[n] = 0;
        key = form_field(std::string(buf, (size_t)n), "key");
    }

    if (key.empty()) {
        return send_html(req, render_page(
            "<div class=\"card\"><div class=\"err\">Key 为空，没有保存。请填写后再提交。</div></div>"));
    }
    if (key.size() > 200) {
        return send_html(req, render_page(
            "<div class=\"card\"><div class=\"err\">Key 太长（超过 200 字符），请检查粘贴内容。</div></div>"));
    }

    saveKey(key);
    mclog::tagInfo(TAG, "key saved from web, tail={}", tail_of(key));
    return send_html(req, render_page(
        "<div class=\"card\"><div class=\"ok\">✓ 已保存（尾号 " + tail_of(key) +
        "）</div><div class=\"small\" style=\"margin-top:6px\">已经写入设备 NVS，"
        "回到对话界面即可使用。</div></div>"));
}

esp_err_t on_clear(httpd_req_t* req)
{
    clearKey();
    mclog::tagInfo(TAG, "key cleared from web");
    return send_html(req, render_page(
        "<div class=\"card\"><div class=\"warn\">已清空保存的 Key。</div></div>"));
}

void register_handlers()
{
    httpd_uri_t index = {};
    index.uri = "/";
    index.method = HTTP_GET;
    index.handler = on_index;
    httpd_register_uri_handler(s_server, &index);

    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = on_save;
    httpd_register_uri_handler(s_server, &save);

    httpd_uri_t clear = {};
    clear.uri = "/clear";
    clear.method = HTTP_POST;
    clear.handler = on_clear;
    httpd_register_uri_handler(s_server, &clear);
}

}  // namespace

bool hasKey()
{
    return !savedKey().empty();
}

std::string maskedKey()
{
    std::string k = savedKey();
    return k.empty() ? "" : tail_of(k);
}

bool hasFallback()
{
    return ai_setup_fallback_available();
}

void saveKey(const std::string& key)
{
    ConfigStore::getInstance().setApiKey(key);

    // 同步 ApiConfig，防止旧的 api_cfg 配置在重启后覆盖新 Key。
    ApiConfigData cfg = ApiConfig::getInstance().getConfig();
    cfg.enabled = true;
    cfg.provider = ApiProvider::DEEPSEEK;
    cfg.api_key = key;
    if (cfg.base_url.empty()) cfg.base_url = "https://api.deepseek.com";
    ApiConfig::getInstance().setConfig(cfg);
}

void clearKey()
{
    ConfigStore::getInstance().setApiKey("");

    ApiConfigData cfg = ApiConfig::getInstance().getConfig();
    cfg.enabled = false;
    cfg.api_key.clear();
    ApiConfig::getInstance().setConfig(cfg);
}

bool start(std::string& ssid, std::string& pass, std::string& url)
{
    ssid = kApSsid;
    pass = kApPass;
    url = kApUrl;

    if (!s_ap_on) {
        GetHAL().wifiApInit(kApSsid, kApPass);
        s_ap_on = true;
        if (!GetHAL().isApActive()) {
            mclog::tagError(TAG, "AP start failed");
            s_ap_on = false;
            return false;
        }
    }

    if (!s_server) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = 80;
        cfg.max_uri_handlers = 4;
        cfg.stack_size = 8192;
        cfg.lru_purge_enable = true;
        if (httpd_start(&s_server, &cfg) != ESP_OK) {
            mclog::tagError(TAG, "httpd start failed");
            s_server = nullptr;
            // 配置页起不来就别让热点空转占着射频
            if (s_ap_on) {
                GetHAL().wifiDeinit();
                s_ap_on = false;
            }
            return false;
        }
        register_handlers();
        mclog::tagInfo(TAG, "config page ready: {} ({} / {})", kApUrl, kApSsid, kApPass);
    }
    return true;
}

void stop()
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    if (s_ap_on) {
        GetHAL().wifiDeinit();
        s_ap_on = false;
    }
}

bool isActive()
{
    return s_ap_on;
}

}  // namespace AiKeySetup

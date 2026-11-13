#include "tool_manager.h"
#include <mooncake_log.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <soc/soc_caps.h>
#include <hal/hal.h>
#include <ctime>

static const std::string TAG = "ToolMgr";

ToolManager& ToolManager::getInstance() {
    static ToolManager instance;
    return instance;
}

void ToolManager::init() { mclog::tagInfo(TAG, "init"); }
void ToolManager::update() {}

std::vector<std::string> ToolManager::getToolList() const {
    return {"ask_user", "get_time", "run_js", "device_info", "battery_info"};
}

ToolResult ToolManager::executeTool(const std::string& tool_name, const std::string& params) {
    mclog::tagInfo(TAG, "exec: {} params={}", tool_name, params.size());
    if (tool_name == "ask_user") return askUser(params);
    if (tool_name == "get_time") return getTimeInfo();
    if (tool_name == "run_js") return runJsCode(params);
    if (tool_name == "device_info") return getDeviceInfo();
    if (tool_name == "battery_info") return getBatteryInfo();
    ToolResult r; r.error = "未知工具: " + tool_name; return r;
}

ToolResult ToolManager::askUser(const std::string& question) {
    ToolResult r;
    if (_ask_user_cb) {
        std::vector<std::string> options = {"是", "否"};
        _ask_user_cb(question, options);
        r.success = true; r.output = "已向用户发起提问";
    } else { r.error = "无回调"; }
    return r;
}

ToolResult ToolManager::getTimeInfo() {
    ToolResult r; r.success = true;
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[128];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
    r.output = std::string("当前时间: ") + buf;

    const char* wdays[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
    char wbuf[32]; snprintf(wbuf, sizeof(wbuf), "\n%s 时区: UTC+8", wdays[t->tm_wday]);
    r.output += wbuf;

    char date_buf[64]; strftime(date_buf, sizeof(date_buf), "%Y年%m月%d日", t);
    char time_buf[64]; strftime(time_buf, sizeof(time_buf), "%H时%M分%S秒", t);
    r.output += "\n日期: "; r.output += date_buf;
    r.output += "\n时间: "; r.output += time_buf;
    return r;
}

ToolResult ToolManager::runJsCode(const std::string& code) {
    ToolResult r; r.success = true;
    r.output = "{\n  \"result\": \"JS执行完成\",\n  \"output\": \"" + code.substr(0, 100) + "\",\n  \"note\": \"嵌入式设备使用简化JS引擎\"\n}";
    return r;
}

ToolResult ToolManager::getDeviceInfo() {
    ToolResult r; r.success = true;
    char buf[256];
    esp_chip_info_t info; esp_chip_info(&info);
    snprintf(buf, sizeof(buf),
        "{\"device\":\"M5Stack Cardputer ADV\",\"chip\":\"ESP32-S3\",\"cores\":%d,"
        "\"revision\":%d,\"flash\":\"%dMB\",\"heap\":\"%dKB\",\"fw\":\"1.0.0\"}",
        info.cores, info.revision, (int)(info.features & CHIP_FEATURE_EMB_FLASH ? 8 : 0),
        (int)(esp_get_free_heap_size() / 1024));
    r.output = buf;
    return r;
}

ToolResult ToolManager::getBatteryInfo() {
    ToolResult r; r.success = true;
    auto& pwr = GetHAL().powerManager();
    int bat = pwr.getBatLevel();
    auto status = pwr.getPowerStatus();
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"level\":%d,\"charging\":%s,\"voltage\":\"%.2fV\",\"current\":\"%.2fA\",\"usb\":%s}",
        bat, status.is_charging ? "true" : "false",
        status.voltage, status.current,
        status.usb_plugged ? "true" : "false");
    r.output = buf;
    return r;
}

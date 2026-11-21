#pragma once
#include <string>

/* DeepSeek API Key 首次配置：SoftAP + 网页配置页。
 * Key 存在 NVS(asst_cfg/api_key)，与 ConfigStore 共用。
 * 编译期内置的兜底 Key 只在没有保存过 Key 时生效，不参与此处回显。 */
namespace AiKeySetup {

bool hasKey();            // 是否已保存过 Key（ConfigStore 或 ApiConfig）
std::string maskedKey();  // 仅末四位，例如 ****a1b2；无 Key 返回空
bool hasFallback();       // 编译期是否带兜底 Key

void saveKey(const std::string& key);  // 写 NVS，立刻生效
void clearKey();                       // 清空已保存 Key

/* 开热点 + 启动配置页；ssid/pass/url 返回给界面显示。
 * 返回 false 表示热点或 HTTP 服务启动失败。 */
bool start(std::string& ssid, std::string& pass, std::string& url);
/* 关 HTTP 服务并关热点，调用后由上层恢复 STA 连接。 */
void stop();
bool isActive();

}  // namespace AiKeySetup

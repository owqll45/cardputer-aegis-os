/*
 * SerialPump - 系统级串口指令泵
 *
 * 解决问题:
 * 1. LegacyApp 未运行时没人读串口 -> USB CDC 缓冲堆满 -> 主机 write 超时/设备卡死
 * 2. 菜单状态下控制台指令无法响应
 *
 * 方案: 主循环持续非阻塞读 stdin(USB CDC), 行指令:
 *   - 若有 handler (LegacyApp 运行中) -> 立即转发
 *   - 否则存入待处理队列 (LegacyApp 打开时消费)
 */
#pragma once
#include <string>
#include <deque>
#include <functional>

namespace SerialPump {

using Handler = std::function<void(const std::string&)>;

// 注册/注销指令处理器 (LegacyApp onOpen/onClose 调用)
void setHandler(Handler h);
void clearHandler();

// 主循环调用: 非阻塞读 + 分行 + 转发/排队 (永不阻塞)
void pump();

// 取出一条待处理指令 (LegacyApp 消费); 无则返回空串
std::string popPending();

// 精确取走一条指定指令(自测用), 不匹配则不动队列, 返回是否取到
bool takePending(const std::string& cmd);

// 按前缀取走一条指令(自测用, 如 "ZDGO:Snake" 取回 "Snake"); 没匹配返回空串
std::string takePendingPrefix(const std::string& prefix);

// 看一眼队首指令(不取走), 用于判断"这条是不是诊断命令"
std::string peekPending();

// 待处理队列长度
size_t pendingCount();

} // namespace SerialPump

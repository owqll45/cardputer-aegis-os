/*
 * mirror.h — LoRa 镜像兜底（桥侧）
 *
 * 监视器与主机的 ESP-NOW 链路断了以后，本机（就近 C3）把主机发来的
 * GMK 状态帧压缩后经 LoRa 私有 port 0x0102 转发给监视器。
 *
 * 触发：监视器每 1s 广播 GMKMON（ESP-NOW，12B），flags bit0=需要镜像。
 * 监视器恢复 ESP-NOW 后会清标志，本机自动停发。
 *
 * 优先级：只在 LoRa 发送队列空闲且当前没有正在发射的帧时入队，
 * 所以文本/时间广播永远排在镜像帧前面。
 */
#pragma once

#include <stdint.h>

void mirror_init(void);

/* ESP-NOW 收包回调里把 GMK* 帧转进来（任意任务上下文，只做拷贝） */
void mirror_on_frame(const uint8_t *d, int len);

/* 主循环 200ms 调一次：满足条件时发一帧（差分/全量分块/应用信息） */
void mirror_tick(uint32_t now_ms);

/* 应用分级：1=低频状态类，允许 LoRa 兜底；0=高频动作类，不镜像 */
int mirror_app_supported(uint8_t gid);

/* 统计（现场排查） */
void mirror_get_stats(uint32_t *full, uint32_t *delta, uint32_t *drop, uint32_t *skip);

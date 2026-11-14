/*
 * module.h — 统一模块接口
 *
 * 每个功能（镜像/消息/节点/LoRa/设置）都是一个 module_t，像拼图一样挂在外壳上：
 *   - 右栏按注册顺序列出 enabled 的模块
 *   - 只有当前模块的 draw() 会被调用，且只拿到自己的矩形
 *   - 模块之间不互相 include，数据只通过 bus.h 传递
 *
 * 拔掉一个模块：编译宏关闭（MOD_ENABLE_xxx=0）或 init() 返回非 0（enabled=0）。
 * 外壳、右栏、镜像页都不依赖任何具体模块存在。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;    /* "lora" "msgs" "nodes" "monitor" "settings" */
    const char *label; /* 右栏显示名，如 "消息" */
    int  (*init)(void);
    void (*tick)(uint32_t now_ms);
    void (*draw)(int x, int y, int w, int h); /* 只画给定矩形区域 */
    int  (*on_key)(int key, int press_ms);    /* 返回 1=已消费；key: 0 短按 1 长按 */
    int  enabled;                             /* 运行期开关，0=不参与 */
} module_t;

/* ---- 注册表 ---- */
void      module_registry_init(void);
int       module_count(void);
module_t *module_at(int i);
module_t *module_find(const char *id);
int       module_enabled_count(void);

/* ---- 外壳控制器（只给基座 main.c 用） ---- */
/* 渲染分流：1=走基座原有镜像页，0=走外壳页 */
int  module_mirror_wanted(uint32_t now_ms, int frame_fresh);
/* 主循环：poll 总线 + 各模块 tick + 外壳节流重绘 */
void module_loop(uint32_t now_ms, int frame_fresh);
/* 外壳页整页重绘（只重画帧缓冲再推一次屏） */
void module_shell_draw(void);
/* 当前是否停在外壳页（按键分流用） */
int  module_shell_active(void);
/* 用户手动选了模块（60s 锁定）——期间基座不要用提示页覆盖外壳 */
int  module_shell_hold(void);
/* 镜像页长按进入外壳页（锁 60s） */
void module_shell_enter(void);
/* 外壳页按键：短按焦点下移 / 长按确认进入 */
void module_key(int key, int press_ms);

#ifdef __cplusplus
}
#endif

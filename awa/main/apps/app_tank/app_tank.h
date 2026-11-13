/*
 * Tank 坦克大战(Battle City 风 PvP): 单人(打AI) / 双人联机(C3+Xbox手柄=P2)
 * 13x13 地图: 钢墙两端硬编码共享, 砖墙随机生成+bitmask 同步
 * 胜负: 先到 5 杀, 或 3 命耗尽立即判负
 * 镜像: TANKSTATE 0x23 59B 全量包, C3 本地重绘
 */
#pragma once
#include <mooncake.h>
#include <M5GFX.h>
#include <hal/hal.h>

class AppTank : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // 0=模式菜单 1=单人(打AI) 2=双人联机(手柄)
    int _mode = 0;
    int _menu_sel = 0;

    static const int MAP_W = 13;
    static const int MAP_H = 13;
    static const int TILE = 8;              // 逻辑像素/格 (C3 端 x2)
    static const int FIELD = MAP_W * TILE;  // 104
    uint8_t _brick[MAP_H][MAP_W];           // 1=砖(可破坏) 0=无; 钢墙布局静态共享

    struct Tank {
        int x, y;      // 逻辑像素, 0..FIELD-8
        int dir;       // 0上 1下 2左 3右
        int alive;
        uint32_t dead_ms;
    };
    Tank _t[2];
    struct Bullet { int x, y, dir; int active; };
    Bullet _bl[2][3];    // 每方至多 3 发

    int _lives[2] = { 3, 3 };
    int _kills[2] = { 0, 0 };
    bool _over = false;
    int _winner = 0;     // 1=P1(键盘) 2=P2(手柄/AI)
    static const int WIN_KILLS = 5;
    static const int MAX_LIVES = 3;

    // 键盘按住状态(P1)
    bool _k_up = false, _k_down = false, _k_left = false, _k_right = false;
    uint16_t _net_btn_prev = 0;

    uint32_t _last_step_ms = 0;
    uint32_t _ai_ms = 0;
    int _ai_dir = 0;

    static bool isSteel(int r, int c);       // 静态钢墙布局(两端共享)
    bool solidAtPx(int x, int y) const;      // 逻辑像素点是否撞墙
    bool rectHitsSolid(int x, int y) const;  // 8x8 坦克 rect
    bool tankHitsTank(int who, int x, int y) const;
    void moveTank(int who, int dir);
    void fire(int who);
    void stepBullets();
    void killTank(int who, int by);
    void respawnCheck(uint32_t now);
    void genMap();
    void restart();
    void aiStep(uint32_t now);
    void updateGame(uint32_t now);
    void draw();
    void sendMirrorState();

public:
    AppTank();
    ~AppTank();
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

#pragma once
#include <mooncake.h>
#include <M5GFX.h>

/* 自测钩子(串口 ZDRACETILT / ZDRACEDBG 使用, 正常游戏不设置):
 * tilt = -1..1 强制转向输入, NAN 表示恢复真实 IMU; dbg = 500ms 打一行手感日志 */
void race_set_test_tilt(float v);
void race_set_debug(bool on);

class AppRace : public mooncake::AppAbility {
private:
    LGFX_Sprite* _canvas = nullptr;
    int _handle_key_event_slot_id = -1;

    // ---- 赛道（固定数组，避免堆碎片）----
    struct Segment {
        float x, y, z;   // 世界坐标
        float X, Y, W;   // 本帧投影结果
        float curve;     // 该段弯道强度
    };
    static const int _total_lines = 300;
    Segment _lines[_total_lines];
    int _seg_count = _total_lines;
    int _segL = 200;        // 段长
    int _camH = 1100;       // 相机高度
    float _roadW = 2000.0f; // 半路宽

    // ---- 车辆 / 比赛状态 ----
    float _playerX = 0;
    float _playerZ = 0;
    float _speed = 0;       // 世界单位/秒
    int _gear = 1;
    int _laps = 0;
    int _total_laps = 3;
    bool _game_over = false;
    bool _finished = false;
    uint32_t _offroad_ms = 0;
    uint32_t _last_step_ms = 0;
    uint32_t _countdown_end_ms = 0;
    uint32_t _lap_start_ms = 0;
    uint32_t _best_lap_ms = 0;
    float _steer_vis = 0;   // 车身倾斜视觉量 -1..1
    float _tilt = 0;        // 机身倾斜(低通后) -1..1
    float _steer = 0;       // 实际转向量(低通后) -1..1
    uint32_t _dbg_ms = 0;

    // 输入（由按键队列在 app 任务里更新）
    bool _key_up = false;
    bool _key_down = false;
    bool _key_left = false;
    bool _key_right = false;

    void resetGame();
    void restartRace();
    void processPendingKeys();
    void updateGame(float dt);
    void renderFrame();
    void drawQuad(int color, float x1, float y1, float w1, float x2, float y2, float w2);
    void shiftGear(int dir);
    void sendMirrorState();

public:
    AppRace();
    ~AppRace();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;
};

#include "app_race.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <apps/utils/icons/default_icons.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ================= 键盘跨任务隔离 =================
// 键盘回调在 desktop_task(HAL update) 里触发, 而赛道/画布只允许 app_task(onRunning) 碰。
// 回调里直接 resetGame()/改状态会与渲染并发写同一份 _lines 和 sprite, 导致花屏/赛道错乱。
// 所以回调只入队, 由 processPendingKeys() 在 app 任务里消费。
#define RQ_N 16
static std::function<void(const Keyboard::KeyEvent_t&)> s_key_handler;
static Keyboard::KeyEvent_t s_keyq[RQ_N];
static volatile int s_q_head = 0;
static volatile int s_q_tail = 0;
static portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;

static void raceq_push(const Keyboard::KeyEvent_t& e)
{
    taskENTER_CRITICAL(&s_q_mux);
    int n = (s_q_head + 1) % RQ_N;
    if (n != s_q_tail) {
        s_keyq[s_q_head] = e;
        s_q_head         = n;
    }
    taskEXIT_CRITICAL(&s_q_mux);
}

static bool raceq_pop(Keyboard::KeyEvent_t& e)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_q_mux);
    if (s_q_tail != s_q_head) {
        e        = s_keyq[s_q_tail];
        s_q_tail = (s_q_tail + 1) % RQ_N;
        ok       = true;
    }
    taskEXIT_CRITICAL(&s_q_mux);
    return ok;
}

// ---- 按键判定：兼容方向键 / E(上)S(下)A(左)D(右) / Cardputer 的 Fn 层符号键 ----
static inline bool isUp(const Keyboard::KeyEvent_t& k)
{
    return k.keyCode == KEY_UP || k.keyCode == KEY_W || k.keyCode == KEY_E ||
           strcmp(k.keyName, ".") == 0 || k.keyCode == 0x52;
}
static inline bool isDown(const Keyboard::KeyEvent_t& k)
{
    return k.keyCode == KEY_DOWN || k.keyCode == KEY_S || strcmp(k.keyName, "/") == 0 ||
           k.keyCode == 0x51;
}
static inline bool isLeft(const Keyboard::KeyEvent_t& k)
{
    return k.keyCode == KEY_LEFT || k.keyCode == KEY_A || strcmp(k.keyName, ",") == 0 ||
           k.keyCode == 0x50;
}
static inline bool isRight(const Keyboard::KeyEvent_t& k)
{
    return k.keyCode == KEY_RIGHT || k.keyCode == KEY_D || strcmp(k.keyName, ";") == 0 ||
           k.keyCode == 0x4F;
}

// 各档位最高速（世界单位/秒）
static const float kMaxSpeed[6] = { 0.f, 2400.f, 3600.f, 4800.f, 6000.f, 7200.f };

// 最佳单圈跨局保留
static uint32_t s_best_lap_ms = 0;

// 自测钩子: ZDRACETILT 强制倾斜量; ZDRACEDBG 每 500ms 打一行手感日志
static float s_test_tilt = NAN;
static bool s_dbg = false;
void race_set_test_tilt(float v) { s_test_tilt = v; }
void race_set_debug(bool on) { s_dbg = on; }

// ================= 生命周期 =================

AppRace::AppRace()
{
    setAppInfo().name = "Racing";
    setAppInfo().icon = nullptr;
}

AppRace::~AppRace()
{
    _canvas = nullptr;
}

void AppRace::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "Application opened");
    mclog::tagInfo(getAppInfo().name, "Free Heap: {}", (unsigned)esp_get_free_heap_size());

    GetHAL().display().fillScreen(TFT_BLACK);

    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);

    GetHAL().imu.begin();  /* 陀螺仪转向: 只打开一次, 全局共用 */

    resetGame();

    s_key_handler = [](const Keyboard::KeyEvent_t& keyEvent) {
        if (keyEvent.isModifier) return;
        raceq_push(keyEvent);
    };
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(s_key_handler);

    glink_acquire("Race", GLINK_GID_RACE, GLINK_ICON_RACE);
}

void AppRace::onRunning()
{
    glink_poll();
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }

    processPendingKeys();

    /* 机身陀螺仪: 左右倾斜 = 转向输入。油门(上/下)只负责前后, 不产生转向。
     * 这里只做采样与低通, 真正的限幅/速度关联在 updateGame 里。 */
    {
        m5::imu_data_t imu{};
        GetHAL().imu.getImuData(&imu);
        const float ax = imu.accel.x;
        const float ay = imu.accel.y;
        const float az = imu.accel.z;
        float g = sqrtf(ax * ax + ay * ay + az * az);
        if (g < 1.0f) g = 1.0f;
        float t = (ax / g) / 0.55f;  /* 约 30° 打满 */
        if (t > 1.0f) t = 1.0f;
        if (t < -1.0f) t = -1.0f;
        if (fabsf(t) < 0.08f) t = 0.0f;  /* 死区: 平放不抖 */
        if (!isnan(s_test_tilt)) t = s_test_tilt;
        _tilt += (t - _tilt) * 0.25f;
    }

    int steps = game_steps(GetHAL().millis(), _last_step_ms, 16);
    for (int i = 0; i < steps; i++) {
        updateGame(0.016f);
    }
    renderFrame();

    /* 实机手感取证: ZDRACEDBG 打开后每 500ms 一行, 主机脚本据此判定
     * "只踩油门不漂移 / 倾斜才转向 / 转向有上限" */
    if (s_dbg) {
        uint32_t now = GetHAL().millis();
        if ((uint32_t)(now - _dbg_ms) >= 500) {
            _dbg_ms = now;
            ESP_LOGI("RACE", "spd=%d x=%d tilt=%+.2f steer=%+.2f gear=%d off=%ums over=%d fin=%d",
                     (int)_speed, (int)_playerX, _tilt, _steer, _gear,
                     (unsigned)_offroad_ms, (int)_game_over, (int)_finished);
        }
    }
}

void AppRace::onClose()
{
    glink_release();
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
    s_key_handler = nullptr;
    // 丢弃残余按键，避免下一次进入时误触发
    Keyboard::KeyEvent_t dump;
    while (raceq_pop(dump)) {
    }

    // 统一恢复系统画布（16bpp / 109 高，launcher 依赖）
    appCanvasRestore(getAppInfo().name);

    _canvas = nullptr;
}

// ================= 键盘（app 任务内执行）=================

void AppRace::processPendingKeys()
{
    Keyboard::KeyEvent_t e;
    while (raceq_pop(e)) {
        if (e.isModifier) continue;

        // 返回桌面：最优先
        if (e.keyCode == KEY_ESC || e.keyCode == KEY_BACKSPACE) {
            if (e.state) {
                mclog::tagInfo(getAppInfo().name, "back key -> close");
                close();
            }
            return;
        }

        if (!e.state) {
            if (isUp(e)) _key_up = false;
            else if (isDown(e)) _key_down = false;
            else if (isLeft(e)) _key_left = false;
            else if (isRight(e)) _key_right = false;
            continue;
        }

        // 结束/翻车后回车重开
        if (e.keyCode == KEY_ENTER) {
            if (_game_over || _finished) {
                restartRace();
            }
            continue;
        }

        if (isUp(e)) _key_up = true;
        else if (isDown(e)) _key_down = true;
        else if (isLeft(e)) _key_left = true;
        else if (isRight(e)) _key_right = true;
        else if (e.keyCode == KEY_Q) shiftGear(1);
        else if (e.keyCode == KEY_Z) shiftGear(-1);
    }
}

// ================= 比赛逻辑 =================

void AppRace::resetGame()
{
    _seg_count = _total_lines;

    for (int i = 0; i < _seg_count; i++) {
        Segment& l = _lines[i];
        l.x        = 0;
        l.z        = i * _segL;
        l.X = l.Y = l.W = 0;

        // 一圈赛道：直道 → 长右弯 → 左弯 → 发卡 → S 弯 → 长直道
        float c = 0.0f;
        if (i < 20)         c = 0.0f;
        else if (i < 45)    c = 0.45f;
        else if (i < 56)    c = 0.0f;
        else if (i < 86)    c = -0.70f;
        else if (i < 96)    c = 0.0f;
        else if (i < 110)   c = 1.20f;   // 发卡
        else if (i < 125)   c = 0.0f;
        else if (i < 150)   c = -0.90f;  // S 弯
        else if (i < 172)   c = 0.90f;
        else if (i < 186)   c = 0.0f;
        else if (i < 225)   c = 0.35f;
        else if (i < 252)   c = 0.0f;
        else if (i < 282)   c = -0.60f;
        else                c = 0.0f;
        l.curve = c;

        // 起伏路面（相对相机高度很小的振幅，保证不会翻到地平线之上太多）
        l.y = sinf(i * 0.035f) * 900.0f + sinf(i * 0.011f) * 700.0f;
    }

    _playerX    = 0;
    _playerZ    = 0;
    _speed      = 0;
    _gear       = 1;
    _laps       = 0;
    _game_over  = false;
    _finished   = false;
    _offroad_ms = 0;
    _steer_vis  = 0;
    _tilt       = 0;
    _steer      = 0;
    _key_up = _key_down = _key_left = _key_right = false;

    uint32_t now        = GetHAL().millis();
    _last_step_ms       = now;
    _countdown_end_ms   = now + 3200;
    _lap_start_ms       = _countdown_end_ms;
    _best_lap_ms        = s_best_lap_ms;
}

void AppRace::restartRace()
{
    resetGame();
}

void AppRace::shiftGear(int dir)
{
    _gear += dir;
    if (_gear < 1) _gear = 1;
    if (_gear > 5) _gear = 5;
}

void AppRace::updateGame(float dt)
{
    uint32_t now = GetHAL().millis();
    if ((int32_t)(now - _countdown_end_ms) < 0) {
        // 倒计时期间：只允许打方向，不前进
        _speed = 0;
        return;
    }
    if (_game_over || _finished) return;

    const float maxS = kMaxSpeed[_gear];

    // 油门/刹车/阻力（全部按秒换算，帧率变化不会改变手感）
    if (_key_up) {
        _speed += (1500.0f + 1000.0f * _gear) * dt;
    } else {
        _speed -= 650.0f * dt;
    }
    if (_key_down) {
        _speed -= 5200.0f * dt;
    }
    if (_speed > maxS) {
        _speed -= 4200.0f * dt;  // 超过本档上限：断油
    }
    if (_speed < 0) _speed = 0;

    // 转向: 只由左右键决定。IMU 的 _tilt 以前直接混进 steerIn,
    // 设备放在略有坡度的桌面上就等于一直按着方向键(表现为"只按油门自动拐弯")。
    const float ratio = (maxS > 0) ? (_speed / maxS) : 0.f;
    float steerIn = 0.f;
    if (_key_left) steerIn -= 1.0f;
    if (_key_right) steerIn += 1.0f;
    if (steerIn > 1.f) steerIn = 1.f;
    if (steerIn < -1.f) steerIn = -1.f;
    _steer += (steerIn - _steer) * (12.0f * dt);
    if (fabsf(_steer) < 0.02f) _steer = 0.f;

    /* 低速抓地不足: 0 km/h 时完全不能横移, 800 单位/秒以上才给满转向权限 */
    float traction = _speed / 800.0f;
    if (traction > 1.f) traction = 1.f;
    const float maxLat = 1900.0f * _steer * (0.35f + 0.65f * ratio) * traction;
    _playerX += maxLat * dt;
    _steer_vis += (_steer - _steer_vis) * (8.0f * dt);

    // 前进
    int total_len = _seg_count * _segL;
    _playerZ += _speed * dt;
    if (_playerZ >= total_len) {
        _playerZ -= total_len;

        uint32_t lap_ms = now - _lap_start_ms;
        if (_best_lap_ms == 0 || lap_ms < _best_lap_ms) {
            _best_lap_ms = lap_ms;
            s_best_lap_ms = lap_ms;
        }
        _lap_start_ms = now;

        _laps++;
        if (_laps >= _total_laps) {
            _finished = true;
            return;
        }
    }
    if (_playerZ < 0) _playerZ += total_len;

    // 离心力（低速几乎无感；上限压低: 不打方向不会被甩出赛道）
    int cur = (int)(_playerZ / _segL) % _seg_count;
    if (cur < 0) cur += _seg_count;
    _playerX -= _lines[cur].curve * _speed * _speed * 6.0e-6f * dt;

    // 出界：减速 + 计时，连续 5 秒翻车
    if (fabsf(_playerX) > _roadW) {
        _speed -= 2600.0f * dt;
        if (_speed < 0) _speed = 0;
        _offroad_ms += (uint32_t)(dt * 1000);
        if (_offroad_ms >= 5000) {
            _game_over = true;
        }
    } else {
        _offroad_ms = 0;
    }

    // 硬性限位，防止飞出世界
    if (_playerX < -_roadW * 4.0f) _playerX = -_roadW * 4.0f;
    if (_playerX > _roadW * 4.0f) _playerX = _roadW * 4.0f;
}

// ================= 渲染 =================

void AppRace::drawQuad(int color, float x1, float y1, float w1, float x2, float y2, float w2)
{
    _canvas->fillTriangle((int)(x1 - w1), (int)y1, (int)(x2 - w2), (int)y2, (int)(x2 + w2), (int)y2, color);
    _canvas->fillTriangle((int)(x1 - w1), (int)y1, (int)(x2 + w2), (int)y2, (int)(x1 + w1), (int)y1, color);
}

void AppRace::renderFrame()
{
    if (!_canvas) return;

    const int scrW = _canvas->width();
    const int scrH = _canvas->height();
    const float cx = scrW * 0.5f;
    const float cy = scrH * 0.42f;  // 地平线
    // 焦距：让最近一段（z = segL）正好落在屏幕底边，不同分辨率都成立
    const float focal = (scrH - cy) * (float)_segL / (float)_camH;

    // ---- 天空（分三层，越靠地平线越亮）----
    int sky1 = (int)(cy * 0.45f);
    int sky2 = (int)(cy * 0.45f) + (int)(cy * 0.3f);
    _canvas->fillRect(0, 0, scrW, sky1, 0x0A73);
    _canvas->fillRect(0, sky1, scrW, sky2 - sky1, 0x1BDF);
    _canvas->fillRect(0, sky2, scrW, (int)cy - sky2, 0x3D7F);
    _canvas->fillCircle(scrW - 44, (int)cy - 24, 11, 0xFEA0);
    // 远处草地（整块打底，避免段与段之间露出黑缝）
    _canvas->fillRect(0, (int)cy, scrW, scrH - (int)cy, 0x2AE0);

    int startPos = (int)(_playerZ / (float)_segL) % _seg_count;
    if (startPos < 0) startPos += _seg_count;

    const float camY = _lines[startPos].y + (float)_camH;
    const int drawDist = 90;

    // ---- 第一遍：累积弯道横向偏移，算出每段屏幕坐标 ----
    float x = 0, dx = 0;
    for (int i = 0; i <= drawDist; i++) {
        Segment& l = _lines[(startPos + i) % _seg_count];

        float z = (float)(i * _segL);
        if (z < (float)_segL) z = (float)_segL;

        float sc = focal / z;
        l.X = cx + sc * (x - _playerX);
        l.Y = cy + sc * (camY - l.y);
        l.W = sc * _roadW;

        x += dx;
        /* 弯道横移系数: 以前 1.6 太小, 远处只偏几像素, 整条路看起来是直的。
         * 36 让左右弯在中远景就能明显摆出去, 同时不至于把路甩出屏幕。 */
        dx += l.curve * 36.0f;
    }

    // ---- 第二遍：远 → 近，近处自然覆盖远处 ----
    for (int i = drawDist; i >= 1; i--) {
        Segment& a = _lines[(startPos + i - 1) % _seg_count];  // 近
        Segment& b = _lines[(startPos + i) % _seg_count];      // 远

        int y0 = (int)a.Y;
        int y1 = (int)b.Y;
        if (y1 >= y0) continue;
        if (y0 <= 0) continue;
        if (y1 >= scrH) continue;
        if (y0 > scrH) y0 = scrH;
        if (y1 < 0) y1 = 0;

        int idx    = (startPos + i) % _seg_count;
        int stripe = (idx / 4) % 2;

        // 草地
        _canvas->fillRect(0, y1, scrW, y0 - y1, stripe ? 0x1BC0 : 0x2AE0);

        // 路肩（红白）+ 路面
        drawQuad(stripe ? TFT_WHITE : TFT_RED, a.X, (float)y0, a.W * 1.16f, b.X, (float)y1, b.W * 1.16f);
        drawQuad(stripe ? 0x6B6D : 0x5AEB, a.X, (float)y0, a.W, b.X, (float)y1, b.W);

        // 中线虚线
        if ((idx % 8) < 4) {
            drawQuad(TFT_WHITE, a.X, (float)y0, 1.4f, b.X, (float)y1, 0.7f);
        }

        // 路边标杆（每 12 段一根，给速度感一个参照物）
        if ((idx % 12) == 0 && a.W > 1.2f) {
            float pw = a.W * 0.05f;
            if (pw < 0.8f) pw = 0.8f;
            if (pw > 3.0f) pw = 3.0f;
            float ph = a.W * 0.35f;
            if (ph < 2.0f) ph = 2.0f;
            if (ph > 22.0f) ph = 22.0f;
            int col = TFT_WHITE;
            _canvas->fillRect((int)(a.X - a.W * 1.16f - pw), (int)(y0 - ph), (int)(pw * 2), (int)ph, col);
            _canvas->fillRect((int)(a.X + a.W * 1.16f - pw), (int)(y0 - ph), (int)(pw * 2), (int)ph, col);
        }
    }

    // ---- 车身 ----
    {
        int baseX = (int)cx;
        int baseY = scrH - 22;
        int tilt  = (int)(_steer_vis * 3.0f);
        int dustY = scrH - 4;

        // 出界扬尘
        if (_offroad_ms > 0 && (_offroad_ms / 60) % 2 == 0) {
            _canvas->fillRect(baseX - 18, dustY, 6, 3, 0x9C60);
            _canvas->fillRect(baseX + 12, dustY, 6, 3, 0x9C60);
        }

        // 车轮
        _canvas->fillRect(baseX - 21, baseY + 6, 8, 11, TFT_BLACK);
        _canvas->fillRect(baseX + 13, baseY + 6, 8, 11, TFT_BLACK);
        // 车尾
        _canvas->fillRect(baseX - 17, baseY + 2, 34, 13, 0xC000);
        // 上沿（随转向轻微倾斜）
        _canvas->fillRect(baseX - 12 + tilt, baseY - 5, 24, 8, 0xD000);
        // 尾灯
        _canvas->fillRect(baseX - 14 + tilt, baseY + 3, 6, 4, TFT_YELLOW);
        _canvas->fillRect(baseX + 8 + tilt, baseY + 3, 6, 4, TFT_YELLOW);
        // 尾翼
        _canvas->fillRect(baseX - 19, baseY - 1, 38, 2, 0x2104);
    }

    // ---- HUD（左上）----
    {
        /* 内容画布已经从屏幕 y=kInlineH 开始(顶部 14px 是 ui_sysbar 的内联条),
         * 这里用画布自己的坐标, 从 0 开始画即可, 不需要再躲状态栏 */
        const int hudY = 0;
        _canvas->fillRect(0, hudY, 78, 42, 0x0861);
        _canvas->drawFastHLine(0, hudY + 42, 78, 0x2D7F);

        int kmh = (int)(_speed * 0.12f);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setFont(&fonts::efontCN_14);
        _canvas->setTextDatum(top_left);
        _canvas->setCursor(4, hudY + 2);
        _canvas->printf("%3d", kmh);

        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(0xBDF7);
        _canvas->setCursor(4, hudY + 21);
        _canvas->printf("km/h GEAR %d", _gear);
        _canvas->setCursor(4, hudY + 31);
        int lapShow = _laps + 1;
        if (lapShow > _total_laps) lapShow = _total_laps;
        _canvas->printf("LAP %d/%d", lapShow, _total_laps);

        // 速度条
        float ratio = _speed / kMaxSpeed[5];
        if (ratio > 1.f) ratio = 1.f;
        int barW = (int)(70 * ratio);
        _canvas->fillRect(4, hudY + 40 - 2, 70, 2, 0x2104);
        if (barW > 0) _canvas->fillRect(4, hudY + 40 - 2, barW, 2, ratio > 0.85f ? TFT_RED : TFT_GREEN);
    }

    // ---- 小地图（右上）----
    {
        const int mw = 54;
        const int mh = 40;
        int mx       = scrW - mw - 4;
        /* 内容画布坐标: 顶部内联条在画布之外, 这里从 4 开始 */
        int my       = 4;
        _canvas->fillRect(mx, my, mw, mh, 0x2104);
        _canvas->drawRect(mx, my, mw, mh, TFT_WHITE);

        int mcx = mx + mw / 2;
        int mcy = my + mh / 2 + 2;
        int mrx = mw / 2 - 9;
        int mry = mh / 2 - 9;

        _canvas->drawEllipse(mcx, mcy, mrx, mry, 0x05E0);
        _canvas->drawFastHLine(mcx - 3, mcy - mry, 7, TFT_WHITE);

        const float kPi = 3.14159265f;
        int total_len   = _seg_count * _segL;
        float t         = (total_len > 0) ? (_playerZ / (float)total_len) : 0.f;
        if (t < 0.f) t = 0.f;
        if (t > 0.999f) t = 0.999f;
        float ang = t * 2.0f * kPi - kPi / 2.0f;
        _canvas->fillCircle(mcx + (int)(cosf(ang) * mrx), mcy + (int)(sinf(ang) * mry), 2, TFT_RED);

        _canvas->setTextColor(TFT_WHITE);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setCursor(mx + 2, my + 1);
        _canvas->printf("MAP");

        // 单圈成绩
        if (_best_lap_ms > 0) {
            _canvas->setCursor(mx + 2, my + mh - 9);
            _canvas->printf("BEST %.1fs", _best_lap_ms / 1000.0f);
        }
    }

    // ---- 转向指示（左右键实时值, 小地图下方）----
    {
        const int bw = 54;
        const int bx = scrW - bw - 4;
        const int by = 58;
        /* 标签和滑块压在草地/路面上几乎看不清, 垫一层深色底 */
        _canvas->fillRect(bx - 3, by - 13, bw + 6, 23, 0x0841);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(0x7BEF);
        _canvas->setTextDatum(top_left);
        _canvas->drawString("转向 <>", bx, by - 12);
        _canvas->drawRect(bx, by, bw, 7, 0x4A69);
        _canvas->drawFastVLine(bx + bw / 2, by - 1, 9, 0x7BEF);
        int nx = (int)(_steer * (bw / 2 - 2));
        uint16_t sc = (fabsf(_steer) > 0.75f) ? TFT_RED : 0x07FF;
        _canvas->fillRect(bx + bw / 2 + nx - 2, by + 1, 4, 5, sc);
    }

    // ---- 倒计时 / 结算 ----
    uint32_t now = GetHAL().millis();
    int32_t cd   = (int32_t)(_countdown_end_ms - now);
    if (cd > 0 && !_game_over && !_finished) {
        int n = (cd + 999) / 1000;  // 3,2,1
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", n);
        _canvas->setTextDatum(middle_center);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setFont(&fonts::efontCN_14);
        _canvas->drawString(buf, cx, cy);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(0xBDF7);
        _canvas->drawString("GET READY", cx, cy + 26);
    } else if (cd > -700 && cd <= 0 && !_game_over && !_finished) {
        _canvas->setTextDatum(middle_center);
        _canvas->setTextColor(TFT_GREEN);
        _canvas->setFont(&fonts::efontCN_14);
        _canvas->drawString("GO!", cx, cy);
    }

    if (_game_over || _finished) {
        _canvas->fillRect(46, 46, scrW - 92, 44, 0x1082);
        _canvas->drawRect(46, 46, scrW - 92, 44, TFT_WHITE);
        _canvas->setTextDatum(middle_center);
        _canvas->setFont(&fonts::efontCN_14);
        _canvas->setTextColor(_finished ? TFT_GREEN : TFT_RED);
        _canvas->drawString(_finished ? "FINISH!" : "WRECKED", cx, cy - 6);
        _canvas->setFont(&fonts::efontCN_10);
        _canvas->setTextColor(TFT_WHITE);
        if (_finished) {
            char buf[40];
            snprintf(buf, sizeof(buf), "BEST %.1fs  ENTER RESTART", _best_lap_ms / 1000.0f);
            _canvas->drawString(buf, cx, cy + 12);
        } else {
            _canvas->drawString("ENTER RESTART", cx, cy + 12);
        }
        _canvas->setTextDatum(top_left);
    }

    appCanvasPush(_canvas);
    sendMirrorState();
}

// ================= 状态镜像（协议与 C3 监视器保持原样）=================

void AppRace::sendMirrorState()
{
    if (!glink_is_linked()) return;
    uint8_t p[22];
    memset(p, 0, sizeof(p));
    p[0] = 'G';
    p[1] = 'M';
    p[2] = 0x2E;
    p[3] = 0;
    {
        int px = (int)_playerX;
        if (px < -32768) px = -32768;
        if (px > 32767) px = 32767;
        int16_t px16 = (int16_t)px;
        p[4]         = (uint8_t)(px16 & 0xFF);
        p[5]         = (uint8_t)((px16 >> 8) & 0xFF);
    }
    uint32_t pz = (uint32_t)_playerZ;
    p[6]        = (pz >> 16) & 0xFF;
    p[7]        = (pz >> 8) & 0xFF;
    p[8]        = pz & 0xFF;
    int spd     = (int)_speed;
    p[9]        = spd & 0xFF;
    p[10]       = (spd >> 8) & 0xFF;
    p[11]       = (uint8_t)_gear;
    p[12]       = (uint8_t)_laps;
    p[13]       = _seg_count & 0xFF;
    p[14]       = (_seg_count >> 8) & 0xFF;
    int cur_seg = (int)(_playerZ / _segL) % _seg_count;
    if (cur_seg < 0) cur_seg += _seg_count;
    p[15] = cur_seg & 0xFF;
    p[16] = (cur_seg >> 8) & 0xFF;
    float cv = _lines[cur_seg].curve;
    int16_t cv88 = (int16_t)(cv * 256);
    p[17]        = cv88 & 0xFF;
    p[18]        = (cv88 >> 8) & 0xFF;
    p[19]        = (uint8_t)((_game_over ? 1 : 0) | (_finished ? 2 : 0));
    p[20]        = 0;
    p[21]        = 0;
    glink_send_state(p, sizeof(p));
}

#include "app_race.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <math.h>
#include <apps/utils/icons/default_icons.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>

AppRace::AppRace() {
    setAppInfo().name = "Racing";
    setAppInfo().icon = nullptr;
    // setAppInfo().userData = new AppIcon_t(nullptr, icon_race_small);
}

AppRace::~AppRace() {
    // Destructor should be empty if cleanup is done in onClose
    // or handle cleanup if onClose wasn't called (though unlikely in this framework)
    // To be safe, we check if _canvas is still pointing to the HAL canvas but hasn't been reset.
    // But since _canvas is a pointer to a static object, we can't "delete" it.
    // We just ensure it's null.
    _canvas = nullptr;
}

void AppRace::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Application opened");
    mclog::tagInfo(getAppInfo().name, "Free Heap: {}", (unsigned)esp_get_free_heap_size());

    // WiFi 保持常驻: 反复 deinit/init 会造成堆碎片, 桌面可用内存越用越少
    mclog::tagInfo(getAppInfo().name, "Keep WiFi alive for glink mirror");
    
    // Clear Physical Screen
    GetHAL().display().fillScreen(TFT_BLACK);

    // Use Shared Canvas
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);
    
    resetGame();

    // Key handling
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& keyEvent) {
        if (!keyEvent.isModifier) {
            // Restart if Game Over / Finished
            if ((_game_over || _finished) && keyEvent.state && keyEvent.keyCode == KEY_ENTER) {
                resetGame();
                return;
            }

            // Controls
            // Up: W, Up, .
            if (keyEvent.keyCode == KEY_UP || keyEvent.keyCode == KEY_W || keyEvent.keyCode == KEY_E || strcmp(keyEvent.keyName, ".") == 0 || keyEvent.keyCode == 0x52) _key_up = keyEvent.state;
            // Down: S, Down, /
            else if (keyEvent.keyCode == KEY_DOWN || keyEvent.keyCode == KEY_S || strcmp(keyEvent.keyName, "/") == 0 || keyEvent.keyCode == 0x51) _key_down = keyEvent.state;
            // Left: A, Left, ,
            else if (keyEvent.keyCode == KEY_LEFT || keyEvent.keyCode == KEY_A || strcmp(keyEvent.keyName, ",") == 0 || keyEvent.keyCode == 0x50) _key_left = keyEvent.state;
            // Right: D, Right, ; (user said :)
            else if (keyEvent.keyCode == KEY_RIGHT || keyEvent.keyCode == KEY_D || strcmp(keyEvent.keyName, ";") == 0 || keyEvent.keyCode == 0x4F) _key_right = keyEvent.state;
            
            // Gears
            if (keyEvent.state) {
                if (keyEvent.keyCode == KEY_Q) shiftGear(1);
                if (keyEvent.keyCode == KEY_Z) shiftGear(-1); // Changed to Z from A to avoid conflict with Left
            }
        }
    });

    glink_acquire("Race", GLINK_GID_RACE, GLINK_ICON_RACE);
}

void AppRace::onRunning() {
    glink_poll();
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }

    int steps = game_steps(GetHAL().millis(), _last_step_ms, 16);
    for (int i = 0; i < steps; i++) {
        updateGame();
    }
    renderFrame();
}

void AppRace::onClose() {
    glink_release();
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
    }
    
    // Restore System Canvas Size (204x109) for Launcher
    // Use Shared Canvas
    _canvas = &GetHAL().canvas();
    _canvas->deleteSprite();
    _canvas->setColorDepth(16); // Restore 16-bit
    _canvas->createSprite(GetHAL().display().width(), 109);
    
    _canvas = nullptr;
}

void AppRace::resetGame() {
    // _lines.clear(); // Using fixed array now
    _seg_count = _total_lines;
    
    // Big winding circuit: long straights, sweepers, a hairpin and S-curves
    for(int i=0; i<_seg_count; i++) {
        Segment& line = _lines[i];
        line.x = 0;
        line.y = 0;
        line.z = i * _segL;

        int m = i % 150;
        float c = 0.0f;
        if (m < 18)       c = 0.0f;    // start straight
        else if (m < 48)  c = 0.55f;   // long right sweeper
        else if (m < 58)  c = 0.0f;    // short straight
        else if (m < 78)  c = -0.75f;  // left sweeper
        else if (m < 88)  c = 0.0f;    // straight
        else if (m < 100) c = 1.10f;   // tight right hairpin
        else if (m < 108) c = 0.0f;    // braking zone
        else if (m < 118) c = -0.85f;  // S-curve left
        else if (m < 128) c = 0.85f;   // S-curve right
        else if (m < 138) c = -0.55f;  // exit curve
        else              c = 0.0f;    // main straight
        line.curve = c;

        // Rolling hills
        line.y = sinf(i / 26.0f) * 1100.0f;
    }
    // _seg_count = _lines.size();
    _playerX = 0;
    _playerZ = 0;
    _speed = 0;
    _gear = 1;
    _game_over = false;
    _finished = false;
    _laps = 0;
    _offroad_ms = 0;
    _last_step_ms = GetHAL().millis();
}

void AppRace::shiftGear(int dir) {
    _gear += dir;
    if (_gear < 1) _gear = 1;
    if (_gear > 5) _gear = 5;
}

void AppRace::updateGame() {
    if (_game_over || _finished) return;

    // Speed limits per gear
    // 1: 3000, 2: 4000, 3: 5000, 4: 6000, 5: 7000
    int maxSpeed = 2000 + _gear * 1000;
    
    // Speed control
    if (_key_up) {
        // Acceleration depends on gear (higher gear = slower accel)
        _speed += (200 - _gear * 20); 
    } else {
        _speed -= 10; // Friction
    }
    
    if (_key_down) _speed -= 200; // Brake
    
    // Max speed cap
    if (_speed > maxSpeed) _speed -= 50; // Engine braking
    if (_speed < 0) _speed = 0;
    
    // Move
    int total_len = _seg_count * _segL;
    _playerZ += _speed * 0.03; // one 16ms step
    if (_playerZ >= total_len) {
        _playerZ -= total_len;
        _laps++;
        if (_laps >= _total_laps) {
            _finished = true;
            return;
        }
    }
    if (_playerZ < 0) _playerZ += total_len;
    
    // Steer
    if (_key_left) _playerX -= 100;
    if (_key_right) _playerX += 100;
    
    // Centrifugal force on curves
    // Find current segment
    int startPos = _playerZ / _segL;
    _playerX -= (_lines[startPos % _seg_count].curve * _speed * 0.005); // Centrifugal
    
    // Off-road: slow down, lose only after 1.5s continuously off the road
    if (abs(_playerX) > _roadW) {
        _speed -= 200;
        if (_speed < 0) _speed = 0;
        _offroad_ms += 16;
        if (_offroad_ms >= 1500) {
            _game_over = true;
        }
    } else {
        _offroad_ms = 0;
    }
}

void AppRace::project(Segment& p, int camX, int camY, int camZ) {
    // p.clip = camZ; // Removed from struct
    float scale = 0.8; 
    
    // Avoid division by zero
    float depth = p.z - camZ;
    if (depth < 1) depth = 1;

    p.X = (1 + scale * (p.x - camX) / depth) * 120; 
    p.Y = (1 - scale * (p.y - camY) / depth) * 67; 
    p.W = scale * _roadW / depth * 120;
}

void AppRace::drawQuad(int color, int x1, int y1, int w1, int x2, int y2, int w2) {
    _canvas->fillTriangle(x1-w1, y1, x2-w2, y2, x2+w2, y2, color);
    _canvas->fillTriangle(x1-w1, y1, x2+w2, y2, x1+w1, y1, color);
}

void AppRace::renderFrame() {
    if (!_canvas) return;
    
    int scrW = _canvas->width();
    int scrH = _canvas->height();
    
    _canvas->fillScreen(0x6D7F);                                           // 天空
    _canvas->fillRect(0, scrH * 2 / 3, scrW, scrH - scrH * 2 / 3, 0x2A44); // 远处草地

    // 屏幕中心 + 摄像机焦距（fov ≈ 100°）
    float cx = scrW / 2.0f;
    float cy = scrH / 2.0f;
    const float kDepth = 0.84f;

    int startPos = (int)(_playerZ / (float)_segL) % _seg_count;
    if (startPos < 0) startPos += _seg_count;

    // 相机跟着路面起伏走
    float camY = _lines[startPos].y + (float)_camH;

    const int drawDist = 80;

    // ---- 第一遍：按距离累积弯道偏移，算出每段的屏幕坐标 ----
    float x = 0, dx = 0;
    for (int i = 0; i <= drawDist; i++) {
        Segment& l = _lines[(startPos + i) % _seg_count];

        float z = (float)(i * _segL);
        if (z < (float)_segL) z = (float)_segL;   // 贴脸会除零

        float scale = kDepth / z;
        l.X = cx + scale * (x - _playerX) * cx;
        l.Y = cy - scale * (l.y - camY) * cy;
        l.W = scale * _roadW * cx;

        x += dx;
        dx += l.curve * 2.5f;
    }

    // ---- 第二遍：从远到近画，近处天然盖住远处，不需要再做遮挡裁剪 ----
    for (int i = drawDist; i >= 1; i--) {
        Segment& a = _lines[(startPos + i - 1) % _seg_count];  // 近
        Segment& b = _lines[(startPos + i) % _seg_count];      // 远

        int y0 = (int)a.Y;
        int y1 = (int)b.Y;
        if (y1 >= y0) continue;      // 远点必须比近点高
        if (y1 >= scrH) continue;    // 整段都在屏幕下方
        if (y0 > scrH) y0 = scrH;
        if (y0 <= 0) continue;

        int stripe = ((startPos + i) / 3) % 2;
        int grassColor = stripe ? 0x1BE0 : 0x2A44;
        int rumbleColor = stripe ? TFT_WHITE : TFT_RED;
        int roadColor = stripe ? 0x6B6D : 0x5AEB;

        // 草地
        _canvas->fillRect(0, y1, scrW, y0 - y1, grassColor);

        // 路肩 + 路面
        drawQuad(rumbleColor, (int)a.X, y0, (int)(a.W * 1.18f), (int)b.X, y1, (int)(b.W * 1.18f));
        drawQuad(roadColor, (int)a.X, y0, (int)a.W, (int)b.X, y1, (int)b.W);

        // 中线虚线
        if (((startPos + i) % 8) < 4) {
            drawQuad(TFT_WHITE, (int)a.X, y0, 1, (int)b.X, y1, 1);
        }
    }
    
    // Draw Player Car
    // Simple car sprite
    int carX = (int)cx;
    int carY = scrH - 25;
    int carW = 40;
    int carH = 20;
    
    // Body
    _canvas->fillRect(carX - carW/2, carY, carW, carH, TFT_RED);
    // Roof
    _canvas->fillRect(carX - carW/4, carY - 10, carW/2, 10, TFT_RED);
    // Wheels
    _canvas->fillRect(carX - carW/2 - 2, carY + 10, 10, 10, TFT_BLACK);
    _canvas->fillRect(carX + carW/2 - 8, carY + 10, 10, 10, TFT_BLACK);
    // Lights
    _canvas->fillRect(carX - carW/2 + 2, carY + 2, 5, 5, TFT_YELLOW);
    _canvas->fillRect(carX + carW/2 - 7, carY + 2, 5, 5, TFT_YELLOW);
    
    // Speedometer
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setFont(&fonts::Font0);
    _canvas->setTextSize(1);
    _canvas->setCursor(5, 5);
    _canvas->printf("Speed: %d km/h", (int)(_speed / 20));
    _canvas->setCursor(5, 15);
    _canvas->printf("Gear: %d", _gear);
    _canvas->setCursor(5, 25);
    _canvas->printf("Lap: %d/%d", _laps < _total_laps ? _laps + 1 : _total_laps, _total_laps);

    // ---- Top-right minimap: closed circuit + current position ----
    {
        const int mw = 54;
        const int mh = 40;
        int mx = scrW - mw - 4;
        int my = 4;
        _canvas->fillRect(mx, my, mw, mh, 0x2104);
        _canvas->drawRect(mx, my, mw, mh, TFT_WHITE);

        int mcx = mx + mw / 2;
        int mcy = my + mh / 2 + 3;
        int mrx = mw / 2 - 9;
        int mry = mh / 2 - 9;

        // Circuit outline
        _canvas->drawEllipse(mcx, mcy, mrx, mry, 0x05E0);
        // Start/finish line marker at the top of the loop
        _canvas->drawFastHLine(mcx - 3, mcy - mry, 7, TFT_WHITE);

        // Current position dot
        const float kPi = 3.14159265f;
        int total_len = _seg_count * _segL;
        float t = (total_len > 0) ? (_playerZ / (float)total_len) : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t >= 1.0f) t = 0.999f;
        float ang = t * 2.0f * kPi - kPi / 2.0f;
        int px = mcx + (int)(cosf(ang) * mrx);
        int py = mcy + (int)(sinf(ang) * mry);
        _canvas->fillCircle(px, py, 2, TFT_RED);

        _canvas->setTextColor(TFT_WHITE);
        _canvas->setCursor(mx + 2, my + 1);
        _canvas->printf("MAP");
    }
    
    if (_game_over) {
        _canvas->setTextColor(TFT_RED);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString("YOU LOSE", cx, cy - 20);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->drawString("Press Enter to Restart", cx, cy);
        _canvas->setTextDatum(top_left); // Reset
    } else if (_finished) {
        _canvas->setTextColor(TFT_GREEN);
        _canvas->setTextDatum(middle_center);
        _canvas->drawString("FINISH", cx, cy - 20);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->drawString("Press Enter to Restart", cx, cy);
        _canvas->setTextDatum(top_left);
    }
    
    _canvas->pushSprite(0, 0);
    sendMirrorState();
}

void AppRace::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[22];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x2E; p[3] = 0;
    // playerX float → int16 (主机 _playerX 是 signed float, C3 侧按 int16 解析)
    {
        int px = (int)_playerX;
        if (px < -32768) px = -32768;
        if (px > 32767) px = 32767;
        int16_t px16 = (int16_t)px;
        p[4] = (uint8_t)(px16 & 0xFF); p[5] = (uint8_t)((px16 >> 8) & 0xFF);
    }
    // playerZ float → uint24 BE
    uint32_t pz = (uint32_t)_playerZ;
    p[6] = (pz >> 16) & 0xFF; p[7] = (pz >> 8) & 0xFF; p[8] = pz & 0xFF;
    // speed float → uint16
    int spd = (int)_speed;
    p[9] = spd & 0xFF; p[10] = (spd >> 8) & 0xFF;
    p[11] = (uint8_t)_gear;
    p[12] = (uint8_t)_laps;
    p[13] = _seg_count & 0xFF; p[14] = (_seg_count >> 8) & 0xFF;
    int cur_seg = (int)(_playerZ / _segL) % _seg_count;
    if (cur_seg < 0) cur_seg += _seg_count;
    p[15] = cur_seg & 0xFF; p[16] = (cur_seg >> 8) & 0xFF;
    float cv = _lines[cur_seg].curve;
    int16_t cv88 = (int16_t)(cv * 256);
    p[17] = cv88 & 0xFF; p[18] = (cv88 >> 8) & 0xFF;
    p[19] = (uint8_t)((_game_over ? 1 : 0) | (_finished ? 2 : 0));
    p[20] = 0;
    p[21] = 0;
    glink_send_state(p, sizeof(p));
}

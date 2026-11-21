#include "app_fps.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <apps/utils/common.h>
#include <math.h>
#include <esp_random.h>
#include <apps/utils/icons/default_icons.h>
#include <apps/utils/glink/glink.h>
#include <apps/utils/app_canvas.h>

// Map constants
#define MAP_WIDTH 24
#define MAP_HEIGHT 24

AppFPS::AppFPS() {
    setAppInfo().name = "FPS";
    setAppInfo().icon = nullptr;
    // setAppInfo().userData = new AppIcon_t(nullptr, icon_fps_small);

    _posX = 22.0; _posY = 12.0;
    _dirX = -1.0; _dirY = 0.0;
    _planeX = 0.0; _planeY = 0.66;
    
    // Add some dummy enemies
    _sprites.push_back({10.5, 10.5, 1}); // Enemy 1
    _sprites.push_back({15.0, 15.0, 1}); // Enemy 2
    _sprites.push_back({20.0, 10.0, 1}); // Enemy 3
}

AppFPS::~AppFPS() {
    // if (_canvas) {
    //     delete _canvas;
    // }
    // if (getAppInfo().userData) {
    //     delete (AppIcon_t*)getAppInfo().userData;
    // }
}

void AppFPS::onOpen() {
    mclog::tagInfo(getAppInfo().name, "Application opened");

    // Use Shared Canvas (resize to full screen for the 240x135 raycaster)
    _canvas = &GetHAL().canvas();
    appCanvasBegin(_canvas, getAppInfo().name);
    _canvas->fillScreen(TFT_BLACK);
    
    // Key handling
    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& keyEvent) {
        if (!keyEvent.isModifier) {
            // Restart
            if (_game_over && keyEvent.state && keyEvent.keyCode == KEY_ENTER) {
                resetGame();
                return;
            }

            // Movement state
            // Up: W, E, .
            if (keyEvent.keyCode == KEY_E || keyEvent.keyCode == KEY_W || keyEvent.keyCode == KEY_UP || strcmp(keyEvent.keyName, ".") == 0) _key_up = keyEvent.state;
            // Down: S, /
            else if (keyEvent.keyCode == KEY_S || keyEvent.keyCode == KEY_DOWN || strcmp(keyEvent.keyName, "/") == 0) _key_down = keyEvent.state;
            // Left: A, ,
            else if (keyEvent.keyCode == KEY_A || keyEvent.keyCode == KEY_LEFT || strcmp(keyEvent.keyName, ",") == 0) _key_left = keyEvent.state;
            // Right: D, ;
            else if (keyEvent.keyCode == KEY_D || keyEvent.keyCode == KEY_RIGHT || strcmp(keyEvent.keyName, ";") == 0) _key_right = keyEvent.state;
            
            // Fire (Enter or Space)
            if (keyEvent.state && (keyEvent.keyCode == KEY_ENTER || keyEvent.keyCode == KEY_SPACE)) {
                shoot();
            }
        }
    });

    resetGame();
    _last_frame_time = GetHAL().millis();
    renderFrame();
    glink_acquire("FPS", GLINK_GID_FPS, GLINK_ICON_FPS);
}

void AppFPS::resetGame() {
    _level = 1;
    _player_health = 100;
    _game_over = false;
    generateMap();
}

void AppFPS::generateMap() {
    // Fill with walls
    for(int x=0; x<MAP_WIDTH; x++) {
        for(int y=0; y<MAP_HEIGHT; y++) {
            if (x==0 || x==MAP_WIDTH-1 || y==0 || y==MAP_HEIGHT-1) _current_map[x][y] = 1; // Borders
            else if (esp_random()%10 < 2) _current_map[x][y] = esp_random()%4 + 1; // Random walls
            else _current_map[x][y] = 0; // Empty
        }
    }
    
    // Ensure player start is empty
    _posX = 22.0; _posY = 12.0;
    _current_map[(int)_posX][(int)_posY] = 0;
    _current_map[(int)_posX-1][(int)_posY] = 0;
    _current_map[(int)_posX][(int)_posY-1] = 0;
    
    // Enemies
    _sprites.clear();
    int enemyCount = 3 + _level; // More enemies per level
    for(int i=0; i<enemyCount; i++) {
        float ex, ey;
        int attempts = 0;
        do {
            ex = 1 + esp_random() % (MAP_WIDTH-2);
            ey = 1 + esp_random() % (MAP_HEIGHT-2);
            attempts++;
        } while((_current_map[(int)ex][(int)ey] != 0 || (abs(ex-_posX)<5 && abs(ey-_posY)<5)) && attempts < 200);
        
        _sprites.push_back({ex, ey, 1, 100, true, 0, GetHAL().millis()});
    }
}

void AppFPS::onRunning() {
    glink_poll();
    if (GetHAL().goButton().wasClicked()) {
        close();
        return;
    }

    updateGame();
    renderFrame();
}

void AppFPS::onClose() {
    glink_release();
    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }
    
    // 统一恢复系统画布（16bpp / 109 高）
    appCanvasRestore(getAppInfo().name);
    
    _canvas = nullptr;
}

void AppFPS::shoot() {
    if (_has_weapon && _weapon_frame == 0) {
        _weapon_frame = 5; // Start animation
        
        // Muzzle flash particles
        addParticles(180, 100, 10, TFT_YELLOW);
        
        // Check for hits
        for(auto& sprite : _sprites) {
            if (!sprite.alive) continue;

            double spriteX = sprite.x - _posX;
            double spriteY = sprite.y - _posY;

            double invDet = 1.0 / (_planeX * _dirY - _dirX * _planeY);
            double transformX = invDet * (_dirY * spriteX - _dirX * spriteY);
            double transformY = invDet * (-_planeY * spriteX + _planeX * spriteY);

            if (transformY > 0) {
                int spriteScreenX = int((240 / 2) * (1 + transformX / transformY));
                int spriteWidth = abs(int(_canvas->height() / (transformY)));
                
                // Center pixel is 120
                if (120 >= spriteScreenX - spriteWidth/2 && 120 <= spriteScreenX + spriteWidth/2) {
                    // HIT!
                    sprite.health -= 50; 
                    if (sprite.health <= 0) {
                        sprite.alive = false;
                        _show_kill_confirm = true;
                        _kill_confirm_timer = GetHAL().millis();
                        
                        // Blood particles?
                        addParticles(spriteScreenX, 67, 20, TFT_RED);
                    }
                    break; 
                }
            }
        }
    }
}

void AppFPS::updateGame() {
    if (_game_over) return;

    // Check level complete
    bool allDead = true;
    for(const auto& s : _sprites) if(s.alive) allDead = false;
    
    if (allDead) {
        if (!_generating_map) {
            _generating_map = true;
            _generate_timer = GetHAL().millis();
        } else if (GetHAL().millis() - _generate_timer > 2000) {
            _level++;
            _generating_map = false;
            generateMap();
        }
        return; // Pause updates while generating
    }

    uint32_t now = GetHAL().millis();
    double frameTime = (now - _last_frame_time) / 1000.0; // seconds
    _last_frame_time = now;
    
    if (_show_kill_confirm && now - _kill_confirm_timer > 1000) {
        _show_kill_confirm = false;
    }
    
    if (_weapon_frame > 0) _weapon_frame--;

    // Update enemies
    updateEnemies(frameTime);
    
    // Update particles
    updateParticles();

    // Speed modifiers
    double moveSpeed = frameTime * 5.0; 
    double rotSpeed = frameTime * 3.0; 

    // Gyro rotation
    GetHAL().imu.update();
    float gyroX, gyroY, gyroZ;
    GetHAL().imu.getGyroData(&gyroX, &gyroY, &gyroZ);
    double gyroRotSpeed = -gyroZ * frameTime * 0.05; 
    
    if (std::abs(gyroZ) > 10.0) { 
        double oldDirX = _dirX;
        _dirX = _dirX * cos(gyroRotSpeed) - _dirY * sin(gyroRotSpeed);
        _dirY = oldDirX * sin(gyroRotSpeed) + _dirY * cos(gyroRotSpeed);
        double oldPlaneX = _planeX;
        _planeX = _planeX * cos(gyroRotSpeed) - _planeY * sin(gyroRotSpeed);
        _planeY = oldPlaneX * sin(gyroRotSpeed) + _planeY * cos(gyroRotSpeed);
    }

    // Move forward
    if (_key_up) {
        if(_current_map[int(_posX + _dirX * moveSpeed)][int(_posY)] == 0) _posX += _dirX * moveSpeed;
        if(_current_map[int(_posX)][int(_posY + _dirY * moveSpeed)] == 0) _posY += _dirY * moveSpeed;
    }
    // Move backward
    if (_key_down) {
        if(_current_map[int(_posX - _dirX * moveSpeed)][int(_posY)] == 0) _posX -= _dirX * moveSpeed;
        if(_current_map[int(_posX)][int(_posY - _dirY * moveSpeed)] == 0) _posY -= _dirY * moveSpeed;
    }
    // Rotate right
    if (_key_right) {
        double oldDirX = _dirX;
        _dirX = _dirX * cos(-rotSpeed) - _dirY * sin(-rotSpeed);
        _dirY = oldDirX * sin(-rotSpeed) + _dirY * cos(-rotSpeed);
        double oldPlaneX = _planeX;
        _planeX = _planeX * cos(-rotSpeed) - _planeY * sin(-rotSpeed);
        _planeY = oldPlaneX * sin(-rotSpeed) + _planeY * cos(-rotSpeed);
    }
    // Rotate left
    if (_key_left) {
        double oldDirX = _dirX;
        _dirX = _dirX * cos(rotSpeed) - _dirY * sin(rotSpeed);
        _dirY = oldDirX * sin(rotSpeed) + _dirY * cos(rotSpeed);
        double oldPlaneX = _planeX;
        _planeX = _planeX * cos(rotSpeed) - _planeY * sin(rotSpeed);
        _planeY = oldPlaneX * sin(rotSpeed) + _planeY * cos(rotSpeed);
    }
    
    // Check if aiming at enemy
    _is_aiming_at_enemy = false;
    for(auto& sprite : _sprites) {
        if (!sprite.alive) continue;
        double spriteX = sprite.x - _posX;
        double spriteY = sprite.y - _posY;
        double invDet = 1.0 / (_planeX * _dirY - _dirX * _planeY);
        double transformX = invDet * (_dirY * spriteX - _dirX * spriteY);
        double transformY = invDet * (-_planeY * spriteX + _planeX * spriteY);
        if (transformY > 0) {
            int spriteScreenX = int((240 / 2) * (1 + transformX / transformY));
            int spriteWidth = abs(int(_canvas->height() / (transformY)));
            if (120 >= spriteScreenX - spriteWidth/2 && 120 <= spriteScreenX + spriteWidth/2) {
                _is_aiming_at_enemy = true;
                break;
            }
        }
    }
}

void AppFPS::updateEnemies(double frameTime) {
    for(auto& s : _sprites) {
        if (!s.alive) continue;
        
        // Simple AI: Move towards player
        double dx = _posX - s.x;
        double dy = _posY - s.y;
        double dist = sqrt(dx*dx + dy*dy);
        
        if (dist < 10.0 && dist > 1.0) { // Chase range
            s.x += (dx / dist) * frameTime * 2.0;
            s.y += (dy / dist) * frameTime * 2.0;
        }
        
        // Attack
        if (dist < 5.0) {
            if (GetHAL().millis() - s.last_shot > 1000) {
                s.last_shot = GetHAL().millis();
                _player_health -= 10;
                // Hit effect (red screen flash handled in render?)
                if (_player_health <= 0) _game_over = true;
            }
        }
    }
}

void AppFPS::addParticles(int x, int y, int count, uint16_t color) {
    for(int i=0; i<count; i++) {
        float vx = ((int)(esp_random()%10) - 5) / 2.0;
        float vy = ((int)(esp_random()%10) - 5) / 2.0;
        _particles.push_back({(float)x, (float)y, vx, vy, (int)(10 + esp_random()%10), color});
    }
}

void AppFPS::updateParticles() {
    for(auto it = _particles.begin(); it != _particles.end();) {
        it->x += it->vx;
        it->y += it->vy;
        it->life--;
        if(it->life <= 0) it = _particles.erase(it);
        else ++it;
    }
}

void AppFPS::drawWeapon() {
    // Smaller gun, bottom right
    int w = 40; // Smaller width
    int h = 50; // Smaller height
    int x = 240 - w - 20; // Right side
    int y = _canvas->height() - h;
    
    // Recoil
    if (_weapon_frame > 0) {
        y += 6;
        x -= 2;
    }
    
    // Draw gun body
    _canvas->fillRect(x, y, w, h, 0x333333); // Dark grey
    _canvas->fillRect(x + 5, y - 15, 10, 30, 0x222222); // Barrel (darker)
    
    // Detail
    _canvas->drawRect(x, y, w, h, TFT_BLACK);
    
    // Muzzle flash
    if (_weapon_frame > 3) {
        _canvas->fillCircle(x + 10, y - 25, 12, TFT_YELLOW);
        _canvas->fillCircle(x + 10, y - 25, 8, TFT_WHITE);
    }
}

void AppFPS::drawCrosshair() {
    // Center dot
    int cx = 120;
    int cy = _canvas->height() / 2;
    uint16_t color = TFT_RED;
    if (_is_aiming_at_enemy) {
        color = TFT_GREEN; // Invert or change color
    }
    _canvas->fillCircle(cx, cy, 2, color);
}

void AppFPS::renderSprites(double* ZBuffer) {
    // Sort sprites by distance
    std::vector<std::pair<double, int>> spriteOrder;
    for(int i=0; i<_sprites.size(); i++) {
        double dist = ((_posX - _sprites[i].x) * (_posX - _sprites[i].x) + (_posY - _sprites[i].y) * (_posY - _sprites[i].y));
        spriteOrder.push_back({dist, i});
    }
    std::sort(spriteOrder.rbegin(), spriteOrder.rend()); // Far to near

    for(int i=0; i<spriteOrder.size(); i++) {
        int id = spriteOrder[i].second;
        double spriteX = _sprites[id].x - _posX;
        double spriteY = _sprites[id].y - _posY;

        // Transform sprite with the inverse camera matrix
        double invDet = 1.0 / (_planeX * _dirY - _dirX * _planeY);
        double transformX = invDet * (_dirY * spriteX - _dirX * spriteY);
        double transformY = invDet * (-_planeY * spriteX + _planeX * spriteY); // this is actually the depth inside the screen

        int spriteScreenX = int((240 / 2) * (1 + transformX / transformY));

        // Calculate height of the sprite on screen
        int spriteHeight = abs(int(_canvas->height() / (transformY))); // using 'transformY' instead of real distance prevents fisheye
        
        // Calculate lowest and highest pixel to fill in current stripe
        int drawStartY = -spriteHeight / 2 + _canvas->height() / 2;
        if(drawStartY < 0) drawStartY = 0;
        int drawEndY = spriteHeight / 2 + _canvas->height() / 2;
        if(drawEndY >= _canvas->height()) drawEndY = _canvas->height() - 1;

        // Calculate width of the sprite
        int spriteWidth = abs(int(_canvas->height() / (transformY)));
        int drawStartX = -spriteWidth / 2 + spriteScreenX;
        if(drawStartX < 0) drawStartX = 0;
        int drawEndX = spriteWidth / 2 + spriteScreenX;
        if(drawEndX >= 240) drawEndX = 240 - 1;

        // Loop through every vertical stripe of the sprite on screen
        for(int stripe = drawStartX; stripe < drawEndX; stripe++) {
            // Check if sprite is visible
            if(transformY > 0 && stripe > 0 && stripe < 240 && transformY < ZBuffer[stripe]) {
                // Draw sprite vertical line
                // Simple color based on texture ID (1 = Enemy = Red-ish)
                uint16_t color = TFT_MAGENTA; // Placeholder
                if (_sprites[id].texture == 1) color = TFT_RED;

                _canvas->drawFastVLine(stripe, drawStartY, drawEndY - drawStartY, color);
                
                // Draw some details to look like a "person"
                // Eyes?
                if (spriteHeight > 10) {
                     int eyeY = drawStartY + spriteHeight / 4;
                     if (eyeY < drawEndY) {
                         _canvas->drawPixel(stripe, eyeY, TFT_BLACK);
                     }
                }
            }
        }
    }
}

void AppFPS::renderFrame() {
    if (!_canvas) return;

    // Draw Ceiling and Floor
    const int horizon = _canvas->height() / 2;
    _canvas->fillRect(0, 0, 240, horizon, 0x3186); // Dark Grey/Blueish
    _canvas->fillRect(0, horizon, 240, _canvas->height() - horizon, 0x738E); // Brownish

    for(int x = 0; x < 240; x++) {
        double cameraX = 2 * x / (double)240 - 1; 
        double rayDirX = _dirX + _planeX * cameraX;
        double rayDirY = _dirY + _planeY * cameraX;

        int mapX = int(_posX);
        int mapY = int(_posY);

        double sideDistX;
        double sideDistY;

        double deltaDistX = (rayDirX == 0) ? 1e30 : std::abs(1 / rayDirX);
        double deltaDistY = (rayDirY == 0) ? 1e30 : std::abs(1 / rayDirY);
        double perpWallDist;

        int stepX;
        int stepY;

        int hit = 0; 
        int side = 0;

        if (rayDirX < 0) {
            stepX = -1;
            sideDistX = (_posX - mapX) * deltaDistX;
        } else {
            stepX = 1;
            sideDistX = (mapX + 1.0 - _posX) * deltaDistX;
        }
        if (rayDirY < 0) {
            stepY = -1;
            sideDistY = (_posY - mapY) * deltaDistY;
        } else {
            stepY = 1;
            sideDistY = (mapY + 1.0 - _posY) * deltaDistY;
        }

        // DDA
        while (hit == 0) {
            if (sideDistX < sideDistY) {
                sideDistX += deltaDistX;
                mapX += stepX;
                side = 0;
            } else {
                sideDistY += deltaDistY;
                mapY += stepY;
                side = 1;
            }
            if (_current_map[mapX][mapY] > 0) hit = 1;
        }

        if (side == 0) perpWallDist = (sideDistX - deltaDistX);
        else           perpWallDist = (sideDistY - deltaDistY);
        
        // ZBuffer for sprites
        _zBuffer[x] = perpWallDist;

        int lineHeight = (int)(_canvas->height() / perpWallDist);

        int drawStart = -lineHeight / 2 + _canvas->height() / 2;
        if(drawStart < 0) drawStart = 0;
        int drawEnd = lineHeight / 2 + _canvas->height() / 2;
        if(drawEnd >= _canvas->height()) drawEnd = _canvas->height() - 1;

        // Choose wall color
        uint16_t color;
        switch(_current_map[mapX][mapY]) {
            case 1:  color = TFT_RED;  break; 
            case 2:  color = TFT_GREEN;  break;
            case 3:  color = TFT_BLUE;   break;
            case 4:  color = TFT_WHITE;  break;
            default: color = TFT_YELLOW; break;
        }

        if (side == 1) color = (color >> 1) & 0x7BEF; // Darken

        _canvas->drawFastVLine(x, drawStart, drawEnd - drawStart, color);
    }
    
    // Draw Sprites (Enemies)
    renderSprites(_zBuffer);
    
    // Draw Particles
    for(const auto& p : _particles) {
        _canvas->drawPixel((int)p.x, (int)p.y, p.color);
    }

    // Draw Weapon
    drawWeapon();
    
    // Draw Crosshair
    drawCrosshair();
    
    // HUD
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setTextSize(1);
    _canvas->setCursor(2, 2);
    _canvas->printf("FPS | Lvl: %d", _level);
    _canvas->setCursor(2, _canvas->height() - 10);
    _canvas->printf("HP: %d", _player_health);
    
    int enemies_left = 0;
    for(const auto& s : _sprites) if(s.alive) enemies_left++;
    _canvas->setCursor(180, _canvas->height() - 10);
    _canvas->printf("Enemies: %d", enemies_left);
    
    // Kill Confirm
    if (_show_kill_confirm) {
        _canvas->setTextColor(TFT_RED);
        _canvas->setTextSize(2);
        _canvas->drawCenterString("ENEMY DOWN", 120, 40, &fonts::efontCN_10);
    }
    
    // Generating Map Popup
    if (_generating_map) {
        _canvas->fillRect(40, 50, 160, 40, TFT_BLUE);
        _canvas->drawRect(40, 50, 160, 40, TFT_WHITE);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setTextSize(1);
        _canvas->drawCenterString("Rendering next map...", 120, 60, &fonts::efontCN_10);
        _canvas->drawCenterString("Please wait", 120, 75, &fonts::efontCN_10);
    }
    
    if (_game_over) {
        _canvas->fillRect(0, 0, 240, _canvas->height(), TFT_BLACK); // Full screen game over? Or overlay?
        _canvas->setTextColor(TFT_RED);
        _canvas->setTextSize(2);
        _canvas->drawCenterString("YOU DIED", 120, 50, &fonts::efontCN_10);
        _canvas->setTextColor(TFT_WHITE);
        _canvas->setTextSize(1);
        _canvas->drawCenterString("Press Enter to Restart", 120, 80, &fonts::efontCN_10);
    }
    
    appCanvasPush(_canvas);
    sendMirrorState();
}

void AppFPS::sendMirrorState() {
    if (!glink_is_linked()) return;
    uint8_t p[121];
    memset(p, 0, sizeof(p));
    p[0] = 'G'; p[1] = 'M'; p[2] = 0x2F; p[3] = 0;
    float f1 = (float)_posX, f2 = (float)_posY;
    memcpy(&p[4], &f1, 4); memcpy(&p[8], &f2, 4);
    f1 = (float)_dirX; f2 = (float)_dirY;
    memcpy(&p[12], &f1, 4); memcpy(&p[16], &f2, 4);
    f1 = (float)_planeX; f2 = (float)_planeY;
    memcpy(&p[20], &f1, 4); memcpy(&p[24], &f2, 4);
    p[28] = (uint8_t)_player_health;
    p[29] = (uint8_t)_weapon_frame;
    p[30] = (uint8_t)_level;
    p[31] = (uint8_t)(_game_over ? 1 : 0);
    int cnt = (int)_sprites.size();
    if (cnt > 8) cnt = 8;
    p[32] = (uint8_t)cnt;
    for (int i = 0; i < cnt; i++) {
        int off = 33 + i * 11;
        float ex = (float)_sprites[i].x;
        float ey = (float)_sprites[i].y;
        memcpy(&p[off], &ex, 4);
        memcpy(&p[off + 4], &ey, 4);
        p[off + 8] = (uint8_t)_sprites[i].health;
        p[off + 9] = (uint8_t)(_sprites[i].alive ? 1 : 0);
        p[off + 10] = (uint8_t)_sprites[i].state;
    }
    glink_send_state(p, sizeof(p));
}

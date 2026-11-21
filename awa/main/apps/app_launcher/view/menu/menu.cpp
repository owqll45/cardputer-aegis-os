/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "menu.h"
#include "../../app_launcher.h"
#include <apps/utils/theme.h>
#include <apps/utils/common.h>
#include <apps/utils/icons/generated_icons.h>
#include <apps/utils/icons/generated_icons_extra.h>
#include <apps/utils/icons/icon_settings_generated.h>
#include <apps/utils/icons/test_folder_icon.h>
#include <apps/utils/ui_theme.h>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/app_registry.h>
#include <hal/hal.h>
#include <hal/utils/perf.h>
#include <string>
#include <math.h>
#include <map>
#include <algorithm>
#include "crash_handler.h"

using namespace mooncake;
using namespace smooth_ui_toolkit;

static const std::string _tag = "LauncherMenu";

// Static AppIcon wrappers for generated icons
static AppIcon_t icon_snake(icon_snake_generated, icon_snake_generated);
static AppIcon_t icon_tetris(icon_tetris_generated, icon_tetris_generated);
static AppIcon_t icon_breakout(icon_breakout_generated, icon_breakout_generated);
static AppIcon_t icon_water(icon_water_generated, icon_water_generated);
static AppIcon_t icon_minesweeper(icon_minesweeper_generated, icon_minesweeper_generated);
static AppIcon_t icon_flappy(icon_flappy_generated, icon_flappy_generated);
static AppIcon_t icon_invaders(icon_invaders_generated, icon_invaders_generated);
static AppIcon_t icon_pong(icon_pong_generated, icon_pong_generated);
static AppIcon_t icon_gomoku(icon_gomoku_generated, icon_gomoku_generated);
static AppIcon_t icon_reversi(icon_reversi_generated, icon_reversi_generated);
static AppIcon_t icon_tank(icon_tank_generated, icon_tank_generated);
static AppIcon_t icon_uno(icon_uno_generated, icon_uno_generated);
static AppIcon_t icon_connect4(icon_connect4_generated, icon_connect4_generated);
static AppIcon_t icon_checkers(icon_checkers_generated, icon_checkers_generated);
static AppIcon_t icon_calc(icon_calc_generated, icon_calc_generated);
static AppIcon_t icon_funccalc(icon_funccalc_generated, icon_funccalc_generated);
static AppIcon_t icon_folder_games(icon_folder_games_generated, icon_folder_games_generated);
static AppIcon_t icon_folder_tools(icon_folder_tools_generated, icon_folder_tools_generated);
static AppIcon_t icon_folder_system(icon_folder_system_generated, icon_folder_system_generated);
static AppIcon_t icon_generic(icon_generic_generated, icon_generic_generated);

// New Icons
static AppIcon_t icon_dino(icon_dino_generated, icon_dino_generated);
static AppIcon_t icon_cube3d(icon_cube3d_generated, icon_cube3d_generated);
static AppIcon_t icon_dice(icon_dice_generated, icon_dice_generated);
static AppIcon_t icon_timer(icon_timer_generated, icon_timer_generated);
static AppIcon_t icon_stopwatch(icon_stopwatch_generated, icon_stopwatch_generated);
static AppIcon_t icon_flashlight(icon_flashlight_generated, icon_flashlight_generated);
static AppIcon_t icon_sysinfo(icon_sysinfo_generated, icon_sysinfo_generated);
static AppIcon_t icon_matrix(icon_matrix_generated, icon_matrix_generated);
static AppIcon_t icon_drum(icon_drum_generated, icon_drum_generated);
static AppIcon_t icon_piano(icon_piano_generated, icon_piano_generated);
static AppIcon_t icon_sketch(icon_sketch_generated, icon_sketch_generated);
static AppIcon_t icon_ruler(icon_ruler_generated, icon_ruler_generated);
static AppIcon_t icon_coin(icon_coin_generated, icon_coin_generated);
static AppIcon_t icon_breathing(icon_breathing_generated, icon_breathing_generated);
static AppIcon_t icon_musicbox(icon_musicbox_generated, icon_musicbox_generated);

// 本工程补充的图标（之前会退回灰色通用图标）
static AppIcon_t icon_shogi(icon_shogi_generated, icon_shogi_generated);
static AppIcon_t icon_fps(icon_fps_generated, icon_fps_generated);
static AppIcon_t icon_2048(icon_2048_generated, icon_2048_generated);
static AppIcon_t icon_racing(icon_racing_generated, icon_racing_generated);
static AppIcon_t icon_stringir(icon_stringir_generated, icon_stringir_generated);
static AppIcon_t icon_distance(icon_distance_generated, icon_distance_generated);
static AppIcon_t icon_temphumi(icon_temphumi_generated, icon_temphumi_generated);
static AppIcon_t icon_multimeter(icon_multimeter_generated, icon_multimeter_generated);
static AppIcon_t icon_relay(icon_relay_generated, icon_relay_generated);
static AppIcon_t icon_clock(icon_clock_generated, icon_clock_generated);
static AppIcon_t icon_imu(icon_imu_generated, icon_imu_generated);
static AppIcon_t icon_network(icon_network_generated, icon_network_generated);
static AppIcon_t icon_zdlink(icon_zdlink_generated, icon_zdlink_generated);
static AppIcon_t icon_settings(icon_settings_generated, icon_settings_generated);

// Folder Icon (Use generic for now or create one dynamically)
// We can use icon_generic for folders

static void* get_icon_for_app(const std::string& name, void* defaultIcon) {
    if (defaultIcon != nullptr) return defaultIcon;
    
    if (name == "Snake") return &icon_snake;
    if (name == "Tetris") return &icon_tetris;
    if (name == "Breakout") return &icon_breakout;
    if (name == "Water Level") return &icon_water;
    if (name == "Minesweeper") return &icon_minesweeper;
    if (name == "Flappy") return &icon_flappy;
    if (name == "Invaders") return &icon_invaders;
    if (name == "Pong") return &icon_pong;
    if (name == "Gomoku") return &icon_gomoku;
    if (name == "Reversi") return &icon_reversi;
    if (name == "Tank") return &icon_tank;
    if (name == "UNO") return &icon_uno;
    if (name == "Connect4") return &icon_connect4;
    if (name == "Checkers") return &icon_checkers;
    if (name == "SciCalc") return &icon_calc;
    if (name == "FuncCalc") return &icon_funccalc;
    
    // New Apps
    if (name == "Dino") return &icon_dino;
    if (name == "Cube3D") return &icon_cube3d;
    if (name == "Dice") return &icon_dice;
    if (name == "Timer") return &icon_timer;
    if (name == "Stopwatch") return &icon_stopwatch;
    if (name == "Flashlight") return &icon_flashlight;
    if (name == "SysInfo") return &icon_sysinfo;
    if (name == "Matrix") return &icon_matrix;
    if (name == "Drum") return &icon_drum;
    if (name == "Piano") return &icon_piano;
    if (name == "Sketch") return &icon_sketch;
    if (name == "Ruler") return &icon_ruler;
    if (name == "Coin") return &icon_coin;
    if (name == "Breathing") return &icon_breathing;
    if (name == "MusicBox") return &icon_musicbox;
    
    // 本工程补充的图标（const 放 flash，不占 RAM）
    if (name == "Shogi") return &icon_shogi;
    if (name == "FPS") return &icon_fps;
    if (name == "2048") return &icon_2048;
    if (name == "Racing") return &icon_racing;
    if (name == "StringIR") return &icon_stringir;
    if (name == "Distance") return &icon_distance;
    if (name == "Temp/Humi+") return &icon_temphumi;
    if (name == "Multimeter") return &icon_multimeter;
    if (name == "RelayTester") return &icon_relay;
    if (name == "Clock") return &icon_clock;
    if (name == "IMU") return &icon_imu;
    if (name == "Network") return &icon_network;
    if (name == "天链" || name == "LoRaChat") return &icon_zdlink;
    if (name == "设置" || name == "Settings") return &icon_settings;
    if (name == "Whack") return &icon_minesweeper;
    if (name == "Blackjack") return &icon_dice;
    if (name == "Maze") return &icon_matrix;

    return &icon_generic;
}

void LauncherMenu::init(int launcherAppId)
{
    mclog::tagInfo(_tag, "init");

    // Initialize Folders (Localized)
    _folders.clear();
    // 240 宽屏下标签用短名: 带 "(Games)" 会和相邻标签撞在一起
    _folders.push_back({"游戏", {}});
    _folders.push_back({"工具", {}});
    _folders.push_back({"系统", {}});
    
    // Define Categories
    std::vector<std::string> cat_games = {
        "Snake", "Tetris", "Breakout", "Pong", "Flappy", "Invaders", "Minesweeper", 
        "Dino", "Cube3D", "Dice", "Coin", "Matrix", "2048", "Gomoku", "Shogi", "FPS",
        "Reversi", "Tank", "UNO", "Connect4", "Checkers", "Whack", "Blackjack", "Maze",
        "Doudizhu", "Racing"
    };
    std::vector<std::string> cat_tools = {
        "Timer", "Stopwatch", "Flashlight", "Ruler", "Sketch", "Piano", "Drum", 
        "Breathing", "MusicBox", "SciCalc", "FuncCalc", "StringIR", "LoRaChat", "GPS", "Distance", "Temp/Humi+",
        "Multimeter", "RelayTester", "Water Level", "MicMeter", "IR Remote"
    };
    std::vector<std::string> cat_system = {
        "SysInfo", "Network", "Clock", "Keyboard", "IMU", "设置", "Settings"
    };

    // Classify Apps (from registry - lazy loading, no instantiation)
    auto& registry = GetAppRegistry();
    _root_apps.clear();
    _pinned_app = -1;
    for (size_t idx = 0; idx < registry.size(); idx++) {
        std::string name = registry[idx].name;
        int id = (int)idx;  // Registry index as menu ID

        // 系统设置归入「系统」文件夹, 不再置顶占桌面根目录
        // 天链终端：直接放在主页最前面, 不用进文件夹翻
        if (name == "天链") {
            _root_apps.insert(_root_apps.begin(), id);
            continue;
        }

        bool found = false;
        
        // Check Games
        for(auto& n : cat_games) if(n == name) {
            _folders[0].appIds.push_back(id);
            found = true; break;
        }
        if(found) continue;

        // Check Tools
        for(auto& n : cat_tools) if(n == name) {
            _folders[1].appIds.push_back(id);
            found = true; break;
        }
        if(found) continue;

        // Check System
        for(auto& n : cat_system) if(n == name) {
            _folders[2].appIds.push_back(id);
            found = true; break;
        }
        if(found) continue;
        
        // Others -> Root
        _root_apps.push_back(id);
    }
    
    // Initial State: Root
    _in_folder = false;
    _current_folder_index = -1;
    updateMenuItems();

    // Setup animation
    getSelectorPostion().x.springOptions().visualDuration = 0.4;
    getSelectorShape().x.springOptions().visualDuration   = 0.4;
    getSelectorShape().y.springOptions().visualDuration   = 0.4;

    // Default app number
    moveTo(0);

    // Read input in every update
    setConfig().readInputInterval = 0;
    
    initStars();
}

void LauncherMenu::updateMenuItems() {
    _data.option_list.clear();
    _current_menu_items.clear();
    
    int i = 0;
    
    // If in folder, add Back button
    if (_in_folder) {
        MenuItem item;
        item.name = "返回 (BACK)";
        item.isFolder = false;
        item.id = -999; // Special ID for Back
        _current_menu_items.push_back(item);
        
        addOption({Vector4i{ICON_GAP + i * (ICON_WIDTH + ICON_GAP), ICON_MARGIN_TOP, ICON_WIDTH, ICON_WIDTH}, nullptr});
        i++;
        
        // Add apps in folder
        if (_current_folder_index >= 0 && _current_folder_index < _folders.size()) {
            for (int regIdx : _folders[_current_folder_index].appIds) {
                if (regIdx >= 0 && regIdx < (int)GetAppRegistry().size()) {
                    MenuItem item;
                    item.name = GetAppRegistry()[regIdx].name;
                    item.isFolder = false;
                    item.id = regIdx;
                    _current_menu_items.push_back(item);
                    
                    void* icon = get_icon_for_app(item.name, nullptr);
                    addOption({Vector4i{ICON_GAP + i * (ICON_WIDTH + ICON_GAP), ICON_MARGIN_TOP, ICON_WIDTH, ICON_WIDTH}, icon});
                    i++;
                }
            }
        }
    } else {
        // 天链: 第一屏就能看到, 不用往右翻
        size_t root_from = 0;
        if (!_root_apps.empty()) {
            const int regIdx = _root_apps.front();
            if (regIdx >= 0 && regIdx < (int)GetAppRegistry().size() &&
                GetAppRegistry()[regIdx].name == "天链") {
                MenuItem item;
                item.name = GetAppRegistry()[regIdx].name;
                item.isFolder = false;
                item.id = regIdx;
                _current_menu_items.push_back(item);
                addOption({Vector4i{ICON_GAP + i * (ICON_WIDTH + ICON_GAP), ICON_MARGIN_TOP, ICON_WIDTH, ICON_WIDTH},
                           get_icon_for_app(item.name, nullptr)});
                i++;
                root_from = 1;
            }
        }

        // Then Folders
        for (int f=0; f<_folders.size(); f++) {
            if (_folders[f].appIds.empty()) continue; // Skip empty folders
            
            MenuItem item;
            item.name = _folders[f].name;
            item.isFolder = true;
            item.id = f; // Folder index
            _current_menu_items.push_back(item);
            
            // 每个文件夹用自己的图标
            void* ficon = &icon_folder_system;
            if (f == 0) ficon = &icon_folder_games;
            else if (f == 1) ficon = &icon_folder_tools;
            else if (f == 2) ficon = &icon_folder_system;
            addOption({Vector4i{ICON_GAP + i * (ICON_WIDTH + ICON_GAP), ICON_MARGIN_TOP, ICON_WIDTH, ICON_WIDTH}, ficon});
            i++;
        }
        
        // Then Root Apps
        for (size_t r = root_from; r < _root_apps.size(); r++) {
            int regIdx = _root_apps[r];
            if (regIdx >= 0 && regIdx < (int)GetAppRegistry().size()) {
                MenuItem item;
                item.name = GetAppRegistry()[regIdx].name;
                item.isFolder = false;
                item.id = regIdx;
                _current_menu_items.push_back(item);
                
                void* icon = get_icon_for_app(item.name, nullptr);
                addOption({Vector4i{ICON_GAP + i * (ICON_WIDTH + ICON_GAP), ICON_MARGIN_TOP, ICON_WIDTH, ICON_WIDTH}, icon});
                i++;
            }
        }
    }
}

void LauncherMenu::initStars() {
    _stars.clear();
    for (int i = 0; i < 30; i++) {
        Star s;
        s.x = rand() % 240;
        s.y = rand() % 135;
        s.speed = (rand() % 10 + 2) / 10.0f;
        s.brightness = (rand() % 100) / 100.0f;
        _stars.push_back(s);
    }
}

void LauncherMenu::renderBackground() {
    // 1. Deep Space Gradient (Banded for performance)
    // Top is black, bottom is dark purple
    for (int y = 0; y < 135; y += 5) {
        uint8_t r = 20 + (y * 40 / 135);
        uint8_t g = 0 + (y * 10 / 135);
        uint8_t b = 40 + (y * 80 / 135);
        uint16_t color = GetHAL().canvas().color565(r, g, b);
        GetHAL().canvas().fillRect(0, y, 240, 5, color);
    }
    
    // 2. Stars
    for (auto& s : _stars) {
        // Move star
        s.x -= s.speed;
        if (s.x < 0) s.x = 240;
        
        // Twinkle
        if (rand() % 20 == 0) s.brightness = (rand() % 100) / 100.0f;
        
        uint8_t c = (uint8_t)(s.brightness * 255);
        GetHAL().canvas().drawPixel((int)s.x, (int)s.y, GetHAL().canvas().color565(c, c, c));
    }
    
    // 3. Grid Floor (Cyberpunk style)
    // Perspective lines
    int horizon_y = 100;
    GetHAL().canvas().drawFastHLine(0, horizon_y, 240, TFT_MAGENTA);
    
    // Vertical lines spreading out
    // Vanishing point approx (120, 60)
    for (int x = -200; x < 440; x += 40) {
        GetHAL().canvas().drawLine(120, 60, x, 135, 0x6010); // Dark Magenta
    }
    
    // Moving horizontal lines
    _anim_time += 0.5f;
    if (_anim_time > 20) _anim_time = 0;
    
    for (int y = horizon_y; y < 135; y += (10 + (y-horizon_y)/2)) {
        int draw_y = y + (int)_anim_time % 10;
        if (draw_y < 135)
            GetHAL().canvas().drawFastHLine(0, draw_y, 240, 0x6010);
    }
}

void LauncherMenu::onReadInput()
{
    auto event = GetHAL().keyboard.getLatestKeyEventRaw();
    
    // Check release first (Debounce / Wait logic)
    if (_waiting_for_release) {
        if (!event.state) { // Key is NOT pressed
            // Require 50ms of continuous release to confirm
            if (_release_check_time == 0) {
                _release_check_time = GetHAL().millis();
            } else if (GetHAL().millis() - _release_check_time > 50) {
                // Confirmed released
                _waiting_for_release = false;
                _release_check_time = 0;
                _last_input_time = GetHAL().millis(); // Reset debounce timer
                _last_key_state = false; // Ensure state is synced
            }
        } else {
            // Key is pressed, reset timer
            _release_check_time = 0;
        }
        return; // Ignore all input while waiting for release
    }

    // Debounce Check (Global)
    if (GetHAL().millis() - _last_input_time < 80) { // 80ms: 按键响应优先, 硬件本身已消抖
        return;
    }

    // Edge Detection Logic
    // Only trigger actions on state CHANGES
    
    // Key pressed (Rising Edge)
    if (event.state == true && !_last_key_state) {
        _last_key_state = true;
        _last_input_time = GetHAL().millis();
        _key_event_ms = GetHAL().keyboard.lastPressMs();
        _action_ms = perf_ms();
        _latency_pending = true;

        // Left
        if (event.row == 3 && event.col == 10) {
            goLast();
        }
        // Right
        else if (event.row == 3 && event.col == 12) {
            goNext();
        }
        // Up
        else if (event.row == 2 && event.col == 11) {
            goLast();
        }
        // Down
        else if (event.row == 3 && event.col == 11) {
            goNext();
        }
        // Enter
        else if (event.row == 2 && event.col == 13) {
            press(getSelectedKeyframe());
        }
        // Mute (Key M: Row 3, Col 9)
        // M key for Mute
        auto key = GetHAL().keyboard.getLatestKeyEvent(); // Use standard event for special keys
        if (key.state && key.keyCode == KEY_M) {
             if (onMuteToggle) onMuteToggle();
        }
    }
    // Key released (Falling Edge)
    else if (event.state == false && _last_key_state) {
        _last_key_state = false;
        
        // Enter Release
        // We only care about Enter release if we were tracking an Enter press
        // Since we don't track WHICH key was pressed in _last_key_state (simplification),
        // we assume the release corresponds to the press.
        // For robustness, we check if the released key IS Enter.
        // Note: event.row/col might be valid even on release in some HALs, but let's assume it is.
        if (event.row == 2 && event.col == 13) {
            release();
        }
    }
    // Else: Holding state (Do nothing)
}

void LauncherMenu::onClick()
{
    int index = getSelectedOptionIndex();
    if (index < 0 || index >= _current_menu_items.size()) return;
    
    const auto& item = _current_menu_items[index];
    
    if (item.isFolder) {
        // Enter Folder
        _in_folder = true;
        _current_folder_index = item.id;
        updateMenuItems();
        moveTo(0);
        
        // Prevent double-click / flickering
        _waiting_for_release = true;
        _release_check_time = 0;
    } 
    else if (item.id == -999) {
        // Back
        _in_folder = false;
        _current_folder_index = -1;
        updateMenuItems();
        moveTo(0);
        
        // Prevent double-click / flickering
        _waiting_for_release = true;
        _release_check_time = 0;
    }
    else {
        // Open App
        if (onAppOpen) {
            onAppOpen(index, item.id);
        }
    }
}

void LauncherMenu::onRender()
{
    auto selector_kf = getSelectorCurrentFrame();

    // Clear & Background
    renderBackground();

    // X offset (keep selector ar the center)
    int x_offset = -(selector_kf.x) + GetHAL().canvas().width() / 2 - ICON_WIDTH / 2;

    // Font
    GetHAL().canvas().setFont(FONT_BASIC);
    GetHAL().canvas().setTextSize(1);
    
    // Levitation effect for selected
    float float_y = sin(GetHAL().millis() / 200.0f) * 3.0f;

    // Redner options
    int index = 0;
    for (auto& i : getOptionList()) {
        // Safety check
        if (index >= _current_menu_items.size()) break;
        std::string name = _current_menu_items[index].name;
        
        if (index == getSelectedOptionIndex()) {
            
            int center_x = i.keyframe.x + i.keyframe.width / 2 + x_offset;
            int center_y = i.keyframe.y + i.keyframe.height / 2 + (int)float_y;
            
            // Glow effect (Concentric rounded rects) - 使用系统设置里的配色
            uint16_t ac = uiAccentColor();
            uint8_t ar = (uint8_t)(((ac >> 11) & 0x1F) * 255 / 31);
            uint8_t ag = (uint8_t)(((ac >> 5) & 0x3F) * 255 / 63);
            uint8_t ab = (uint8_t)((ac & 0x1F) * 255 / 31);
            for (int r = 0; r < 5; r++) {
                uint16_t glow_color = GetHAL().canvas().color565(
                    ar * (5 - r) / 5, ag * (5 - r) / 5, ab * (5 - r) / 5);
                GetHAL().canvas().drawRoundRect(
                    center_x - ICON_SELECTED_WIDTH/2 - r,
                    center_y - ICON_SELECTED_WIDTH/2 - r,
                    ICON_SELECTED_WIDTH + r*2,
                    ICON_SELECTED_WIDTH + r*2,
                    10 + r,
                    glow_color
                );
            }
            
            // Shadow
            GetHAL().canvas().fillEllipse(center_x, center_y + 40, 20 + (int)float_y, 4, TFT_BLACK);

            // Icon bg
            GetHAL().canvas().fillSmoothRoundRect(
                center_x - ICON_SELECTED_WIDTH/2,
                center_y - ICON_SELECTED_WIDTH/2,
                ICON_SELECTED_WIDTH, ICON_SELECTED_WIDTH, 8, TFT_WHITE); // White highlight

            // Icon
            if (i.userData != nullptr) {
                AppIcon_t* icon = (AppIcon_t*)(i.userData);
                bool is_generated = (icon->iconBig == icon->iconSmall);
                
                if (is_generated) {
                    // Draw 40x40 centered
                    GetHAL().canvas().pushImage(
                        center_x - 20, // 40/2
                        center_y - 20, 
                        40, 40,
                        icon->iconBig);
                } else {
                    // Legacy behavior (56x56)
                    GetHAL().canvas().pushImage(
                        center_x - ICON_SELECTED_WIDTH/2 + 4,
                        center_y - ICON_SELECTED_WIDTH/2 + 4, 
                        56, 56,
                        icon->iconBig);
                }
            }

            // Font - Use Chinese Font
            GetHAL().canvas().setFont(&fonts::efontCN_14);
            GetHAL().canvas().setTextSize(1);
            
            // Draw name (Pulsing color)
            GetHAL().canvas().setTextColor(TFT_YELLOW); // Transparent background
                
            GetHAL().canvas().drawCenterString(
                name.c_str(), 
                center_x,
                center_y + ICON_SELECTED_WIDTH/2 + ICON_TAG_MARGIN_TOP);
                
        } else {
            // Unselected items - Darker, Smaller
            int item_x = i.keyframe.x + x_offset;
            int item_y = i.keyframe.y;
            
            // Icon bg (Darker)
            GetHAL().canvas().fillSmoothRoundRect(item_x, item_y, i.keyframe.width,
                                                i.keyframe.height, 8, 0x3186); // Dark Grey/Blue

            // Icon
            if (i.userData != nullptr) {
                AppIcon_t* icon = (AppIcon_t*)(i.userData);
                if (icon->iconSmall != nullptr) {
                    GetHAL().canvas().pushImage(item_x + 4, item_y + 4, 40, 40,
                                              icon->iconSmall);
                }
            }

            // Draw tag
            GetHAL().canvas().setFont(&fonts::efontCN_12); // Smaller Chinese font
            GetHAL().canvas().setTextColor(TFT_LIGHTGREY); // Transparent bg
            GetHAL().canvas().drawCenterString(name.c_str(),
                                             item_x + i.keyframe.width / 2,
                                             item_y + i.keyframe.height + ICON_TAG_MARGIN_TOP);
        }

        index++;
    }

    if (pushCanvas) {
        GetHAL().pushCanvas();
    }

    // 按键延迟: poll = 硬件按下→菜单处理, frame = 处理→本帧推屏完成
    if (_latency_pending) {
        _latency_pending = false;
        uint32_t now = perf_ms();
        ESP_LOGI("PERF", "key poll=%ums frame=%ums total=%ums",
                 (unsigned)(_action_ms - _key_event_ms),
                 (unsigned)(now - _action_ms),
                 (unsigned)(now - _key_event_ms));
    }
}

static LauncherMenu* _launcher_menu = nullptr;

void Launcher::start_menu()
{
    _launcher_menu = new LauncherMenu();
    _launcher_menu->init(getId());
    _launcher_menu->onAppOpen = [this](int index, int appId) { handle_app_open(index, appId); };
    _launcher_menu->onMuteToggle = [this]() {
        _data.system_state.is_muted = !_data.system_state.is_muted;
        // Apply mute to speaker
        if (_data.system_state.is_muted) GetHAL().speaker.setVolume(0);
        else GetHAL().speaker.setVolume(255); // Restore volume
        
        GetHAL().speaker.tone(1000, 100);
    };
}

void Launcher::update_menu(bool pushCanvas)
{
    _launcher_menu->pushCanvas = pushCanvas;
    _launcher_menu->update();
}

void Launcher::reset_menu_input()
{
    if (_launcher_menu) {
        _launcher_menu->waitInputRelease();
    }
}

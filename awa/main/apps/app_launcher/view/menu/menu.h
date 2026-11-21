/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <smooth_ui_toolkit.h>
#include <apps/utils/common.h>
#include <functional>

class LauncherMenu : public smooth_ui_toolkit::SmoothSelectorMenu {
public:
    struct OptionInfo_t {
        int appId = -1;
        std::string name;
    };

    std::function<void(int index, int appId)> onAppOpen;
    std::function<void()> onMuteToggle;
    bool pushCanvas = true;

    // Folder Support
    struct MenuItem {
        bool isFolder = false;
        int id = -1; // AppID or Folder ID (if needed)
        std::string name;
        std::vector<int> childrenIDs; // List of AppIDs in this folder
    };
    
    // We need to override OptionInfo_t usage, but SmoothSelectorMenu uses its own list.
    // We will map index to our MenuItem list.
    std::vector<MenuItem> _current_menu_items;
    
    // Store the full structure
    struct Folder {
        std::string name;
        std::vector<int> appIds;
    };
    std::vector<Folder> _folders;
    std::vector<int> _root_apps; // Apps in root
    int _pinned_app = -1;        // 置顶显示的 app（系统设置），排在桌面最左
    
    bool _in_folder = false;
    int _current_folder_index = -1;
    bool _waiting_for_release = true;

    void init(int launcherAppId);
    void updateMenuItems(); // Refresh selector options from _current_menu_items
    void waitInputRelease() { _waiting_for_release = true; _release_check_time = 0; }
    void onReadInput() override;
    void onRender() override;
    void onClick() override;

    // Theme Support
    enum Theme {
        THEME_LIST = 0,
        THEME_GRID = 1
    };
    Theme _current_theme = THEME_GRID;
    void setTheme(Theme t);
    Theme getTheme() { return _current_theme; }

    // Grid Support
    int _cols = 3;
    int _rows = 2;
    smooth_ui_toolkit::AnimateValue _scroll_x;
    
    // Cursor Animation
    smooth_ui_toolkit::AnimateValue _cursor_x;
    smooth_ui_toolkit::AnimateValue _cursor_y;
    smooth_ui_toolkit::AnimateValue _cursor_w;
    smooth_ui_toolkit::AnimateValue _cursor_h;
    
    void renderGrid();
    void renderList(); // Added
    void readInputGrid(); // Added
    void readInputList(); // Added

private:
    std::vector<OptionInfo_t> _option_infos;
    
    // Animation states
    float _anim_time = 0.0f;
    
    // Input Debounce
    uint32_t _last_input_time = 0;
    uint32_t _release_check_time = 0;
    bool _last_key_state = false; // Added to track state

    // 按键延迟测量: 硬件按下时间 → 本帧 pushCanvas 完成
    uint32_t _key_event_ms = 0;
    uint32_t _action_ms = 0;
    bool _latency_pending = false;
    
    // Background stars
    struct Star {
        float x, y, speed, brightness;
    };
    std::vector<Star> _stars;
    
    void renderBackground();
    void initStars();
};

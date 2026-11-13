I will implement the requested changes to standardize controls, fix system mute, improve memory usage, and ensure reliable app exiting.

### 1. Standardize Control Scheme (Global Key Mapping)
- **Goal:** Map `;`, `.`, `,`, `/` to Up, Down, Left, Right respectively for ALL apps.
- **Implementation:** Modify `main/hal/keyboard/keybaord.cpp` in `Keyboard::update()`.
- **Logic:** After processing the raw key event, if the key code matches `;` (0x33), replace it with `KEY_UP` (0x52). Do the same for `.` (Down), `,` (Left), and `/` (Right). This ensures every app (Snake, Menu, etc.) receives standard arrow key events without modifying each app individually.

### 2. System-Level Force Exit
- **Goal:** Ensure the "Exit" key works reliably in all apps.
- **Implementation:** Modify `main/main.cpp` in the `app_task` loop.
- **Logic:** Add a global check for `KEY_ESC` (0x29). If pressed, call `GetMooncake().closeApp()`. This bypasses individual app logic and forces a return to the launcher, solving the "cannot exit" issue.

### 3. Fix Global Mute (System Mute)
- **Goal:** Ensure "System Mute" actually silences the speaker, including in the ESP app.
- **Implementation:** Modify `main/hal/hal.cpp` in `Hal::setGlobalMute`.
- **Logic:** Update `setGlobalMute` to explicitly call `M5.Speaker.setVolume(0)` when muted, and restore the volume from settings when unmuted. This ensures hardware-level silence.

### 4. Optimize Memory in LegacyApp
- **Goal:** Fix the "1KB free memory" issue when opening the ESP app.
- **Implementation:** Modify `main/apps/app_legacy_app/app_legacy_app.cpp` in `onOpen`.
- **Logic:** Explicitly call `GetHAL().wifiDeinit()` at the very beginning of `onOpen`. This ensures any WiFi resources from previous apps (like the Launcher or WiFi Scanner) are fully freed before LegacyApp initializes its own WiFi and graphics, preventing memory exhaustion.

### 5. Verify Icons
- **Goal:** Address "broken icons".
- **Action:** I will inspect `main/utils/icons/default_icons.cpp` and `main/apps/app_launcher/view/menu/menu.cpp` to ensure icon assets are correctly referenced and loaded. If specific icons are missing, I will fix the references.

### 6. Verification
- **Test:** Compile the firmware.
- **Verify:**
    - Controls: Check if `;` moves selection up in Launcher.
    - Exit: Check if `ESC` quits LegacyApp.
    - Mute: Check if Mute toggle silences LegacyApp tones.
    - Memory: Check logs for heap usage.

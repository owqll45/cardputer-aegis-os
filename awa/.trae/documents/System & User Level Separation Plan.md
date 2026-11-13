# System & User Level Separation Architecture Plan

## 1. Architectural Overview
To achieve an "Android-like" separation between System (OS/Launcher) and User (Apps) levels on an ESP32 (which is a single-process microcontroller), we need to simulate this isolation. True process isolation requires an MMU-capable OS (like Linux), but we can achieve a functional equivalent:

*   **System Level (OS/Launcher)**: The core firmware that boots the device, manages hardware (HAL), handles system UI (Status Bar, Keyboard Bar), and launches apps. It should be resilient.
*   **User Level (Apps)**: Individual applications that run "on top" of the system.
*   **Isolation Mechanism**:
    *   Since we share the same memory space, a true crash (Panic) resets the whole chip.
    *   **Solution**: We use the `CrashHandler` as the "Kernel Panic" handler. When an App crashes, the system catches the reset, identifies it was an App fault, and "reboots into the Desktop" gracefully, presenting a "The application has stopped" dialog, rather than a raw boot log.
    *   **Data Sharing**: Use NVS (Non-Volatile Storage) or a shared memory struct in RTC RAM to pass data between System and Apps, ensuring data persistence even if an app crashes.

## 2. Implementation Steps

### Phase 1: Core Architecture Refactoring
1.  **Define System Context**: Ensure `Launcher`, `SystemBar`, and `KeyboardBar` are treated as "System Services" that persist or are easily restored.
2.  **Define App Context**: Encapsulate Apps strictly. Ensure they don't directly manipulate System global state unless via defined APIs.

### Phase 2: Crash Isolation & Recovery (The "Android" Feel)
1.  **Enhanced Crash Handler**:
    *   Detect if the crash happened while an App was running (using the `_running_app_name` we added).
    *   If App crashed: Show "App [Name] has stopped" dialog.
    *   User Action: "Close App" (Returns to Desktop) or "Report" (Dumps log).
    *   **Crucial**: The system must *look* like it didn't crash. We do this by restoring the Desktop state immediately after the "Crash Dialog", giving the illusion that only the app died.

### Phase 3: Shared Data Interface
1.  **Clipboard/Intents**: Create a simple "Intent" system.
    *   Example: App A wants to open a URL -> Sends Intent -> System handles it.
    *   Example: App A crashes -> Data in Shared Preferences (NVS) remains safe.

### Phase 4: Full Localization (Chinese)
1.  **System UI**: Status bar, Menu, Settings -> All Chinese.
2.  **Crash UI**: "应用无响应" / "应用已停止运行" style dialogs.
3.  **Apps**: Ensure the provided apps (Water Level, 2048, etc.) have Chinese UI.

## 3. Execution Plan
1.  **Modify `crash_handler.cpp`**: To look exactly like an Android "App Crash" dialog.
2.  **Update `Launcher`**: To handle the "Recovery" state (auto-restore desktop after crash reboot).
3.  **Refactor Apps**: Ensure they register themselves correctly so the System knows who is running.
4.  **Flash & Verify**: Using the robust port-clearing method.

I will start by refining the `CrashHandler` to provide that specific "System vs App" experience you requested.
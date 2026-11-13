# Finalize Layout & Verify Stability (Sidebar Restored)

Since we have restored the **Left Sidebar** (Keyboard Map), the available screen width for the app grid is now approximately **200px** (240px total - ~40px sidebar).

## 1. Layout Refinement (Right Side)
We need to fit the **3x2 Grid** perfectly into the right side without overlapping the sidebar or the screen edge.
*   **Target Area**: X = 40 to X = 240 (Width: 200px).
*   **New Config**:
    *   **Margin Left**: `40` (Skip the sidebar).
    *   **Cell Width**: `66` (66 * 3 = 198px). This fits perfectly in the 200px space.
    *   **Cell Height**: `58` (Keep text visible).
    *   **Top Margin**: `18` (Avoid status bar).

## 2. Stability Check
*   Confirm that separating the layout (`Margin 45` -> `40`) stops the "Left Button Crash".
*   If it still crashes, we will need to debug the event conflict between `KeyboardBar` and `LauncherMenu`.

## 3. Crash Handler (Optional)
*   The user mentioned the crash report didn't show. This usually happens with **Watchdog Resets** (infinite loops) which reboot the device before the software handler can catch it.
*   We can add a "Safe Mode" boot check if needed, but fixing the layout overlap should prevent the crash entirely.

Shall we proceed with applying the **66px Width / 40px Margin** layout to perfect the display?
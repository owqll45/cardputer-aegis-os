# Layout Adjustment Plan (V17)

Based on your feedback ("move left", "move up", "font not showing"), here is the fix:

## 1. Move Left & Up
*   **Move Left**: Change `MARGIN_LEFT` from **40** to **28**. This moves the grid significantly closer to the sidebar without burying it.
*   **Move Up**: Change `GRID_START_Y` from **18** to **10**. This lifts the content up.

## 2. Fix Font Visibility (Critical)
*   **Problem**: In the previous version, the text (at `y+38`) was overlapping with the bottom of the icons (which end at `y+42`), making the text unreadable.
*   **Fix**: Move text down to `y + 45` to clear the icon.
*   **Height**: Increase `CELL_H` to **60** to accommodate the text.
    *   Total Height: `10 (top) + 60 + 60 = 130px`. This fits perfectly in the 135px screen.

## 3. Configuration
*   **Grid**: 3x2
*   **Cell Width**: **68** (Slightly wider to fill the space gained by moving left).

This should result in a layout where icons are close to the sidebar, text is clearly visible below the icons, and everything fits on screen.
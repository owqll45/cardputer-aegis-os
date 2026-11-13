# Layout Refinement Plan (V19)

Addressing your feedback: "Smaller horizontal gap", "Move further left", "Second row font still cut off".

## 1. Move Further Left & Reduce Gap
*   **Move Left**: Change `MARGIN_LEFT` from **15** to **5**. This is extremely aggressive, bringing the grid almost to the physical edge (ignoring the sidebar visually, but hopefully not colliding logically).
*   **Reduce Gap**: Decrease `CELL_W` from **70** to **64**. This pulls the columns closer together horizontally.

## 2. Fix Second Row Font Visibility (The Core Issue)
The issue is likely that the **total height** of the grid exceeds the screen bottom, or the text offset is too low.
*   **Strategy**: Compress Vertical Space.
    *   **Reduce Cell Height**: Change `CELL_H` from **60** to **52**. This saves 16px total height (8px * 2 rows).
    *   **Lift Text**: Move text drawing from `y + 45` to `y + 36`.
    *   **Adjust Icon**: Ensure icons are drawn higher (start at `y + 2` instead of `y + 4`).
    *   **Start Position**: Keep `GRID_START_Y` at **2**.

**Calculation:**
*   Row 1: Y=2 to Y=54. Text at Y=38.
*   Row 2: Y=56 to Y=108. Text at Y=92.
*   Screen Bottom: 135.
*   **Result**: The bottom of the second row (108px) is well above the screen bottom (135px). There is **27px** of clearance. The text should be perfectly visible.

## 3. Configuration Summary (V19)
*   `GRID_COLS = 3`, `GRID_ROWS = 2`
*   `CELL_W = 64` (Tighter horizontal)
*   `CELL_H = 52` (Compressed vertical)
*   `GRID_START_Y = 2` (Top aligned)
*   `MARGIN_LEFT = 5` (Extreme left)
*   Text at `y + 36` (Closer to icon)

I will apply these changes now.
# Interface system

The interface borrows Zune's strengths—large type, flat planes, purposeful
accent colour, and information hierarchy—without copying branded assets.

- Canvas: `#090B10`; panels: `#11151E`.
- Primary accent: teal `#00D7C7`; progress accent: blue `#35A7FF`.
- Montserrat is provided by LVGL under SIL OFL 1.1 and compiled in the 14, 18,
  22, 28, 36, and 48 px sizes used by the interface.
- Pages share a 40 px status header and 46 px bottom navigation.
- Lists use fixed-height rows, bounded text, ellipsis, and persistent scroll.
- Destructive operations use long press instead of confirmation dialogs to
  keep the small-screen flow predictable.

The physical panel is natively 320x480. LVGL rotates the display 90 degrees to
480x320; the board support package applies the corresponding touch transform.


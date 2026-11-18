#pragma once

namespace musicremote {

// Optional CJK glyphs from cjk_font::kFontPath on the microSD card, rendered
// by LVGL's tiny_ttf into the ui_font_cjk_<size> fonts that ui_font_ext_<size>
// falls back to. Until a font is active (or without the card or file), those
// fonts are copies of Montserrat (LV_SYMBOL_* glyphs only) and CJK text shows
// placeholder boxes.
//
// start_cjk_font() checks the file and starts a low-priority task that
// hashes it; only a font whose size and SHA-256 match cjk_font::kReviewedFonts
// is ever parsed. Call once after the card mounts. Logs one line unless the
// verification starts.
bool start_cjk_font(bool sd_mounted);

// Activates a verified font: creates the tiny_ttf fonts, swaps them in, and
// re-measures all labels. Call from the GUI task with LVGL locked, each loop;
// it does work once. Logs one line when verification finishes either way.
void poll_cjk_font();

} // namespace musicremote

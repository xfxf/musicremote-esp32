#pragma once

namespace musicremote {

// Loads cjk_font::kFontPath from the microSD card into the ui_font_cjk_<size>
// fonts that ui_font_ext_<size> falls back to. Without the card or the file,
// those fonts stay copies of Montserrat (LV_SYMBOL_* glyphs only) and CJK
// text shows placeholder boxes. Logs exactly one line either way.
//
// Call once, after lv_init() and before any label is drawn: either before
// the GUI task starts, as app_main does, or with the GUI mutex held.
bool load_cjk_font(bool sd_mounted);

} // namespace musicremote

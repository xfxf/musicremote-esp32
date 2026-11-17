#include "music_ui.hpp"
#include "settings_policy.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace musicremote {
namespace {

lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }
constexpr uint32_t kBackground = 0x090B10;
constexpr uint32_t kPanel = 0x11151E;
constexpr uint32_t kMuted = 0x8C96A8;
constexpr uint32_t kText = 0xF5F7FA;
constexpr uint32_t kAccent = 0x00D7C7;
constexpr uint32_t kAccentBlue = 0x35A7FF;
// How long the previous cover stays up while the next track's cover loads.
constexpr int64_t kArtworkLingerUs = 2'000'000;
constexpr char kTag[] = "music_ui";

// LVGL's default symbol page lacks ~ | ^ and `, which are valid in Wi-Fi and
// AIMP passwords. This copy of its 1# page adds them to the bottom row, so
// together with the letter pages every printable ASCII character is typeable.
constexpr lv_buttonmatrix_ctrl_t key_control(uint32_t flags, uint32_t width) {
  return static_cast<lv_buttonmatrix_ctrl_t>(flags | width);
}
constexpr lv_buttonmatrix_ctrl_t kKey = key_control(LV_BUTTONMATRIX_CTRL_POPOVER, 1);
constexpr lv_buttonmatrix_ctrl_t kWideKey = key_control(LV_BUTTONMATRIX_CTRL_POPOVER, 2);
constexpr lv_buttonmatrix_ctrl_t kSpaceKey = key_control(0, 4);
constexpr lv_buttonmatrix_ctrl_t kModeKey = key_control(LV_KEYBOARD_CTRL_BUTTON_FLAGS, 2);
constexpr lv_buttonmatrix_ctrl_t kCheckedKey = key_control(LV_BUTTONMATRIX_CTRL_CHECKED, 2);
const char *const kSymbolKeyboardMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    LV_SYMBOL_KEYBOARD, "~", "|", "^", "`", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
const lv_buttonmatrix_ctrl_t kSymbolKeyboardControls[] = {
    kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kCheckedKey,
    kModeKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey,
    kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey, kKey,
    kModeKey, kWideKey, kWideKey, kWideKey, kWideKey, kCheckedKey, kSpaceKey, kCheckedKey, kModeKey};
static_assert(std::size(kSymbolKeyboardMap) == 48);
static_assert(std::size(kSymbolKeyboardControls) == 44);

const char *connection_text(ConnectionState state) {
  switch (state) {
  case ConnectionState::online:
    return "AIMP ONLINE";
  case ConnectionState::connecting:
    return "CONNECTING";
  case ConnectionState::authenticating:
    return "AUTHENTICATING";
  case ConnectionState::auth_failed:
    return "PASSWORD REJECTED";
  case ConnectionState::wifi:
    return "WIFI";
  case ConnectionState::offline:
  default:
    return "OFFLINE";
  }
}

std::string format_time(int32_t milliseconds) {
  const int32_t seconds = std::max<int32_t>(0, milliseconds / 1000);
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%ld:%02ld", static_cast<long>(seconds / 60),
                static_cast<long>(seconds % 60));
  return buffer;
}

void format_gain(char (&text)[16], int32_t value) {
  std::snprintf(text, sizeof(text), "%+.1f dB", (value - 150) / 10.0);
}

void format_band(char (&text)[16], uint16_t frequency_hz) {
  if (frequency_hz >= 1000 && frequency_hz % 1000 == 0) {
    std::snprintf(text, sizeof(text), "%uK", static_cast<unsigned>(frequency_hz / 1000));
  } else {
    std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(frequency_hz));
  }
}

} // namespace

MusicUi::MusicUi(Model &model, MonkeyMoteClient &client, DeviceSettings &settings,
                 std::function<void(uint8_t)> brightness_callback)
    : model_(model), client_(client), settings_(settings),
      brightness_callback_(std::move(brightness_callback)),
      last_interaction_us_(esp_timer_get_time()) {
  create_ui();
}

MusicUi::~MusicUi() = default;

void MusicUi::start() {
  if (task_ != nullptr) {
    return;
  }
  TaskHandle_t handle = nullptr;
  if (xTaskCreatePinnedToCore(task_entry, "music-ui", 16 * 1024, this, 4, &handle, 1) ==
      pdPASS) {
    task_ = handle;
  }
}

void MusicUi::apply_display_orientation(bool flipped) {
  lv_display_t *display = lv_display_get_default();
  if (display == nullptr) {
    return;
  }
  // The 320x480 panel needs a quarter turn to present the 480x320 UI. Rotation
  // 270 is the 180-degree complement of 90. LVGL rotates touch points from this
  // value and the BSP reprograms the ST7796 MADCTL, so nothing else changes.
  const lv_display_rotation_t rotation =
      flipped ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90;
  if (lv_display_get_rotation(display) != rotation) {
    lv_display_set_rotation(display, rotation);
    ESP_LOGI(kTag, "Display orientation: %s", flipped ? "flipped" : "normal");
  }
}

bool MusicUi::capture_rgb565(std::vector<uint8_t> &pixels, uint32_t &width,
                             uint32_t &height, uint32_t &stride) {
  std::lock_guard lock(mutex_);
  lv_draw_buf_t *snapshot = lv_snapshot_take(root_, LV_COLOR_FORMAT_RGB565);
  if (snapshot == nullptr) {
    return false;
  }
  width = snapshot->header.w;
  height = snapshot->header.h;
  stride = snapshot->header.stride;
  const size_t bytes = static_cast<size_t>(stride) * height;
  pixels.assign(snapshot->data, snapshot->data + bytes);
  lv_draw_buf_destroy(snapshot);
  return true;
}

bool MusicUi::select_page_for_diagnostics(uint8_t page) {
  if (page >= 4) {
    return false;
  }
  std::lock_guard lock(mutex_);
  end_advanced_demo();
  last_interaction_us_ = esp_timer_get_time();
  set_display_blank(false);
  lv_obj_set_hidden(filter_overlay_, true);
  lv_obj_set_hidden(search_overlay_, true);
  lv_obj_set_hidden(library_overlay_, true);
  lv_obj_set_hidden(lyrics_overlay_, true);
  lv_obj_set_hidden(settings_overlay_, true);
  lv_obj_set_hidden(setting_editor_, true);
  lv_obj_set_hidden(equalizer_overlay_, true);
  lv_obj_set_hidden(power_confirm_overlay_, true);
  show_page(static_cast<Page>(page));
  return true;
}

bool MusicUi::select_overlay_for_diagnostics(uint8_t overlay) {
  if (overlay > 4) {
    return false;
  }
  std::lock_guard lock(mutex_);
  end_advanced_demo();
  last_interaction_us_ = esp_timer_get_time();
  set_display_blank(false);
  lv_obj_set_hidden(filter_overlay_, overlay != 1);
  lv_obj_set_hidden(search_overlay_, overlay != 2);
  lv_obj_set_hidden(library_overlay_, overlay != 3);
  lv_obj_set_hidden(lyrics_overlay_, overlay != 4);
  lv_obj_set_hidden(settings_overlay_, true);
  lv_obj_set_hidden(setting_editor_, true);
  lv_obj_set_hidden(equalizer_overlay_, true);
  lv_obj_set_hidden(power_confirm_overlay_, true);
  if (overlay == 1) {
    lv_obj_move_foreground(filter_overlay_);
  } else if (overlay == 2) {
    lv_obj_move_foreground(search_overlay_);
  } else if (overlay == 3) {
    show_library_root();
    lv_obj_move_foreground(library_overlay_);
  } else if (overlay == 4) {
    lv_obj_move_foreground(lyrics_overlay_);
  }
  return true;
}

bool MusicUi::select_library_category_for_diagnostics(uint8_t category) {
  if (category >= 4) {
    return false;
  }
  std::lock_guard lock(mutex_);
  end_advanced_demo();
  library_root_visible_ = false;
  library_page_start_ = 0;
  lv_obj_set_hidden(filter_overlay_, true);
  lv_obj_set_hidden(search_overlay_, true);
  lv_obj_set_hidden(lyrics_overlay_, true);
  lv_obj_set_hidden(settings_overlay_, true);
  lv_obj_set_hidden(setting_editor_, true);
  lv_obj_set_hidden(equalizer_overlay_, true);
  lv_obj_set_hidden(power_confirm_overlay_, true);
  lv_obj_set_hidden(library_overlay_, false);
  lv_obj_move_foreground(library_overlay_);
  lv_label_set_text(library_count_, "LOADING...");
  lv_obj_clean(library_list_);
  client_.enqueue({CommandType::request_library_root, category});
  return true;
}

bool MusicUi::select_settings_for_diagnostics(uint8_t section) {
  if (section >= std::size(settings_panels_)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  end_advanced_demo();
  last_interaction_us_ = esp_timer_get_time();
  set_display_blank(false);
  lv_obj_set_hidden(filter_overlay_, true);
  lv_obj_set_hidden(search_overlay_, true);
  lv_obj_set_hidden(library_overlay_, true);
  lv_obj_set_hidden(lyrics_overlay_, true);
  lv_obj_set_hidden(setting_editor_, true);
  lv_obj_set_hidden(equalizer_overlay_, true);
  lv_obj_set_hidden(power_confirm_overlay_, true);
  const auto selected = static_cast<SettingsSection>(section);
  show_settings_section(selected);
  return true;
}

bool MusicUi::select_advanced_demo_for_diagnostics(uint8_t view) {
  if (view > 2) {
    return false;
  }
  std::lock_guard lock(mutex_);
  advanced_demo_active_ = true;
  last_interaction_us_ = esp_timer_get_time();
  set_display_blank(false);
  lv_obj_set_hidden(filter_overlay_, true);
  lv_obj_set_hidden(search_overlay_, true);
  lv_obj_set_hidden(library_overlay_, true);
  lv_obj_set_hidden(lyrics_overlay_, true);
  lv_obj_set_hidden(setting_editor_, true);
  lv_obj_set_hidden(equalizer_overlay_, true);
  lv_obj_set_hidden(power_confirm_overlay_, true);

  // This diagnostic state is deliberately view-only: snapshot_ retains the
  // real connection state, so touch events cannot enqueue a command while a
  // synthetic capability screen is being captured.
  Snapshot demo = snapshot_;
  demo.capabilities_known = true;
  demo.server_capabilities = advanced::kEqualizerCapability |
                             advanced::kSleepTimerCapability |
                             advanced::kComputerPowerCapability;
  demo.equalizer.known = true;
  demo.equalizer.enabled = true;
  demo.equalizer.preamp_tenths_offset = 150;
  demo.equalizer.bands = {{70, 150}, {180, 165}, {320, 155}, {600, 145},
                          {1000, 150}, {3000, 160}, {6000, 150}, {12000, 140}};
  demo.advanced_availability.equalizer = advanced::StateAvailability::ready;
  demo.advanced_availability.equalizer_presets = advanced::StateAvailability::ready;
  demo.equalizer_revision = rendered_equalizer_revision_ == UINT64_MAX
                                ? 1
                                : rendered_equalizer_revision_ + 1;
  demo.sleep_timer = {.known = true,
                      .enabled = true,
                      .trigger = 1,
                      .action = 1,
                      .seconds_remaining = 30 * 60};
  demo.advanced_availability.sleep_timer = advanced::StateAvailability::ready;
  update_advanced_controls(demo);
  show_settings_section(SettingsSection::advanced);

  if (view == 1) {
    lv_obj_set_hidden(equalizer_overlay_, false);
    lv_obj_move_foreground(equalizer_overlay_);
  } else if (view == 2) {
    lv_label_set_text(power_confirm_message_,
                      "Shut down the AIMP computer now?\n"
                      "Unsaved work on that computer may be lost.");
    pending_power_action_ = -1;
    lv_obj_set_hidden(power_confirm_overlay_, false);
    lv_obj_move_foreground(power_confirm_overlay_);
  }
  return true;
}

void MusicUi::task_entry(void *context) {
  static_cast<MusicUi *>(context)->run();
  vTaskDelete(nullptr);
}

void MusicUi::run() {
  while (true) {
    {
      std::lock_guard lock(mutex_);
      Snapshot latest;
      if (model_.copy_if_changed(rendered_revision_, latest)) {
        update(latest);
        rendered_revision_ = latest.revision;
      }
      if (!advanced_demo_active_) {
        // Applies an equalizer rebuild deferred while a slider was dragged.
        sync_equalizer(snapshot_);
      }
      update_timeline(esp_timer_get_time());
      append_queue_rows(snapshot_);
      lv_timer_handler();
      poll_display_timeout(esp_timer_get_time());
      poll_artwork_linger(esp_timer_get_time());
    }
    vTaskDelay(pdMS_TO_TICKS(16));
  }
}

lv_obj_t *MusicUi::make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              lv_color_t text_color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, text_color, 0);
  return label;
}

lv_obj_t *MusicUi::make_button(lv_obj_t *parent, const char *text, int width, int height) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_size(button, width, height);
  lv_obj_set_ext_click_area(button, 4);
  lv_obj_set_style_radius(button, 4, 0);
  lv_obj_set_style_bg_color(button, color(kPanel), 0);
  lv_obj_set_style_bg_color(button, color(0x174148), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(button, color(0x174148), LV_STATE_CHECKED);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_t *label = make_label(button, text, &lv_font_montserrat_18, color(kText));
  lv_obj_set_clickable(label, false);
  lv_obj_center(label);
  return button;
}

void MusicUi::create_ui() {
  std::lock_guard lock(mutex_);
  root_ = lv_screen_active();
  lv_obj_set_style_bg_color(root_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  create_header();
  create_now_page();
  create_queue_page();
  create_playlists_page();
  create_more_page();
  create_navigation();
  create_settings_overlays();
  create_equalizer_overlay();
  create_filter_overlays();
  create_library_overlay();
  create_lyrics_overlay();
  show_page(Page::now);
}

void MusicUi::create_header() {
  lv_obj_t *header = lv_obj_create(root_);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, lv_pct(100), 40);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_bg_color(header, color(kBackground), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

  lv_obj_t *brand = make_label(header, "MUSIC / REMOTE", &lv_font_montserrat_18, color(kText));
  lv_obj_align(brand, LV_ALIGN_LEFT_MID, 14, 0);
  status_dot_ = lv_obj_create(header);
  lv_obj_remove_style_all(status_dot_);
  lv_obj_set_size(status_dot_, 8, 8);
  lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(status_dot_, color(0xE04F5F), 0);
  lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, 0);
  lv_obj_align(status_dot_, LV_ALIGN_RIGHT_MID, -14, 0);
  status_label_ = make_label(header, "OFFLINE", &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_width(status_label_, 190);
  lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(status_label_, LV_ALIGN_RIGHT_MID, -34, 0);
}

void MusicUi::create_navigation() {
  lv_obj_t *nav = lv_obj_create(root_);
  lv_obj_remove_style_all(nav);
  lv_obj_set_size(nav, lv_pct(100), 46);
  lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(nav, color(0x0D1118), 0);
  lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
  lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

  const char *labels[] = {"NOW", "QUEUE", "PLAYLISTS", "MORE"};
  for (size_t i = 0; i < 4; ++i) {
    nav_buttons_[i] = lv_button_create(nav);
    // Four contiguous hit regions cover the complete bottom bar, including
    // the physical screen edge. There are intentionally no dead gutters.
    lv_obj_set_pos(nav_buttons_[i], static_cast<int32_t>(i) * 120, 0);
    lv_obj_set_size(nav_buttons_[i], 120, 46);
    lv_obj_set_style_radius(nav_buttons_[i], 0, 0);
    lv_obj_set_style_bg_opa(nav_buttons_[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(nav_buttons_[i], 0, 0);
    lv_obj_add_event_cb(nav_buttons_[i], event_handler, LV_EVENT_PRESSED, this);
    lv_obj_t *label = make_label(nav_buttons_[i], labels[i], &lv_font_montserrat_14,
                                 i == 0 ? color(kAccent) : color(kMuted));
    lv_obj_set_clickable(label, false);
    lv_obj_center(label);
  }
}

void MusicUi::create_now_page() {
  lv_obj_t *page = lv_obj_create(root_);
  pages_[0] = page;
  lv_obj_remove_style_all(page);
  lv_obj_set_pos(page, 0, 40);
  lv_obj_set_size(page, 480, 234);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  art_ = lv_obj_create(page);
  lv_obj_set_pos(art_, 14, 8);
  lv_obj_set_size(art_, 144, 144);
  lv_obj_set_style_radius(art_, 2, 0);
  lv_obj_set_style_bg_color(art_, color(0x163D59), 0);
  lv_obj_set_style_border_width(art_, 0, 0);
  lv_obj_set_style_shadow_width(art_, 0, 0);
  lv_obj_clear_flag(art_, LV_OBJ_FLAG_SCROLLABLE);
  art_image_ = lv_image_create(art_);
  lv_obj_set_clickable(art_image_, false);
  lv_obj_add_flag(art_image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_center(art_image_);
  art_fallback_ = make_label(art_, LV_SYMBOL_AUDIO, &lv_font_montserrat_48, color(kAccent));
  lv_obj_set_clickable(art_fallback_, false);
  lv_obj_center(art_fallback_);

  title_label_ = make_label(page, "NOTHING PLAYING", &lv_font_montserrat_28, color(kText));
  lv_obj_set_pos(title_label_, 176, 8);
  lv_obj_set_size(title_label_, 286, 36);
  lv_label_set_long_mode(title_label_, LV_LABEL_LONG_DOT);
  artist_label_ = make_label(page, "AIMP", &lv_font_montserrat_18, color(kAccent));
  lv_obj_set_pos(artist_label_, 176, 50);
  lv_obj_set_size(artist_label_, 286, 24);
  lv_label_set_long_mode(artist_label_, LV_LABEL_LONG_DOT);
  album_label_ = make_label(page, "WAITING FOR A TRACK", &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_pos(album_label_, 176, 80);
  lv_obj_set_size(album_label_, 286, 18);
  lv_label_set_long_mode(album_label_, LV_LABEL_LONG_DOT);

  shuffle_button_ = make_button(page, LV_SYMBOL_SHUFFLE, 48, 48);
  lv_obj_set_pos(shuffle_button_, 176, 112);
  repeat_button_ = make_button(page, LV_SYMBOL_LOOP, 48, 48);
  lv_obj_set_pos(repeat_button_, 232, 112);
  lv_obj_add_flag(shuffle_button_, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_flag(repeat_button_, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_event_cb(shuffle_button_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(repeat_button_, event_handler, LV_EVENT_CLICKED, this);

  lv_obj_t *previous = make_button(page, LV_SYMBOL_PREV, 54, 48);
  play_button_ = make_button(page, LV_SYMBOL_PLAY, 68, 54);
  play_icon_ = lv_obj_get_child(play_button_, 0);
  lv_obj_t *next = make_button(page, LV_SYMBOL_NEXT, 54, 48);
  lv_obj_set_pos(previous, 290, 112);
  lv_obj_set_pos(play_button_, 352, 106);
  lv_obj_set_pos(next, 426, 112);
  lv_obj_add_event_cb(previous, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(play_button_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(play_button_, event_handler, LV_EVENT_LONG_PRESSED, this);
  lv_obj_add_event_cb(next, event_handler, LV_EVENT_CLICKED, this);

  progress_slider_ = lv_slider_create(page);
  lv_obj_set_pos(progress_slider_, 14, 174);
  lv_obj_set_size(progress_slider_, 252, 12);
  lv_obj_set_ext_click_area(progress_slider_, 16);
  lv_slider_set_range(progress_slider_, 0, 1);
  lv_obj_set_style_bg_color(progress_slider_, color(0x28303D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress_slider_, color(kAccentBlue), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(progress_slider_, color(kText), LV_PART_KNOB);
  lv_obj_add_event_cb(progress_slider_, event_handler, LV_EVENT_RELEASED, this);
  time_label_ = make_label(page, "0:00  /  0:00", &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_pos(time_label_, 14, 207);

  main_volume_slider_ = lv_slider_create(page);
  lv_obj_set_pos(main_volume_slider_, 300, 174);
  lv_obj_set_size(main_volume_slider_, 166, 12);
  lv_obj_set_ext_click_area(main_volume_slider_, 16);
  lv_slider_set_range(main_volume_slider_, 0, 100);
  lv_obj_set_style_bg_color(main_volume_slider_, color(0x28303D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(main_volume_slider_, color(kAccent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(main_volume_slider_, color(kText), LV_PART_KNOB);
  lv_obj_add_event_cb(main_volume_slider_, event_handler, LV_EVENT_RELEASED, this);
  main_volume_label_ = make_label(page, "VOL 0%", &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_pos(main_volume_label_, 300, 207);
}

void MusicUi::create_queue_page() {
  lv_obj_t *page = lv_obj_create(root_);
  pages_[1] = page;
  lv_obj_remove_style_all(page);
  lv_obj_set_pos(page, 0, 40);
  lv_obj_set_size(page, 480, 234);
  queue_cache_label_ =
      make_label(page, "CACHED", &lv_font_montserrat_14, color(kAccent));
  lv_obj_set_pos(queue_cache_label_, 14, 0);
  lv_obj_add_flag(queue_cache_label_, LV_OBJ_FLAG_HIDDEN);
  queue_title_ = make_label(page, "CURRENT PLAYLIST", &lv_font_montserrat_18, color(kText));
  lv_obj_set_pos(queue_title_, 14, 8);
  lv_obj_set_width(queue_title_, 144);
  lv_label_set_long_mode(queue_title_, LV_LABEL_LONG_DOT);
  queue_filter_button_ = make_button(page, "FIND", 82, 42);
  lv_obj_set_pos(queue_filter_button_, 168, 0);
  lv_obj_add_event_cb(queue_filter_button_, event_handler, LV_EVENT_CLICKED, this);
  queue_previous_page_ = make_button(page, LV_SYMBOL_LEFT, 48, 42);
  lv_obj_set_pos(queue_previous_page_, 258, 0);
  lv_obj_add_event_cb(queue_previous_page_, event_handler, LV_EVENT_CLICKED, this);
  queue_next_page_ = make_button(page, LV_SYMBOL_RIGHT, 48, 42);
  lv_obj_set_pos(queue_next_page_, 312, 0);
  lv_obj_add_event_cb(queue_next_page_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_t *clear = make_button(page, "CLEAR", 80, 42);
  lv_obj_align(clear, LV_ALIGN_TOP_RIGHT, -14, 0);
  lv_obj_add_event_cb(clear, event_handler, LV_EVENT_LONG_PRESSED, this);

  queue_list_ = lv_obj_create(page);
  lv_obj_set_pos(queue_list_, 8, 42);
  lv_obj_set_size(queue_list_, 464, 188);
  lv_obj_set_style_bg_opa(queue_list_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(queue_list_, 0, 0);
  lv_obj_set_style_pad_all(queue_list_, 0, 0);
  lv_obj_set_style_pad_row(queue_list_, 3, 0);
  lv_obj_set_flex_flow(queue_list_, LV_FLEX_FLOW_COLUMN);
}

void MusicUi::create_filter_overlays() {
  filter_overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(filter_overlay_);
  lv_obj_set_size(filter_overlay_, 480, 320);
  lv_obj_set_pos(filter_overlay_, 0, 0);
  lv_obj_set_style_bg_color(filter_overlay_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(filter_overlay_, LV_OPA_COVER, 0);
  lv_obj_add_flag(filter_overlay_, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *title = make_label(filter_overlay_, "BROWSE A-Z", &lv_font_montserrat_22,
                               color(kText));
  lv_obj_set_pos(title, 14, 14);
  filter_search_button_ = make_button(filter_overlay_, "SEARCH", 100, 48);
  lv_obj_set_pos(filter_search_button_, 260, 4);
  lv_obj_add_event_cb(filter_search_button_, event_handler, LV_EVENT_CLICKED, this);
  filter_close_button_ = make_button(filter_overlay_, "CLOSE", 100, 48);
  lv_obj_set_pos(filter_close_button_, 368, 4);
  lv_obj_add_event_cb(filter_close_button_, event_handler, LV_EVENT_CLICKED, this);

  static constexpr const char *kAlphabet[28] = {
      "#", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
      "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "ALL"};
  for (size_t i = 0; i < std::size(alphabet_buttons_); ++i) {
    alphabet_buttons_[i] = make_button(filter_overlay_, kAlphabet[i], 72, 44);
    lv_obj_set_pos(alphabet_buttons_[i], 14 + static_cast<int>(i % 6) * 76,
                   62 + static_cast<int>(i / 6) * 49);
    lv_obj_add_event_cb(alphabet_buttons_[i], event_handler, LV_EVENT_CLICKED, this);
  }

  search_overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(search_overlay_);
  lv_obj_set_size(search_overlay_, 480, 320);
  lv_obj_set_pos(search_overlay_, 0, 0);
  lv_obj_set_style_bg_color(search_overlay_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(search_overlay_, LV_OPA_COVER, 0);
  lv_obj_add_flag(search_overlay_, LV_OBJ_FLAG_HIDDEN);

  search_textarea_ = lv_textarea_create(search_overlay_);
  lv_obj_set_pos(search_textarea_, 12, 8);
  lv_obj_set_size(search_textarea_, 332, 48);
  lv_textarea_set_one_line(search_textarea_, true);
  lv_textarea_set_max_length(search_textarea_, 64);
  lv_textarea_set_placeholder_text(search_textarea_, "TITLE OR ARTIST");
  lv_obj_set_style_text_font(search_textarea_, &lv_font_montserrat_18, 0);
  lv_obj_set_style_bg_color(search_textarea_, color(kPanel), 0);
  lv_obj_set_style_text_color(search_textarea_, color(kText), 0);

  lv_obj_t *cancel = make_button(search_overlay_, "CANCEL", 116, 48);
  lv_obj_set_pos(cancel, 352, 8);

  search_keyboard_ = lv_keyboard_create(search_overlay_);
  // lv_keyboard_create() aligns to BOTTOM_MID and lv_obj_set_pos() keeps that
  // alignment, so anchor explicitly to keep the keyboard inside the screen.
  lv_obj_align(search_keyboard_, LV_ALIGN_TOP_LEFT, 8, 64);
  lv_obj_set_size(search_keyboard_, 464, 248);
  lv_keyboard_set_textarea(search_keyboard_, search_textarea_);
  lv_obj_set_style_text_font(search_keyboard_, &lv_font_montserrat_18, LV_PART_ITEMS);
  // Enter on a one-line textarea sends READY only to the textarea, while the
  // checkmark sends it to both; listening on the textarea submits exactly once.
  lv_obj_add_event_cb(search_textarea_, event_handler, LV_EVENT_READY, this);
  lv_obj_add_event_cb(search_keyboard_, event_handler, LV_EVENT_CANCEL, this);
  lv_obj_add_event_cb(cancel, [](lv_event_t *event) {
    auto *ui = static_cast<MusicUi *>(lv_event_get_user_data(event));
    if (ui != nullptr) {
      lv_obj_add_flag(ui->search_overlay_, LV_OBJ_FLAG_HIDDEN);
      if (ui->search_context_ == SearchContext::library) {
        lv_obj_remove_flag(ui->library_overlay_, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_remove_flag(ui->filter_overlay_, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }, LV_EVENT_CLICKED, this);
}

void MusicUi::create_library_overlay() {
  library_overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(library_overlay_);
  lv_obj_set_size(library_overlay_, 480, 320);
  lv_obj_set_pos(library_overlay_, 0, 0);
  lv_obj_set_style_bg_color(library_overlay_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(library_overlay_, LV_OPA_COVER, 0);
  lv_obj_add_flag(library_overlay_, LV_OBJ_FLAG_HIDDEN);

  library_title_ = make_label(library_overlay_, "MEDIA LIBRARY", &lv_font_montserrat_22,
                              color(kText));
  lv_obj_set_pos(library_title_, 14, 8);
  lv_obj_set_size(library_title_, 210, 28);
  lv_label_set_long_mode(library_title_, LV_LABEL_LONG_DOT);
  library_count_ = make_label(library_overlay_, "SELECT A VIEW", &lv_font_montserrat_14,
                              color(kMuted));
  lv_obj_set_pos(library_count_, 14, 37);

  library_search_button_ = make_button(library_overlay_, "SEARCH", 100, 48);
  lv_obj_set_pos(library_search_button_, 258, 4);
  library_back_button_ = make_button(library_overlay_, "CLOSE", 100, 48);
  lv_obj_set_pos(library_back_button_, 368, 4);
  lv_obj_add_event_cb(library_search_button_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(library_back_button_, event_handler, LV_EVENT_CLICKED, this);

  library_list_ = lv_obj_create(library_overlay_);
  lv_obj_set_pos(library_list_, 8, 58);
  lv_obj_set_size(library_list_, 464, 210);
  lv_obj_set_style_bg_opa(library_list_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(library_list_, 0, 0);
  lv_obj_set_style_pad_all(library_list_, 0, 0);
  lv_obj_set_style_pad_row(library_list_, 3, 0);
  lv_obj_set_flex_flow(library_list_, LV_FLEX_FLOW_COLUMN);

  library_previous_page_ = make_button(library_overlay_, LV_SYMBOL_LEFT " PREV", 112, 44);
  library_next_page_ = make_button(library_overlay_, "NEXT " LV_SYMBOL_RIGHT, 112, 44);
  lv_obj_set_pos(library_previous_page_, 122, 272);
  lv_obj_set_pos(library_next_page_, 246, 272);
  lv_obj_add_event_cb(library_previous_page_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(library_next_page_, event_handler, LV_EVENT_CLICKED, this);
  show_library_root();
}

void MusicUi::create_lyrics_overlay() {
  lyrics_overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(lyrics_overlay_);
  lv_obj_set_size(lyrics_overlay_, 480, 320);
  lv_obj_set_pos(lyrics_overlay_, 0, 0);
  lv_obj_set_style_bg_color(lyrics_overlay_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(lyrics_overlay_, LV_OPA_COVER, 0);
  lv_obj_add_flag(lyrics_overlay_, LV_OBJ_FLAG_HIDDEN);

  lyrics_title_ = make_label(lyrics_overlay_, "LYRICS", &lv_font_montserrat_22, color(kText));
  lv_obj_set_pos(lyrics_title_, 14, 12);
  lv_obj_set_size(lyrics_title_, 330, 28);
  lv_label_set_long_mode(lyrics_title_, LV_LABEL_LONG_DOT);
  lv_obj_t *close = make_button(lyrics_overlay_, "CLOSE", 100, 48);
  lv_obj_set_pos(close, 368, 4);
  lv_obj_add_event_cb(close, [](lv_event_t *event) {
    auto *ui = static_cast<MusicUi *>(lv_event_get_user_data(event));
    if (ui != nullptr) {
      lv_obj_add_flag(ui->lyrics_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
  }, LV_EVENT_CLICKED, this);

  lv_obj_t *scroll = lv_obj_create(lyrics_overlay_);
  lv_obj_set_pos(scroll, 10, 58);
  lv_obj_set_size(scroll, 460, 252);
  lv_obj_set_style_bg_color(scroll, color(kPanel), 0);
  lv_obj_set_style_border_width(scroll, 0, 0);
  lv_obj_set_style_radius(scroll, 3, 0);
  lv_obj_set_style_pad_all(scroll, 12, 0);
  lyrics_text_ = make_label(scroll, "REQUESTING LYRICS...", &lv_font_montserrat_18,
                            color(kText));
  lv_obj_set_width(lyrics_text_, 424);
  lv_label_set_long_mode(lyrics_text_, LV_LABEL_LONG_WRAP);
}

void MusicUi::show_library_root() {
  library_root_visible_ = true;
  library_page_start_ = 0;
  lv_obj_clean(library_list_);
  lv_label_set_text(library_title_, "MEDIA LIBRARY");
  lv_label_set_text(library_count_, "SELECT A VIEW");
  lv_label_set_text(lv_obj_get_child(library_back_button_, 0), "CLOSE");
  static constexpr std::array<const char *, 4> kLabels{
      "ARTISTS", "ALBUMS", "GENRES", "COMPOSERS"};
  for (const char *label : kLabels) {
    lv_obj_t *row = make_button(library_list_, label, 448, 48);
    lv_obj_add_event_cb(row, library_row_handler, LV_EVENT_CLICKED, this);
  }
  lv_obj_add_state(library_previous_page_, LV_STATE_DISABLED);
  lv_obj_add_state(library_next_page_, LV_STATE_DISABLED);
}

void MusicUi::rebuild_library(const Snapshot &snapshot) {
  constexpr size_t kPageSize = 25;
  const LibraryItems &items = *snapshot.library_items;
  if (library_root_visible_) {
    return;
  }
  lv_obj_clean(library_list_);
  lv_label_set_text(library_title_, snapshot.library_title.c_str());
  lv_label_set_text(lv_obj_get_child(library_back_button_, 0), "BACK");
  if (snapshot.library_loading) {
    lv_label_set_text(library_count_, "LOADING...");
    lv_obj_t *loading = make_label(library_list_, "ASKING AIMP FOR LIBRARY DATA",
                                   &lv_font_montserrat_18, color(kMuted));
    lv_obj_set_style_pad_all(loading, 16, 0);
    return;
  }
  if (library_page_start_ >= items.size()) {
    library_page_start_ = items.empty()
                              ? 0
                              : (items.size() - 1) / kPageSize * kPageSize;
  }
  const size_t end = std::min(library_page_start_ + kPageSize, items.size());
  char count[64];
  if (items.empty()) {
    std::snprintf(count, sizeof(count), "%s",
                  snapshot.library_message.empty() ? "NO RESULTS"
                                                   : snapshot.library_message.c_str());
  } else {
    std::snprintf(count, sizeof(count), "%u-%u OF %u%s",
                  static_cast<unsigned>(library_page_start_ + 1),
                  static_cast<unsigned>(end),
                  static_cast<unsigned>(snapshot.library_total_count),
                  snapshot.library_truncated ? " (LIMITED)" : "");
  }
  lv_label_set_text(library_count_, count);
  for (size_t index = library_page_start_; index < end; ++index) {
    const LibraryItem &item = items[index];
    lv_obj_t *row = lv_button_create(library_list_);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 50);
    lv_obj_set_style_radius(row, 2, 0);
    lv_obj_set_style_bg_color(row, color(kPanel), 0);
    lv_obj_set_style_bg_color(row, color(0x174148), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_add_event_cb(row, library_row_handler, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(row, library_row_handler, LV_EVENT_LONG_PRESSED, this);
    lv_obj_t *name = make_label(row, item.title.empty() ? "UNKNOWN" : item.title.c_str(),
                                &lv_font_montserrat_18, color(kText));
    lv_obj_set_size(name, 360, 22);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 2, item.subtitle.empty() ? 0 : -9);
    if (!item.subtitle.empty()) {
      lv_obj_t *details = make_label(row, item.subtitle.c_str(), &lv_font_montserrat_14,
                                     color(kMuted));
      lv_obj_set_size(details, 360, 18);
      lv_label_set_long_mode(details, LV_LABEL_LONG_DOT);
      lv_obj_align(details, LV_ALIGN_LEFT_MID, 2, 11);
    }
    lv_obj_t *action = make_label(row,
                                  item.kind == LibraryItemKind::track ? LV_SYMBOL_PLAY
                                                                      : LV_SYMBOL_RIGHT,
                                  &lv_font_montserrat_18, color(kAccent));
    lv_obj_align(action, LV_ALIGN_RIGHT_MID, -4, 0);
  }
  lv_obj_set_state(library_previous_page_, LV_STATE_DISABLED, library_page_start_ == 0);
  lv_obj_set_state(library_next_page_, LV_STATE_DISABLED,
                   end >= items.size());
}

void MusicUi::update_lyrics(const Snapshot &snapshot) {
  const std::string title = snapshot.now_playing.title.empty()
                                ? "LYRICS"
                                : "LYRICS / " + snapshot.now_playing.title;
  lv_label_set_text(lyrics_title_, title.c_str());
  if (!lv_obj_is_hidden(lyrics_overlay_) &&
      snapshot.connection == ConnectionState::online &&
      !snapshot.artwork_track_key.empty() &&
      lyrics_requested_key_ != snapshot.artwork_track_key) {
    lyrics_requested_key_ = snapshot.artwork_track_key;
    client_.enqueue({CommandType::request_lyrics});
    lv_label_set_text(lyrics_text_, "REQUESTING LYRICS...");
    return;
  }
  switch (snapshot.lyrics_state) {
  case LyricsState::loading:
    lv_label_set_text(lyrics_text_, "REQUESTING LYRICS...");
    break;
  case LyricsState::ready:
  case LyricsState::unavailable:
  case LyricsState::failed:
    lv_label_set_text(lyrics_text_, snapshot.lyrics->c_str());
    break;
  case LyricsState::none:
  default:
    lv_label_set_text(lyrics_text_, "OPEN LYRICS TO REQUEST THEM FROM AIMP.");
    break;
  }
}

void MusicUi::create_playlists_page() {
  lv_obj_t *page = lv_obj_create(root_);
  pages_[2] = page;
  lv_obj_remove_style_all(page);
  lv_obj_set_pos(page, 0, 40);
  lv_obj_set_size(page, 480, 234);
  playlist_title_ = make_label(page, "PLAYLISTS", &lv_font_montserrat_22, color(kText));
  lv_obj_set_pos(playlist_title_, 14, 4);
  lv_obj_set_size(playlist_title_, 270, 30);
  lv_label_set_long_mode(playlist_title_, LV_LABEL_LONG_DOT);
  new_playlist_button_ = make_button(page, "+ ESP REMOTE", 154, 42);
  lv_obj_align(new_playlist_button_, LV_ALIGN_TOP_RIGHT, -14, 0);
  lv_obj_add_event_cb(new_playlist_button_, event_handler, LV_EVENT_LONG_PRESSED, this);
  playlist_back_button_ = make_button(page, LV_SYMBOL_LEFT " BACK", 112, 42);
  lv_obj_align(playlist_back_button_, LV_ALIGN_TOP_RIGHT, -14, 0);
  lv_obj_add_flag(playlist_back_button_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(playlist_back_button_, event_handler, LV_EVENT_CLICKED, this);
  playlist_list_ = lv_obj_create(page);
  lv_obj_set_pos(playlist_list_, 8, 42);
  lv_obj_set_size(playlist_list_, 464, 188);
  lv_obj_set_style_bg_opa(playlist_list_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(playlist_list_, 0, 0);
  lv_obj_set_style_pad_all(playlist_list_, 0, 0);
  lv_obj_set_style_pad_row(playlist_list_, 3, 0);
  lv_obj_set_flex_flow(playlist_list_, LV_FLEX_FLOW_COLUMN);
}

void MusicUi::create_more_page() {
  lv_obj_t *page = lv_obj_create(root_);
  pages_[3] = page;
  lv_obj_remove_style_all(page);
  lv_obj_set_pos(page, 0, 40);
  lv_obj_set_size(page, 480, 234);
  lv_obj_t *title = make_label(page, "SETTINGS", &lv_font_montserrat_28, color(kText));
  lv_obj_set_pos(title, 14, 4);
  more_status_ = make_label(page, "OFFLINE", &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_pos(more_status_, 250, 13);
  lv_obj_set_size(more_status_, 214, 18);
  lv_obj_set_style_text_align(more_status_, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *menu = lv_obj_create(page);
  lv_obj_set_pos(menu, 8, 40);
  lv_obj_set_size(menu, 464, 190);
  lv_obj_set_style_bg_opa(menu, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(menu, 0, 0);
  lv_obj_set_style_pad_all(menu, 0, 0);
  lv_obj_set_style_pad_row(menu, 3, 0);
  lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);

  auto add_row = [this, menu](const char *text) {
    lv_obj_t *row = make_button(menu, text, 448, 48);
    lv_obj_t *label = lv_obj_get_child(row, 0);
    lv_obj_set_width(label, 380);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *arrow = make_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_18, color(kAccent));
    lv_obj_set_clickable(arrow, false);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_event_cb(row, event_handler, LV_EVENT_CLICKED, this);
    return row;
  };
  library_button_ = add_row("MEDIA LIBRARY");
  lyrics_button_ = add_row("LYRICS");
  playback_settings_button_ = add_row("PLAYBACK");
  connection_settings_button_ = add_row("CONNECTION");
  display_settings_button_ = add_row("DISPLAY");
  storage_settings_button_ = add_row("STORAGE / ABOUT");
  advanced_settings_button_ = add_row("AIMP CONTROLS");
}

void MusicUi::create_settings_overlays() {
  settings_overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(settings_overlay_);
  lv_obj_set_size(settings_overlay_, 480, 320);
  lv_obj_set_style_bg_color(settings_overlay_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(settings_overlay_, LV_OPA_COVER, 0);
  lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);

  settings_title_ = make_label(settings_overlay_, "PLAYBACK", &lv_font_montserrat_22,
                               color(kText));
  lv_obj_set_pos(settings_title_, 14, 14);
  settings_notice_ = make_label(settings_overlay_, "", &lv_font_montserrat_14,
                                color(0xE04F5F));
  lv_obj_set_pos(settings_notice_, 214, 18);
  lv_obj_set_size(settings_notice_, 130, 18);
  lv_obj_set_style_text_align(settings_notice_, LV_TEXT_ALIGN_RIGHT, 0);
  settings_back_button_ = make_button(settings_overlay_, LV_SYMBOL_LEFT " BACK", 112, 48);
  lv_obj_set_pos(settings_back_button_, 356, 4);
  lv_obj_add_event_cb(settings_back_button_, event_handler, LV_EVENT_CLICKED, this);

  for (auto &panel : settings_panels_) {
    panel = lv_obj_create(settings_overlay_);
    lv_obj_set_pos(panel, 8, 58);
    lv_obj_set_size(panel, 464, 254);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_t *playback = settings_panels_[static_cast<size_t>(SettingsSection::playback)];
  more_shuffle_ = make_button(playback, "SHUFFLE", 208, 52);
  more_repeat_ = make_button(playback, "REPEAT", 208, 52);
  lv_obj_t *stop = make_button(playback, "STOP", 208, 52);
  lv_obj_t *refresh = make_button(playback, "REFRESH FROM AIMP", 208, 52);
  lv_obj_set_pos(more_shuffle_, 8, 10);
  lv_obj_set_pos(more_repeat_, 232, 10);
  lv_obj_set_pos(stop, 8, 78);
  lv_obj_set_pos(refresh, 232, 78);
  lv_obj_add_flag(more_shuffle_, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_flag(more_repeat_, LV_OBJ_FLAG_CHECKABLE);
  for (lv_obj_t *button : {more_shuffle_, more_repeat_, stop, refresh}) {
    lv_obj_add_event_cb(button, event_handler, LV_EVENT_CLICKED, this);
  }
  lv_obj_t *playback_help = make_label(
      playback, "Long-press the main play button to stop.\nVolume and seek stay on Now Playing.",
      &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_pos(playback_help, 8, 154);
  lv_obj_set_width(playback_help, 420);

  lv_obj_t *connection = settings_panels_[static_cast<size_t>(SettingsSection::connection)];
  lv_obj_set_style_pad_row(connection, 3, 0);
  lv_obj_set_flex_flow(connection, LV_FLEX_FLOW_COLUMN);
  auto setting_row = [this, connection](const char *title, lv_obj_t **value_label) {
    lv_obj_t *row = lv_button_create(connection);
    lv_obj_set_size(row, 440, 50);
    lv_obj_set_style_radius(row, 3, 0);
    lv_obj_set_style_bg_color(row, color(kPanel), 0);
    lv_obj_set_style_bg_color(row, color(0x174148), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_t *name = make_label(row, title, &lv_font_montserrat_14, color(kMuted));
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 2, -10);
    *value_label = make_label(row, "", &lv_font_montserrat_18, color(kText));
    lv_obj_set_size(*value_label, 380, 22);
    lv_label_set_long_mode(*value_label, LV_LABEL_LONG_DOT);
    lv_obj_align(*value_label, LV_ALIGN_LEFT_MID, 2, 10);
    lv_obj_t *arrow = make_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_18, color(kAccent));
    lv_obj_set_clickable(arrow, false);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_add_event_cb(row, event_handler, LV_EVENT_CLICKED, this);
    return row;
  };
  wifi_ssid_button_ = setting_row("WI-FI NETWORK", &wifi_ssid_value_);
  wifi_password_button_ = setting_row("WI-FI PASSWORD", &wifi_password_value_);
  server_host_button_ = setting_row("AIMP SERVER", &server_host_value_);
  server_port_button_ = setting_row("AIMP PORT", &server_port_value_);
  server_password_button_ = setting_row("AIMP PASSWORD", &server_password_value_);
  lv_obj_t *restart_note = make_label(connection, "CONNECTION CHANGES APPLY AFTER RESTART",
                                      &lv_font_montserrat_14, color(kAccent));
  lv_obj_set_style_pad_all(restart_note, 8, 0);

  lv_obj_t *display = settings_panels_[static_cast<size_t>(SettingsSection::display)];
  lv_obj_t *brightness_label = make_label(display, "BRIGHTNESS", &lv_font_montserrat_18,
                                          color(kText));
  lv_obj_set_pos(brightness_label, 8, 12);
  brightness_value_ = make_label(display, "82%", &lv_font_montserrat_18, color(kAccent));
  lv_obj_set_pos(brightness_value_, 372, 12);
  lv_obj_set_width(brightness_value_, 62);
  lv_obj_set_style_text_align(brightness_value_, LV_TEXT_ALIGN_RIGHT, 0);
  brightness_slider_ = lv_slider_create(display);
  lv_obj_set_pos(brightness_slider_, 8, 52);
  lv_obj_set_size(brightness_slider_, 426, 12);
  lv_obj_set_ext_click_area(brightness_slider_, 16);
  lv_slider_set_range(brightness_slider_, 5, 100);
  lv_obj_set_style_bg_color(brightness_slider_, color(0x28303D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(brightness_slider_, color(kAccent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightness_slider_, color(kText), LV_PART_KNOB);
  lv_obj_add_event_cb(brightness_slider_, event_handler, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(brightness_slider_, event_handler, LV_EVENT_RELEASED, this);
  auto display_value_row = [this, display](const char *title, const char *value, int y,
                                            lv_obj_t **value_label) {
    lv_obj_t *row = make_button(display, title, 426, 52);
    lv_obj_set_pos(row, 8, y);
    lv_obj_t *name = lv_obj_get_child(row, 0);
    lv_obj_set_width(name, 210);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, 0);
    *value_label = make_label(row, value, &lv_font_montserrat_18, color(kAccent));
    lv_obj_set_width(*value_label, 170);
    lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(*value_label, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_clickable(*value_label, false);
    lv_obj_add_event_cb(row, event_handler, LV_EVENT_CLICKED, this);
    return row;
  };
  auto_blank_button_ = display_value_row("SCREEN OFF", "5 MIN", 92, &auto_blank_value_);
  flip_button_ = display_value_row("FLIP SCREEN", "OFF", 154, &flip_value_);
  lv_obj_t *wake_help = make_label(display, "TOUCH ANYWHERE ON THE DARK SCREEN TO WAKE",
                                   &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_pos(wake_help, 8, 218);

  lv_obj_t *storage = settings_panels_[static_cast<size_t>(SettingsSection::storage)];
  lv_obj_set_style_pad_all(storage, 10, 0);
  lv_obj_t *storage_text = make_label(
      storage,
      "LARGE PLAYLIST CACHE\n"
      "Indexes and artwork persist on microSD when available. A matching content hash reuses the index after "
      "reboot; changed content rebuilds safely. AIMP has no revision or delta paging, so the response is still "
      "checked.\n\nAIMP CONTROLS\n"
      "Equalizer, sleep timer and confirmed computer actions are available from their own settings page when "
      "the connected plug-in advertises support.",
      &lv_font_montserrat_14, color(kText));
  // The connected capability label wraps to two lines, so the NVS status and
  // this text start below the second line instead of overlapping it.
  lv_obj_set_pos(storage_text, 10, 64);
  lv_obj_set_width(storage_text, 420);
  lv_label_set_long_mode(storage_text, LV_LABEL_LONG_WRAP);
  capabilities_value_ = make_label(storage, "CAPABILITIES: WAITING FOR AIMP",
                                   &lv_font_montserrat_14, color(kAccent));
  lv_obj_set_pos(capabilities_value_, 10, 4);
  lv_obj_set_width(capabilities_value_, 420);
  settings_storage_value_ = make_label(storage, "SETTINGS STORAGE: NVS OK",
                                       &lv_font_montserrat_14, color(kAccent));
  lv_obj_set_pos(settings_storage_value_, 10, 44);
  lv_obj_set_width(settings_storage_value_, 420);

  lv_obj_t *advanced_panel =
      settings_panels_[static_cast<size_t>(SettingsSection::advanced)];
  lv_obj_set_style_pad_row(advanced_panel, 4, 0);
  lv_obj_set_flex_flow(advanced_panel, LV_FLEX_FLOW_COLUMN);
  advanced_status_ = make_label(advanced_panel, "WAITING FOR AIMP CAPABILITIES",
                                &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_width(advanced_status_, 430);
  lv_label_set_long_mode(advanced_status_, LV_LABEL_LONG_WRAP);
  equalizer_enable_button_ = lv_obj_create(advanced_panel);
  lv_obj_set_size(equalizer_enable_button_, 440, 52);
  lv_obj_set_style_radius(equalizer_enable_button_, 4, 0);
  lv_obj_set_style_bg_color(equalizer_enable_button_, color(kPanel), 0);
  lv_obj_set_style_border_width(equalizer_enable_button_, 0, 0);
  lv_obj_set_style_pad_all(equalizer_enable_button_, 0, 0);
  lv_obj_set_clickable(equalizer_enable_button_, false);
  lv_obj_set_scrollable(equalizer_enable_button_, false);
  lv_obj_t *eq_name = make_label(equalizer_enable_button_, "EQUALIZER STATUS",
                                 &lv_font_montserrat_18, color(kText));
  lv_obj_set_width(eq_name, 250);
  lv_obj_set_style_text_align(eq_name, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(eq_name, LV_ALIGN_LEFT_MID, 4, 0);
  equalizer_enable_value_ = make_label(equalizer_enable_button_, "WAITING",
                                       &lv_font_montserrat_18, color(kAccent));
  lv_obj_set_width(equalizer_enable_value_, 150);
  lv_obj_set_style_text_align(equalizer_enable_value_, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(equalizer_enable_value_, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_clickable(equalizer_enable_value_, false);
  equalizer_edit_button_ = make_button(advanced_panel, "ADJUST EQUALIZER", 440, 48);
  lv_obj_add_event_cb(equalizer_edit_button_, event_handler, LV_EVENT_CLICKED, this);

  sleep_timer_button_ = make_button(advanced_panel, "SLEEP TIMER", 440, 52);
  lv_obj_t *sleep_name = lv_obj_get_child(sleep_timer_button_, 0);
  lv_obj_set_width(sleep_name, 220);
  lv_obj_set_style_text_align(sleep_name, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(sleep_name, LV_ALIGN_LEFT_MID, 4, 0);
  sleep_timer_value_ = make_label(sleep_timer_button_, "OFF", &lv_font_montserrat_18,
                                  color(kAccent));
  lv_obj_set_width(sleep_timer_value_, 190);
  lv_obj_set_style_text_align(sleep_timer_value_, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(sleep_timer_value_, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_clickable(sleep_timer_value_, false);
  lv_obj_add_event_cb(sleep_timer_button_, event_handler, LV_EVENT_CLICKED, this);

  power_title_ = make_label(advanced_panel, "COMPUTER / CONFIRMATION REQUIRED",
                            &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_style_pad_top(power_title_, 8, 0);
  static constexpr std::array<const char *, 3> kPowerLabels{
      "SLEEP COMPUTER", "RESTART COMPUTER", "SHUT DOWN COMPUTER"};
  for (size_t index = 0; index < power_buttons_.size(); ++index) {
    power_buttons_[index] = make_button(advanced_panel, kPowerLabels[index], 440, 48);
    lv_obj_add_event_cb(power_buttons_[index], event_handler, LV_EVENT_CLICKED, this);
  }

  setting_editor_ = lv_obj_create(root_);
  lv_obj_remove_style_all(setting_editor_);
  lv_obj_set_size(setting_editor_, 480, 320);
  lv_obj_set_style_bg_color(setting_editor_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(setting_editor_, LV_OPA_COVER, 0);
  lv_obj_add_flag(setting_editor_, LV_OBJ_FLAG_HIDDEN);
  setting_editor_title_ = make_label(setting_editor_, "EDIT SETTING", &lv_font_montserrat_22,
                                     color(kText));
  lv_obj_set_pos(setting_editor_title_, 12, 10);
  setting_editor_textarea_ = lv_textarea_create(setting_editor_);
  lv_obj_set_pos(setting_editor_textarea_, 12, 48);
  lv_obj_set_size(setting_editor_textarea_, 332, 48);
  lv_textarea_set_one_line(setting_editor_textarea_, true);
  lv_obj_set_style_text_font(setting_editor_textarea_, &lv_font_montserrat_18, 0);
  lv_obj_set_style_bg_color(setting_editor_textarea_, color(kPanel), 0);
  lv_obj_set_style_text_color(setting_editor_textarea_, color(kText), 0);
  lv_obj_t *cancel = make_button(setting_editor_, "CANCEL", 116, 48);
  lv_obj_set_pos(cancel, 352, 48);
  lv_obj_add_event_cb(cancel, [](lv_event_t *event) {
    auto *ui = static_cast<MusicUi *>(lv_event_get_user_data(event));
    if (ui != nullptr) {
      ui->finish_setting_edit(false);
    }
  }, LV_EVENT_CLICKED, this);
  setting_editor_message_ = make_label(setting_editor_, "PRESS ENTER TO SAVE",
                                       &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_pos(setting_editor_message_, 14, 105);
  setting_editor_keyboard_ = lv_keyboard_create(setting_editor_);
  lv_obj_align(setting_editor_keyboard_, LV_ALIGN_TOP_LEFT, 8, 128);
  lv_obj_set_size(setting_editor_keyboard_, 464, 184);
  lv_keyboard_set_textarea(setting_editor_keyboard_, setting_editor_textarea_);
  // LVGL keeps one map table for all keyboards, so search also gains these keys.
  lv_keyboard_set_map(setting_editor_keyboard_, LV_KEYBOARD_MODE_SPECIAL, kSymbolKeyboardMap,
                      kSymbolKeyboardControls);
  lv_obj_set_style_text_font(setting_editor_keyboard_, &lv_font_montserrat_18, LV_PART_ITEMS);
  // As for search: READY from both Enter and the checkmark reaches the textarea.
  lv_obj_add_event_cb(setting_editor_textarea_, event_handler, LV_EVENT_READY, this);
  lv_obj_add_event_cb(setting_editor_keyboard_, event_handler, LV_EVENT_CANCEL, this);

  blank_overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(blank_overlay_);
  lv_obj_set_size(blank_overlay_, 480, 320);
  lv_obj_set_style_bg_color(blank_overlay_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(blank_overlay_, LV_OPA_COVER, 0);
  lv_obj_add_flag(blank_overlay_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(blank_overlay_, LV_OBJ_FLAG_HIDDEN);
  refresh_setting_labels();
}

void MusicUi::create_equalizer_overlay() {
  equalizer_overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(equalizer_overlay_);
  lv_obj_set_size(equalizer_overlay_, 480, 320);
  lv_obj_set_style_bg_color(equalizer_overlay_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(equalizer_overlay_, LV_OPA_COVER, 0);
  lv_obj_add_flag(equalizer_overlay_, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *title = make_label(equalizer_overlay_, "EQUALIZER", &lv_font_montserrat_22,
                               color(kText));
  lv_obj_set_pos(title, 14, 14);
  equalizer_overlay_toggle_ = make_label(equalizer_overlay_, "OFF",
                                         &lv_font_montserrat_18, color(kAccent));
  lv_obj_set_pos(equalizer_overlay_toggle_, 260, 17);
  lv_obj_set_width(equalizer_overlay_toggle_, 80);
  lv_obj_set_style_text_align(equalizer_overlay_toggle_, LV_TEXT_ALIGN_RIGHT, 0);
  equalizer_close_button_ = make_button(equalizer_overlay_, "CLOSE", 112, 48);
  lv_obj_set_pos(equalizer_close_button_, 356, 4);
  lv_obj_add_event_cb(equalizer_close_button_, event_handler, LV_EVENT_CLICKED, this);

  equalizer_list_ = lv_obj_create(equalizer_overlay_);
  lv_obj_set_pos(equalizer_list_, 8, 58);
  lv_obj_set_size(equalizer_list_, 464, 254);
  lv_obj_set_style_bg_opa(equalizer_list_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(equalizer_list_, 0, 0);
  lv_obj_set_style_pad_all(equalizer_list_, 4, 0);
  lv_obj_set_style_pad_row(equalizer_list_, 4, 0);
  lv_obj_set_flex_flow(equalizer_list_, LV_FLEX_FLOW_COLUMN);

  power_confirm_overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(power_confirm_overlay_);
  lv_obj_set_size(power_confirm_overlay_, 480, 320);
  lv_obj_set_style_bg_color(power_confirm_overlay_, color(kBackground), 0);
  lv_obj_set_style_bg_opa(power_confirm_overlay_, LV_OPA_COVER, 0);
  lv_obj_add_flag(power_confirm_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *warning = make_label(power_confirm_overlay_, "CONFIRM COMPUTER ACTION",
                                 &lv_font_montserrat_22, color(0xE04F5F));
  lv_obj_set_pos(warning, 20, 30);
  power_confirm_message_ = make_label(
      power_confirm_overlay_, "", &lv_font_montserrat_18, color(kText));
  lv_obj_set_pos(power_confirm_message_, 20, 92);
  lv_obj_set_width(power_confirm_message_, 440);
  lv_label_set_long_mode(power_confirm_message_, LV_LABEL_LONG_WRAP);
  power_cancel_button_ = make_button(power_confirm_overlay_, "CANCEL", 196, 56);
  lv_obj_set_pos(power_cancel_button_, 32, 228);
  power_confirm_button_ = make_button(power_confirm_overlay_, "CONFIRM", 196, 56);
  lv_obj_set_pos(power_confirm_button_, 252, 228);
  lv_obj_set_style_bg_color(power_confirm_button_, color(0x7A2431), 0);
  lv_obj_set_style_bg_color(power_confirm_button_, color(0xA73545), LV_STATE_PRESSED);
  lv_obj_add_event_cb(power_cancel_button_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(power_confirm_button_, event_handler, LV_EVENT_CLICKED, this);
}

void MusicUi::rebuild_equalizer(const Snapshot &snapshot) {
  if (equalizer_list_ == nullptr) {
    return;
  }
  lv_obj_clean(equalizer_list_);
  equalizer_gain_sliders_.clear();
  equalizer_gain_values_.clear();
  if (!advanced::equalizer_mutation_allowed(
          snapshot.advanced_availability.equalizer, snapshot.equalizer)) {
    const char *message =
        snapshot.advanced_availability.equalizer ==
                advanced::StateAvailability::unavailable
            ? "AIMP DID NOT RETURN EQUALIZER STATE"
            : "WAITING FOR EQUALIZER STATE FROM AIMP";
    lv_obj_t *waiting =
        make_label(equalizer_list_, message, &lv_font_montserrat_18, color(kMuted));
    lv_obj_set_style_pad_all(waiting, 16, 0);
    return;
  }
  auto add_gain = [this](const char *name, uint16_t value) {
    lv_obj_t *row = lv_obj_create(equalizer_list_);
    lv_obj_set_size(row, 440, 52);
    lv_obj_set_style_bg_color(row, color(kPanel), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 3, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_t *label = make_label(row, name, &lv_font_montserrat_14, color(kMuted));
    lv_obj_set_pos(label, 8, 17);
    lv_obj_set_width(label, 66);
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_pos(slider, 76, 20);
    lv_obj_set_size(slider, 272, 10);
    // The 10 px visual track keeps the Zune-like lightness, while the extended
    // area provides a full 46 px finger target in both axes.
    lv_obj_set_ext_click_area(slider, 18);
    lv_slider_set_range(slider, 0, 300);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, color(0x28303D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, color(kAccentBlue), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, color(kText), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, event_handler, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(slider, event_handler, LV_EVENT_RELEASED, this);
    char db[16];
    format_gain(db, value);
    lv_obj_t *value_label = make_label(row, db, &lv_font_montserrat_14, color(kText));
    lv_obj_set_pos(value_label, 354, 17);
    lv_obj_set_width(value_label, 78);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    equalizer_gain_sliders_.push_back(slider);
    equalizer_gain_values_.push_back(value_label);
  };
  add_gain("PREAMP", snapshot.equalizer.preamp_tenths_offset);
  for (const auto &band : snapshot.equalizer.bands) {
    char frequency[16];
    format_band(frequency, band.frequency_hz);
    add_gain(frequency, band.gain_tenths_offset);
  }
}

void MusicUi::sync_equalizer(const Snapshot &snapshot) {
  if (snapshot.equalizer_revision == rendered_equalizer_revision_) {
    return;
  }
  bool slider_pressed = false;
  for (lv_obj_t *slider : equalizer_gain_sliders_) {
    slider_pressed = slider_pressed || lv_obj_has_state(slider, LV_STATE_PRESSED);
  }
  const size_t sliders = advanced::equalizer_slider_count(
      snapshot.advanced_availability.equalizer, snapshot.equalizer);
  switch (advanced::equalizer_refresh_action(equalizer_gain_sliders_.size(), sliders,
                                             slider_pressed)) {
  case advanced::EqualizerRefresh::defer:
    return;
  case advanced::EqualizerRefresh::rebuild:
    rebuild_equalizer(snapshot);
    break;
  case advanced::EqualizerRefresh::update_in_place:
    for (size_t index = 0; index < sliders; ++index) {
      lv_obj_t *slider = equalizer_gain_sliders_[index];
      if (lv_obj_has_state(slider, LV_STATE_PRESSED)) {
        continue; // Keep the drag; its release sends the user's value.
      }
      const uint16_t value = index == 0
                                 ? snapshot.equalizer.preamp_tenths_offset
                                 : snapshot.equalizer.bands[index - 1].gain_tenths_offset;
      lv_slider_set_value(slider, value, LV_ANIM_OFF);
      char text[16];
      format_gain(text, value);
      lv_label_set_text(equalizer_gain_values_[index], text);
      if (index > 0) {
        format_band(text, snapshot.equalizer.bands[index - 1].frequency_hz);
        lv_label_set_text(lv_obj_get_child(lv_obj_get_parent(slider), 0), text);
      }
    }
    break;
  }
  rendered_equalizer_revision_ = snapshot.equalizer_revision;
}

void MusicUi::show_settings_section(SettingsSection section) {
  static constexpr std::array<const char *, 5> kTitles{
      "PLAYBACK", "CONNECTION", "DISPLAY", "STORAGE / ABOUT", "AIMP CONTROLS"};
  for (size_t i = 0; i < std::size(settings_panels_); ++i) {
    lv_obj_set_hidden(settings_panels_[i], i != static_cast<size_t>(section));
  }
  lv_label_set_text(settings_title_, kTitles[static_cast<size_t>(section)]);
  lv_label_set_text(settings_notice_, settings_.persistence_available() ? "" : "NVS UNAVAILABLE");
  refresh_setting_labels();
  lv_obj_remove_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(settings_overlay_);
}

void MusicUi::refresh_setting_labels() {
  const DeviceSettingsData data = settings_.snapshot();
  lv_label_set_text(wifi_ssid_value_, data.wifi_ssid.c_str());
  lv_label_set_text(wifi_password_value_, data.wifi_password.empty() ? "NOT SET" : "********");
  lv_label_set_text(server_host_value_, data.server_host.c_str());
  char port[8];
  std::snprintf(port, sizeof(port), "%u", static_cast<unsigned>(data.server_port));
  lv_label_set_text(server_port_value_, port);
  lv_label_set_text(server_password_value_,
                    data.server_password.empty() ? "NOT SET" : "********");
  lv_slider_set_value(brightness_slider_, data.brightness, LV_ANIM_OFF);
  char brightness[8];
  std::snprintf(brightness, sizeof(brightness), "%u%%", data.brightness);
  lv_label_set_text(brightness_value_, brightness);
  const char *blank = "OFF";
  switch (data.auto_blank_seconds) {
  case 30:
    blank = "30 SEC";
    break;
  case 60:
    blank = "1 MIN";
    break;
  case 300:
    blank = "5 MIN";
    break;
  case 900:
    blank = "15 MIN";
    break;
  default:
    break;
  }
  lv_label_set_text(auto_blank_value_, blank);
  lv_label_set_text(flip_value_, data.display_flipped ? "ON" : "OFF");
  const bool persistent = settings_.persistence_available();
  lv_label_set_text(settings_storage_value_, persistent ? "SETTINGS STORAGE: NVS OK"
                                                        : "SETTINGS STORAGE: UNAVAILABLE");
  lv_obj_set_style_text_color(settings_storage_value_,
                              persistent ? color(kAccent) : color(0xE04F5F), 0);
}

void MusicUi::update_advanced_controls(const Snapshot &snapshot) {
  const bool known = snapshot.capabilities_known;
  const bool has_equalizer =
      known && (snapshot.server_capabilities & advanced::kEqualizerCapability) != 0;
  const bool has_sleep =
      known && (snapshot.server_capabilities & advanced::kSleepTimerCapability) != 0;
  const bool has_power =
      known && (snapshot.server_capabilities & advanced::kComputerPowerCapability) != 0;
  const bool advanced_unavailable =
      (has_equalizer &&
       (snapshot.advanced_availability.equalizer ==
            advanced::StateAvailability::unavailable ||
        snapshot.advanced_availability.equalizer_presets ==
            advanced::StateAvailability::unavailable)) ||
      (has_sleep && snapshot.advanced_availability.sleep_timer ==
                        advanced::StateAvailability::unavailable);
  if (!known) {
    lv_label_set_text(advanced_status_, "WAITING FOR AIMP CAPABILITIES");
  } else if (!has_equalizer && !has_sleep && !has_power) {
    lv_label_set_text(advanced_status_, "THE CONNECTED PLUG-IN REPORTS NO ADVANCED CONTROLS");
  } else if (advanced_unavailable) {
    lv_label_set_text(advanced_status_,
                      "AIMP DID NOT RETURN STATE FOR AN ADVERTISED CONTROL");
  } else {
    lv_label_set_text(advanced_status_, "ONLY CONTROLS ADVERTISED BY AIMP ARE SHOWN");
  }
  lv_obj_set_hidden(equalizer_enable_button_, !has_equalizer);
  lv_obj_set_hidden(equalizer_edit_button_, !has_equalizer);
  lv_obj_set_hidden(sleep_timer_button_, !has_sleep);
  lv_obj_set_hidden(power_title_, !has_power);
  for (lv_obj_t *button : power_buttons_) {
    lv_obj_set_hidden(button, !has_power);
  }

  if (has_equalizer) {
    const char *state =
        snapshot.advanced_availability.equalizer ==
                advanced::StateAvailability::unavailable
            ? "UNAVAILABLE"
            : snapshot.equalizer.known ? (snapshot.equalizer.enabled ? "ON" : "OFF")
                                       : "LOADING";
    lv_label_set_text(equalizer_enable_value_, state);
    lv_label_set_text(equalizer_overlay_toggle_, state);
    lv_obj_set_state(equalizer_edit_button_, LV_STATE_DISABLED,
                     !advanced::equalizer_mutation_allowed(
                         snapshot.advanced_availability.equalizer,
                         snapshot.equalizer));
  }

  if (has_sleep) {
    char timer[32];
    if (snapshot.advanced_availability.sleep_timer ==
        advanced::StateAvailability::unavailable) {
      std::snprintf(timer, sizeof(timer), "UNAVAILABLE");
    } else if (!snapshot.sleep_timer.known) {
      std::snprintf(
          timer, sizeof(timer), "LOADING");
    } else if (!snapshot.sleep_timer.enabled) {
      std::snprintf(timer, sizeof(timer), "OFF");
    } else if (snapshot.sleep_timer.trigger == 2) {
      std::snprintf(timer, sizeof(timer), "END OF TRACK");
    } else {
      const uint32_t minutes = (snapshot.sleep_timer.seconds_remaining + 59) / 60;
      std::snprintf(timer, sizeof(timer), "%u MIN LEFT", static_cast<unsigned>(minutes));
    }
    lv_label_set_text(sleep_timer_value_, timer);
    lv_obj_set_state(sleep_timer_button_, LV_STATE_DISABLED,
                     !advanced::sleep_timer_mutation_allowed(
                         snapshot.advanced_availability.sleep_timer,
                         snapshot.sleep_timer));
  }

  sync_equalizer(snapshot);
  rendered_sleep_timer_revision_ = snapshot.sleep_timer_revision;
}

void MusicUi::end_advanced_demo() {
  if (!advanced_demo_active_) {
    return;
  }
  advanced_demo_active_ = false;
  pending_power_action_ = -1;
  update_advanced_controls(snapshot_);
}

void MusicUi::begin_setting_edit(SettingField field, const char *title, bool password) {
  const DeviceSettingsData data = settings_.snapshot();
  const std::string *value = nullptr;
  std::string port;
  size_t maximum = 127;
  switch (field) {
  case SettingField::wifi_ssid:
    value = &data.wifi_ssid;
    maximum = 32;
    break;
  case SettingField::wifi_password:
    value = &data.wifi_password;
    maximum = 64;
    break;
  case SettingField::server_host:
    value = &data.server_host;
    break;
  case SettingField::server_port:
    port = std::to_string(data.server_port);
    value = &port;
    maximum = 5;
    break;
  case SettingField::server_password:
    value = &data.server_password;
    break;
  case SettingField::none:
    return;
  }
  editing_field_ = field;
  lv_label_set_text(setting_editor_title_, title);
  lv_label_set_text(setting_editor_message_, "PRESS ENTER TO SAVE");
  lv_obj_set_style_text_color(setting_editor_message_, color(kMuted), 0);
  lv_textarea_set_password_mode(setting_editor_textarea_, password);
  lv_textarea_set_password_show_time(setting_editor_textarea_, 0);
  lv_textarea_set_max_length(setting_editor_textarea_, maximum);
  lv_textarea_set_text(setting_editor_textarea_, value->c_str());
  lv_obj_remove_flag(setting_editor_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(setting_editor_);
  lv_obj_send_event(setting_editor_textarea_, LV_EVENT_FOCUSED, nullptr);
}

void MusicUi::finish_setting_edit(bool save) {
  if (!save) {
    editing_field_ = SettingField::none;
    lv_obj_add_flag(setting_editor_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const char *text = lv_textarea_get_text(setting_editor_textarea_);
  const std::string value = text == nullptr ? "" : text;
  bool valid = false;
  bool stored = false;
  switch (editing_field_) {
  case SettingField::wifi_ssid:
    valid = DeviceSettings::valid_wifi_ssid(value);
    stored = valid && settings_.set_wifi_ssid(value);
    break;
  case SettingField::wifi_password:
    valid = DeviceSettings::valid_wifi_password(value);
    stored = valid && settings_.set_wifi_password(value);
    break;
  case SettingField::server_host:
    valid = DeviceSettings::valid_server_host(value);
    stored = valid && settings_.set_server_host(value);
    break;
  case SettingField::server_port: {
    char *end = nullptr;
    const unsigned long port = std::strtoul(value.c_str(), &end, 10);
    valid = end != value.c_str() && end != nullptr && *end == '\0' && port <= UINT16_MAX &&
            port != 0;
    stored = valid && settings_.set_server_port(static_cast<uint16_t>(port));
    break;
  }
  case SettingField::server_password:
    valid = DeviceSettings::valid_server_password(value);
    stored = valid && settings_.set_server_password(value);
    break;
  case SettingField::none:
    break;
  }
  if (!stored) {
    lv_label_set_text(setting_editor_message_, valid ? "SAVE FAILED - SETTINGS STORAGE UNAVAILABLE"
                                                     : "INVALID VALUE - CHECK FORMAT OR PORT");
    lv_obj_set_style_text_color(setting_editor_message_, color(0xE04F5F), 0);
    refresh_setting_labels();
    return;
  }
  editing_field_ = SettingField::none;
  lv_obj_add_flag(setting_editor_, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(settings_notice_, "");
  refresh_setting_labels();
}

void MusicUi::set_display_blank(bool blank) {
  if (blank == display_blank_) {
    return;
  }
  display_blank_ = blank;
  if (blank) {
    lv_obj_remove_flag(blank_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(blank_overlay_);
    brightness_callback_(0);
  } else {
    brightness_callback_(settings_.snapshot().brightness);
    lv_obj_add_flag(blank_overlay_, LV_OBJ_FLAG_HIDDEN);
  }
}

void MusicUi::show_artwork(std::shared_ptr<const ArtworkBitmap> artwork) {
  artwork_linger_deadline_us_ = 0;
  if (artwork == nullptr || artwork->pixels.empty() || artwork->width > 144 ||
      artwork->height > 144 || artwork->stride < artwork->width * 3u) {
    show_artwork_fallback();
    return;
  }
  // Point LVGL at the new pixels before releasing the previous bitmap.
  displayed_artwork_ = std::move(artwork);
  artwork_descriptor_ = {};
  artwork_descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
  artwork_descriptor_.header.cf = LV_COLOR_FORMAT_RGB888;
  artwork_descriptor_.header.w = displayed_artwork_->width;
  artwork_descriptor_.header.h = displayed_artwork_->height;
  artwork_descriptor_.header.stride = displayed_artwork_->stride;
  artwork_descriptor_.data_size = displayed_artwork_->pixels.size();
  artwork_descriptor_.data = displayed_artwork_->pixels.data();
  lv_image_set_src(art_image_, nullptr);
  lv_image_set_src(art_image_, &artwork_descriptor_);
  lv_obj_center(art_image_);
  lv_obj_clear_flag(art_image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(art_fallback_, LV_OBJ_FLAG_HIDDEN);
}

void MusicUi::show_artwork_fallback() {
  artwork_linger_deadline_us_ = 0;
  lv_image_set_src(art_image_, nullptr);
  lv_obj_add_flag(art_image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(art_fallback_, LV_OBJ_FLAG_HIDDEN);
  displayed_artwork_.reset();
}

void MusicUi::poll_artwork_linger(int64_t now_us) {
  if (artwork_linger_deadline_us_ != 0 && now_us >= artwork_linger_deadline_us_) {
    show_artwork_fallback();
  }
}

void MusicUi::poll_display_timeout(int64_t now_us) {
  lv_indev_t *input = nullptr;
  bool pressed = false;
  while ((input = lv_indev_get_next(input)) != nullptr) {
    if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER &&
        lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) {
      pressed = true;
      if (!touch_was_pressed_) {
        last_interaction_us_ = now_us;
        if (display_blank_) {
          set_display_blank(false);
          lv_indev_wait_release(input);
        }
      }
    }
  }
  touch_was_pressed_ = pressed;
  const uint32_t timeout = settings_.snapshot().auto_blank_seconds;
  if (!display_blank_ && timeout != 0 &&
      now_us - last_interaction_us_ >= static_cast<int64_t>(timeout) * 1'000'000) {
    set_display_blank(true);
  }
}

void MusicUi::show_page(Page page) {
  page_ = page;
  for (size_t i = 0; i < 4; ++i) {
    if (i == static_cast<size_t>(page)) {
      lv_obj_clear_flag(pages_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(nav_buttons_[i], color(kAccent), 0);
      lv_obj_set_style_bg_opa(nav_buttons_[i], LV_OPA_30, 0);
      lv_obj_set_style_text_color(lv_obj_get_child(nav_buttons_[i], 0), color(kAccent), 0);
    } else {
      lv_obj_add_flag(pages_[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_opa(nav_buttons_[i], LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(lv_obj_get_child(nav_buttons_[i], 0), color(kMuted), 0);
    }
  }
}

void MusicUi::update(const Snapshot &snapshot) {
  snapshot_ = snapshot;
  update_advanced_controls(snapshot);

  if (capabilities_value_ != nullptr) {
    char capabilities[112];
    if (snapshot.capabilities_known) {
      std::snprintf(capabilities, sizeof(capabilities),
                    "SERVER CAPS 0x%016llX\nEQ %s   SLEEP %s   POWER %s",
                    static_cast<unsigned long long>(snapshot.server_capabilities),
                    (snapshot.server_capabilities & (1ULL << 21)) != 0 ? "YES" : "NO",
                    (snapshot.server_capabilities & (1ULL << 22)) != 0 ? "YES" : "NO",
                    (snapshot.server_capabilities & (1ULL << 17)) != 0 ? "YES" : "NO");
    } else {
      std::snprintf(capabilities, sizeof(capabilities), "CAPABILITIES: WAITING FOR AIMP");
    }
    lv_label_set_text(capabilities_value_, capabilities);
  }
  const bool online = snapshot.connection == ConnectionState::online;
  lv_label_set_text(status_label_, connection_text(snapshot.connection));
  lv_obj_set_style_bg_color(status_dot_, online ? color(kAccent) : color(0xE04F5F), 0);
  lv_label_set_text(more_status_, snapshot.status.c_str());

  const std::string title = snapshot.now_playing.title.empty() ? "NOTHING PLAYING"
                                                               : snapshot.now_playing.title;
  const std::string artist = snapshot.now_playing.artist.empty() ? "AIMP"
                                                                  : snapshot.now_playing.artist;
  const std::string album = snapshot.now_playing.album.empty()
                                ? (snapshot.now_playing.title.empty() ? "WAITING FOR A TRACK"
                                                                      : "AIMP PLAYBACK")
                                : snapshot.now_playing.album;
  lv_label_set_text(title_label_, title.c_str());
  lv_label_set_text(artist_label_, artist.c_str());
  lv_label_set_text(album_label_, album.c_str());
  if (snapshot.artwork_revision != rendered_artwork_revision_) {
    if (snapshot.artwork != nullptr) {
      show_artwork(snapshot.artwork);
    } else if (snapshot.artwork_state == ArtworkState::loading &&
               displayed_artwork_ != nullptr) {
      // Keep the previous cover while the next one loads; consecutive tracks
      // on one album would otherwise flash the blank tile.
      // Each newly selected track (each revision) gets its own grace period.
      artwork_linger_deadline_us_ = esp_timer_get_time() + kArtworkLingerUs;
    } else {
      show_artwork_fallback();
    }
    rendered_artwork_revision_ = snapshot.artwork_revision;
  }
  lv_label_set_text(play_icon_, snapshot.playback == PlaybackState::playing ? LV_SYMBOL_PAUSE
                                                                            : LV_SYMBOL_PLAY);
  const int32_t duration_deciseconds =
      std::max<int32_t>(1, snapshot.now_playing.duration_ms / 100);
  lv_slider_set_range(progress_slider_, 0, duration_deciseconds);
  update_timeline(esp_timer_get_time(), true);
  const int volume_percent = (static_cast<uint32_t>(snapshot.volume) * 100) / 65535;
  if (volume_slider_ != nullptr && !lv_slider_is_dragged(volume_slider_)) {
    lv_slider_set_value(volume_slider_, volume_percent, LV_ANIM_OFF);
  }
  if (!lv_slider_is_dragged(main_volume_slider_)) {
    lv_slider_set_value(main_volume_slider_, volume_percent, LV_ANIM_OFF);
  }
  char volume_text[16];
  std::snprintf(volume_text, sizeof(volume_text), "VOL %d%%", volume_percent);
  lv_label_set_text(main_volume_label_, volume_text);

  auto apply_toggle = [](lv_obj_t *object, bool checked) {
    if (checked) {
      lv_obj_add_state(object, LV_STATE_CHECKED);
    } else {
      lv_obj_remove_state(object, LV_STATE_CHECKED);
    }
  };
  apply_toggle(shuffle_button_, snapshot.shuffle);
  apply_toggle(more_shuffle_, snapshot.shuffle);
  apply_toggle(repeat_button_, snapshot.repeat != 0);
  apply_toggle(more_repeat_, snapshot.repeat != 0);
  const std::string queue_title = snapshot.queue_indexing
                                      ? snapshot.status
                                  : snapshot.queue_filter_active
                                      ? snapshot.queue_filter_label
                                      : snapshot.active_playlist_name;
  const bool cached = snapshot.queue_from_persisted_cache && !snapshot.queue_indexing;
  lv_obj_set_pos(queue_title_, 14, cached ? 18 : 8);
  if (cached) {
    lv_obj_remove_flag(queue_cache_label_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(queue_cache_label_, LV_OBJ_FLAG_HIDDEN);
  }
  lv_label_set_text(queue_title_, queue_title.c_str());
  lv_obj_set_state(queue_filter_button_, LV_STATE_DISABLED,
                   !snapshot.queue_index_ready || snapshot.queue_indexing);
  if (snapshot.queue_revision != rendered_queue_revision_ ||
      snapshot.queue.size() != rendered_queue_size_) {
    if (snapshot.queue_index_ready) {
      queue_page_start_ = snapshot.queue_page_start;
    } else if (snapshot.active_queue_index >= 0 &&
        static_cast<size_t>(snapshot.active_queue_index) < snapshot.queue.size()) {
      queue_page_start_ = static_cast<size_t>(snapshot.active_queue_index) / 100 * 100;
    } else {
      queue_page_start_ = 0;
    }
    rebuild_queue(snapshot);
    rendered_queue_size_ = snapshot.queue.size();
    rendered_queue_revision_ = snapshot.queue_revision;
  }
  if (snapshot.active_queue_index != rendered_active_queue_index_) {
    update_queue_selection(snapshot);
    rendered_active_queue_index_ = snapshot.active_queue_index;
  }
  if (snapshot.playlists_revision != rendered_playlist_revision_ ||
      snapshot.playlists.size() != rendered_playlist_size_) {
    rebuild_playlists(snapshot);
    rendered_playlist_size_ = snapshot.playlists.size();
    rendered_playlist_revision_ = snapshot.playlists_revision;
  }
  if (snapshot.library_revision != rendered_library_revision_) {
    rebuild_library(snapshot);
    rendered_library_revision_ = snapshot.library_revision;
  }
  if (snapshot.connection == ConnectionState::online &&
      snapshot.connection_generation != rendered_lyrics_connection_generation_) {
    lyrics_requested_key_.clear();
  }
  if (snapshot.lyrics_revision != rendered_lyrics_revision_ ||
      snapshot.connection != rendered_lyrics_connection_ ||
      snapshot.connection_generation != rendered_lyrics_connection_generation_ ||
      snapshot.artwork_track_key != rendered_lyrics_track_key_) {
    update_lyrics(snapshot);
    rendered_lyrics_revision_ = snapshot.lyrics_revision;
    rendered_lyrics_connection_ = snapshot.connection;
    rendered_lyrics_connection_generation_ = snapshot.connection_generation;
    rendered_lyrics_track_key_ = snapshot.artwork_track_key;
  }
}

void MusicUi::update_timeline(int64_t now_us, bool force) {
  constexpr int64_t kTimelineIntervalUs = 100'000;
  if (!force && now_us - last_timeline_render_us_ < kTimelineIntervalUs) {
    return;
  }
  last_timeline_render_us_ = now_us;

  if (timeline_pending_seek_us_ != 0 && snapshot_.seek_sent_us > timeline_pending_seek_us_) {
    timeline_pending_seek_sent_us_ = snapshot_.seek_sent_us;
  }
  if (snapshot_.position_updated_us > timeline_authoritative_updated_us_) {
    timeline_authoritative_updated_us_ = snapshot_.position_updated_us;
    // A model copy made before the seek reply can still carry the old host
    // position. Keep the optimistic anchor until a packet received after the
    // seek arrives.
    if (timeline_pending_seek_us_ == 0 ||
        (timeline_pending_seek_sent_us_ != 0 &&
         snapshot_.position_updated_us > timeline_pending_seek_sent_us_)) {
      timeline_position_ms_ = snapshot_.position_ms;
      timeline_anchor_position_ms_ = snapshot_.position_ms;
      timeline_anchor_us_ = snapshot_.position_updated_us;
      timeline_extrapolating_ = snapshot_.connection == ConnectionState::online &&
                                snapshot_.playback == PlaybackState::playing;
      timeline_pending_seek_us_ = 0;
      timeline_pending_seek_sent_us_ = 0;
    }
  }

  if (timeline_extrapolating_ && now_us >= timeline_anchor_us_) {
    timeline_position_ms_ =
        timeline_anchor_position_ms_ + (now_us - timeline_anchor_us_) / 1000;
  }
  if (snapshot_.connection != ConnectionState::online ||
      snapshot_.playback != PlaybackState::playing) {
    // Freeze the most recently extrapolated value. Reconnecting does not turn
    // extrapolation back on; only a newer authoritative type-6 packet does.
    timeline_anchor_position_ms_ = timeline_position_ms_;
    timeline_anchor_us_ = now_us;
    timeline_extrapolating_ = false;
    timeline_pending_seek_us_ = 0;
    timeline_pending_seek_sent_us_ = 0;
  }

  int64_t position_ms = timeline_position_ms_;
  if (snapshot_.now_playing.duration_ms > 0) {
    position_ms = std::clamp<int64_t>(position_ms, 0, snapshot_.now_playing.duration_ms);
  } else {
    position_ms = std::max<int64_t>(0, position_ms);
  }

  const int32_t duration_deciseconds =
      std::max<int32_t>(1, snapshot_.now_playing.duration_ms / 100);
  const int64_t elapsed_seconds = position_ms / 1000;
  const int32_t slider_deciseconds = static_cast<int32_t>(
      std::min<int64_t>(position_ms / 100, duration_deciseconds));
  if (!lv_slider_is_dragged(progress_slider_)) {
    lv_slider_set_value(progress_slider_, slider_deciseconds, LV_ANIM_OFF);
  }
  if (force || elapsed_seconds != rendered_position_second_) {
    const std::string times = format_time(static_cast<int32_t>(position_ms)) + "  /  " +
                              format_time(snapshot_.now_playing.duration_ms);
    lv_label_set_text(time_label_, times.c_str());
    rendered_position_second_ = elapsed_seconds;
  }
}

void MusicUi::rebuild_queue(const Snapshot &snapshot) {
  lv_obj_clean(queue_list_);
  queue_build_index_ = snapshot.queue_index_ready ? 0 : queue_page_start_;
  queue_build_limit_ = snapshot.queue_index_ready
                           ? snapshot.queue.size()
                           : std::min<size_t>(snapshot.queue.size(), queue_page_start_ + 100);
  const size_t total = snapshot.queue_index_ready ? snapshot.queue_total_count
                                                   : snapshot.queue.size();
  lv_obj_set_state(queue_previous_page_, LV_STATE_DISABLED, queue_page_start_ == 0);
  lv_obj_set_state(queue_next_page_, LV_STATE_DISABLED,
                   queue_page_start_ + 100 >= total);
}

void MusicUi::append_queue_rows(const Snapshot &snapshot) {
  // Constructing hundreds of LVGL objects in one pass starves the idle task on
  // the ESP32-S3. Eight rows per frame keeps input and the watchdog responsive.
  const size_t end = std::min(queue_build_index_ + 8, queue_build_limit_);
  for (size_t i = queue_build_index_; i < end; ++i) {
    const Track &track = snapshot.queue[i];
    const size_t queue_row = track.queue_row >= 0 ? static_cast<size_t>(track.queue_row) : i;
    lv_obj_t *row = lv_button_create(queue_list_);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_radius(row, 2, 0);
    lv_obj_set_style_bg_color(row, queue_row == static_cast<size_t>(snapshot.active_queue_index)
                                      ? color(0x154344)
                                      : color(kPanel),
                              0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_add_event_cb(row, queue_row_handler, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(row, queue_row_handler, LV_EVENT_LONG_PRESSED, this);
    lv_obj_t *index = make_label(row, std::to_string(queue_row + 1).c_str(), &lv_font_montserrat_14,
                                 color(kMuted));
    lv_obj_align(index, LV_ALIGN_LEFT_MID, 0, 0);
    const std::string title = track.title.empty() ? "Unknown track" : track.title;
    lv_obj_t *name = make_label(row, title.c_str(), &lv_font_montserrat_18, color(kText));
    lv_obj_set_size(name, 318, 21);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 48, -8);
    lv_obj_t *artist = make_label(row, track.artist.c_str(), &lv_font_montserrat_14, color(kMuted));
    lv_obj_set_size(artist, 318, 17);
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, 48, 11);
    const std::string duration = format_time(track.duration_ms);
    lv_obj_t *time = make_label(row, duration.c_str(), &lv_font_montserrat_14, color(kMuted));
    lv_obj_align(time, LV_ALIGN_RIGHT_MID, -2, 0);
  }
  queue_build_index_ = end;
}

void MusicUi::update_queue_selection(const Snapshot &snapshot) {
  const uint32_t count = lv_obj_get_child_count(queue_list_);
  for (uint32_t child = 0; child < count; ++child) {
    const size_t position = snapshot.queue_index_ready ? child : queue_page_start_ + child;
    if (position >= snapshot.queue.size()) {
      break;
    }
    const Track &track = snapshot.queue[position];
    const size_t index = track.queue_row >= 0 ? static_cast<size_t>(track.queue_row)
                                              : queue_page_start_ + child;
    lv_obj_set_style_bg_color(lv_obj_get_child(queue_list_, child),
                              index == static_cast<size_t>(snapshot.active_queue_index)
                                  ? color(0x154344)
                                  : color(kPanel),
                              0);
  }
}

void MusicUi::rebuild_playlists(const Snapshot &snapshot) {
  lv_obj_clean(playlist_list_);
  for (size_t i = 0; i < snapshot.playlists.size(); ++i) {
    const Playlist &playlist = snapshot.playlists[i];
    lv_obj_t *row = lv_button_create(playlist_list_);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_radius(row, 2, 0);
    lv_obj_set_style_bg_color(row, playlist.active ? color(0x154344) : color(kPanel), 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_add_event_cb(row, playlist_row_handler, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(row, playlist_row_handler, LV_EVENT_LONG_PRESSED, this);
    lv_obj_t *name = make_label(row, playlist.name.c_str(), &lv_font_montserrat_18,
                                playlist.active ? color(kAccent) : color(kText));
    lv_obj_set_width(name, 350);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 2, 0);
    const std::string count = std::to_string(playlist.count) +
                              (playlist.folder ? " ITEMS  " LV_SYMBOL_RIGHT : " TRACKS");
    lv_obj_t *details = make_label(row, count.c_str(), &lv_font_montserrat_14, color(kMuted));
    lv_obj_align(details, LV_ALIGN_RIGHT_MID, -2, 0);
  }
}

void MusicUi::event_handler(lv_event_t *event) {
  auto *ui = static_cast<MusicUi *>(lv_event_get_user_data(event));
  if (ui != nullptr) {
    ui->handle_event(event);
  }
}

void MusicUi::queue_row_handler(lv_event_t *event) {
  auto *ui = static_cast<MusicUi *>(lv_event_get_user_data(event));
  lv_obj_t *row = static_cast<lv_obj_t *>(lv_event_get_target(event));
  const int32_t local_index = lv_obj_get_index(row);
  if (ui == nullptr || local_index < 0) {
    return;
  }
  const size_t index = ui->snapshot_.queue_index_ready
                           ? static_cast<size_t>(local_index)
                           : ui->queue_page_start_ + static_cast<size_t>(local_index);
  if (index >= ui->snapshot_.queue.size()) {
    return;
  }
  if (ui->snapshot_.connection != ConnectionState::online) {
    return;
  }
  if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
    ui->suppress_queue_click_ = true;
    if (ui->snapshot_.queue_from_persisted_cache) {
      return;
    }
    const Track &track = ui->snapshot_.queue[index];
    const int32_t item = track.index >= 0       ? track.index
                         : track.queue_row >= 0 ? track.queue_row
                                                : static_cast<int32_t>(index);
    ui->client_.enqueue({CommandType::delete_index, ui->snapshot_.active_playlist_id,
                         item});
  } else {
    if (ui->suppress_queue_click_) {
      ui->suppress_queue_click_ = false;
      return;
    }
    // AIMP's play command takes the zero-based queue row. The item ID carried
    // in playlist metadata is for mutations/details and may legitimately be -1.
    const int32_t queue_row = ui->snapshot_.queue[index].queue_row >= 0
                                  ? ui->snapshot_.queue[index].queue_row
                                  : static_cast<int32_t>(index);
    ui->client_.enqueue({CommandType::play_index, queue_row});
    ui->show_page(Page::now);
  }
}

void MusicUi::playlist_row_handler(lv_event_t *event) {
  auto *ui = static_cast<MusicUi *>(lv_event_get_user_data(event));
  lv_obj_t *row = static_cast<lv_obj_t *>(lv_event_get_target(event));
  const int32_t index = lv_obj_get_index(row);
  if (ui == nullptr || index < 0 || static_cast<size_t>(index) >= ui->snapshot_.playlists.size()) {
    return;
  }
  const Playlist &playlist = ui->snapshot_.playlists[index];
  if (ui->snapshot_.connection != ConnectionState::online) {
    return;
  }
  if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
    ui->suppress_playlist_click_ = true;
    if (!ui->snapshot_.queue_from_persisted_cache && !playlist.folder &&
        !playlist.read_only && !playlist.active) {
      const int32_t folder_id = ui->playlist_folder_ids_.empty()
                                    ? -1
                                    : ui->playlist_folder_ids_.back();
      ui->client_.enqueue({CommandType::delete_playlist, playlist.id, folder_id});
    }
    return;
  }
  if (ui->suppress_playlist_click_) {
    ui->suppress_playlist_click_ = false;
    return;
  }
  if (playlist.folder && lv_event_get_code(event) != LV_EVENT_LONG_PRESSED) {
    ui->playlist_folder_ids_.push_back(playlist.id);
    ui->playlist_folder_names_.push_back(playlist.name);
    lv_label_set_text(ui->playlist_title_, playlist.name.c_str());
    lv_obj_add_flag(ui->new_playlist_button_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui->playlist_back_button_, LV_OBJ_FLAG_HIDDEN);
    ui->client_.enqueue({CommandType::request_playlists, playlist.id});
  } else if (!playlist.folder) {
    ui->client_.enqueue({CommandType::activate_playlist, playlist.id});
    ui->show_page(Page::queue);
  }
}

void MusicUi::library_row_handler(lv_event_t *event) {
  auto *ui = static_cast<MusicUi *>(lv_event_get_user_data(event));
  lv_obj_t *row = static_cast<lv_obj_t *>(lv_event_get_target(event));
  const int32_t local_index = lv_obj_get_index(row);
  if (ui == nullptr || local_index < 0 ||
      ui->snapshot_.connection != ConnectionState::online) {
    return;
  }
  if (ui->library_root_visible_) {
    if (local_index < 4) {
      ui->library_root_visible_ = false;
      ui->library_page_start_ = 0;
      ui->client_.enqueue({CommandType::request_library_root, local_index});
      lv_label_set_text(ui->library_count_, "LOADING...");
      lv_obj_clean(ui->library_list_);
    }
    return;
  }
  const size_t index = ui->library_page_start_ + static_cast<size_t>(local_index);
  const LibraryItems &items = *ui->snapshot_.library_items;
  if (index >= items.size()) {
    return;
  }
  const LibraryItem &item = items[index];
  if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
    ui->suppress_library_click_ = true;
    if (item.kind == LibraryItemKind::track &&
        !ui->snapshot_.queue_from_persisted_cache) {
      ui->client_.enqueue({CommandType::enqueue_library_item, item.selection_index,
                           static_cast<int32_t>(item.selection_request_id), item.title,
                           item.path});
    } else if (item.kind == LibraryItemKind::track) {
      lv_label_set_text(ui->library_count_, "REFRESH PLAYLIST FIRST");
    }
    return;
  }
  if (ui->suppress_library_click_) {
    ui->suppress_library_click_ = false;
    return;
  }
  if (item.kind == LibraryItemKind::track) {
    if (ui->snapshot_.queue_from_persisted_cache ||
        !ui->snapshot_.active_playlist_count_known ||
        ui->snapshot_.active_playlist_id < 0 || ui->snapshot_.library_play_pending) {
      lv_label_set_text(ui->library_count_, ui->snapshot_.library_play_pending
                                                ? "WAIT FOR CURRENT ADD"
                                                : "PLAYLIST REFRESHING - TRY AGAIN");
      return;
    }
    ui->client_.enqueue({CommandType::play_library_item, item.selection_index,
                         static_cast<int32_t>(item.selection_request_id), item.title,
                         item.path});
    lv_obj_add_flag(ui->library_overlay_, LV_OBJ_FLAG_HIDDEN);
    ui->show_page(Page::now);
  } else {
    ui->library_page_start_ = 0;
    ui->client_.enqueue({CommandType::browse_library, 0, 0, item.title, item.path});
  }
}

void MusicUi::handle_event(lv_event_t *event) {
  lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
  const lv_event_code_t code = lv_event_get_code(event);
  for (size_t i = 0; i < 4; ++i) {
    if (target == nav_buttons_[i]) {
      end_advanced_demo();
      show_page(static_cast<Page>(i));
      return;
    }
  }
  if (target == settings_back_button_) {
    end_advanced_demo();
    lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (target == playback_settings_button_) {
    show_settings_section(SettingsSection::playback);
    return;
  }
  if (target == connection_settings_button_) {
    show_settings_section(SettingsSection::connection);
    return;
  }
  if (target == display_settings_button_) {
    show_settings_section(SettingsSection::display);
    return;
  }
  if (target == storage_settings_button_) {
    show_settings_section(SettingsSection::storage);
    return;
  }
  if (target == advanced_settings_button_) {
    end_advanced_demo();
    show_settings_section(SettingsSection::advanced);
    if (snapshot_.connection == ConnectionState::online) {
      client_.enqueue({CommandType::request_advanced_controls});
    }
    return;
  }
  if (target == equalizer_close_button_) {
    lv_obj_add_flag(equalizer_overlay_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (target == equalizer_edit_button_) {
    if (snapshot_.connection == ConnectionState::online &&
        advanced::equalizer_mutation_allowed(
            snapshot_.advanced_availability.equalizer, snapshot_.equalizer)) {
      rebuild_equalizer(snapshot_);
      lv_obj_remove_flag(equalizer_overlay_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(equalizer_overlay_);
    }
    return;
  }
  if (target == sleep_timer_button_) {
    if (advanced_demo_active_ || snapshot_.connection != ConnectionState::online ||
        !advanced::sleep_timer_mutation_allowed(
            snapshot_.advanced_availability.sleep_timer, snapshot_.sleep_timer)) {
      return;
    }
    int32_t seconds = 0;
    uint8_t trigger = 1;
    if (!snapshot_.sleep_timer.enabled) {
      seconds = 15 * 60;
    } else if (snapshot_.sleep_timer.trigger == 1 &&
               snapshot_.sleep_timer.seconds_remaining <= 15 * 60) {
      seconds = 30 * 60;
    } else if (snapshot_.sleep_timer.trigger == 1 &&
               snapshot_.sleep_timer.seconds_remaining <= 30 * 60) {
      seconds = 60 * 60;
    } else if (snapshot_.sleep_timer.trigger == 1) {
      trigger = 2;
    }
    client_.enqueue({CommandType::sleep_timer, seconds,
                     static_cast<int32_t>((trigger << 8) | 1U)});
    return;
  }
  for (size_t index = 0; index < power_buttons_.size(); ++index) {
    if (target != power_buttons_[index]) {
      continue;
    }
    if (snapshot_.connection != ConnectionState::online) {
      return;
    }
    static constexpr std::array<const char *, 3> kMessages{
        "Put the AIMP computer to sleep?\nPlayback and the remote connection will stop.",
        "Restart the AIMP computer now?\nUnsaved work on that computer may be lost.",
        "Shut down the AIMP computer now?\nUnsaved work on that computer may be lost."};
    pending_power_action_ = static_cast<int32_t>(index);
    lv_label_set_text(power_confirm_message_, kMessages[index]);
    lv_obj_remove_flag(power_confirm_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(power_confirm_overlay_);
    return;
  }
  if (target == power_cancel_button_) {
    pending_power_action_ = -1;
    lv_obj_add_flag(power_confirm_overlay_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (target == power_confirm_button_) {
    if (!advanced_demo_active_ && pending_power_action_ >= 0 && pending_power_action_ <= 2 &&
        snapshot_.connection == ConnectionState::online) {
      client_.enqueue({CommandType::computer_power, pending_power_action_});
    }
    pending_power_action_ = -1;
    lv_obj_add_flag(power_confirm_overlay_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  for (size_t index = 0; index < equalizer_gain_sliders_.size(); ++index) {
    if (target != equalizer_gain_sliders_[index]) {
      continue;
    }
    if (!advanced::equalizer_mutation_allowed(
            snapshot_.advanced_availability.equalizer, snapshot_.equalizer)) {
      return;
    }
    const int32_t value = lv_slider_get_value(target);
    char db[16];
    format_gain(db, value);
    lv_label_set_text(equalizer_gain_values_[index], db);
    if (code == LV_EVENT_RELEASED && !advanced_demo_active_ &&
        snapshot_.connection == ConnectionState::online) {
      client_.enqueue({CommandType::equalizer_gain,
                       index == 0 ? -1 : static_cast<int32_t>(index - 1), value});
    }
    return;
  }
  if (target == wifi_ssid_button_) {
    begin_setting_edit(SettingField::wifi_ssid, "WI-FI NETWORK", false);
    return;
  }
  if (target == wifi_password_button_) {
    begin_setting_edit(SettingField::wifi_password, "WI-FI PASSWORD", true);
    return;
  }
  if (target == server_host_button_) {
    begin_setting_edit(SettingField::server_host, "AIMP SERVER", false);
    return;
  }
  if (target == server_port_button_) {
    begin_setting_edit(SettingField::server_port, "AIMP PORT", false);
    return;
  }
  if (target == server_password_button_) {
    begin_setting_edit(SettingField::server_password, "AIMP PASSWORD", true);
    return;
  }
  if (target == setting_editor_keyboard_ || target == setting_editor_textarea_) {
    finish_setting_edit(code == LV_EVENT_READY);
    return;
  }
  if (target == brightness_slider_) {
    const uint8_t brightness = static_cast<uint8_t>(lv_slider_get_value(brightness_slider_));
    char text[8];
    std::snprintf(text, sizeof(text), "%u%%", brightness);
    lv_label_set_text(brightness_value_, text);
    brightness_callback_(brightness);
    last_interaction_us_ = esp_timer_get_time();
    if (code == LV_EVENT_RELEASED) {
      if (!settings_.set_brightness(brightness)) {
        const uint8_t stored_brightness = settings_.snapshot().brightness;
        brightness_callback_(stored_brightness);
        lv_label_set_text(settings_notice_, "SAVE FAILED");
        refresh_setting_labels();
      } else {
        lv_label_set_text(settings_notice_, "");
      }
    }
    return;
  }
  if (target == auto_blank_button_) {
    const uint32_t current = settings_.snapshot().auto_blank_seconds;
    if (!settings_.set_auto_blank_seconds(settings_policy::next_auto_blank_seconds(current))) {
      lv_label_set_text(settings_notice_, "SAVE FAILED");
    } else {
      lv_label_set_text(settings_notice_, "");
    }
    last_interaction_us_ = esp_timer_get_time();
    refresh_setting_labels();
    return;
  }
  if (target == flip_button_) {
    const bool flipped = !settings_.snapshot().display_flipped;
    if (!settings_.set_display_flipped(flipped)) {
      lv_label_set_text(settings_notice_, "SAVE FAILED");
    } else {
      lv_label_set_text(settings_notice_, "");
      apply_display_orientation(flipped);
    }
    last_interaction_us_ = esp_timer_get_time();
    refresh_setting_labels();
    return;
  }
  if (target == queue_previous_page_ || target == queue_next_page_) {
    if (target == queue_previous_page_ && queue_page_start_ >= 100) {
      queue_page_start_ -= 100;
    } else if (target == queue_next_page_ &&
               queue_page_start_ + 100 < (snapshot_.queue_index_ready
                                               ? snapshot_.queue_total_count
                                               : snapshot_.queue.size())) {
      queue_page_start_ += 100;
    }
    if (snapshot_.queue_index_ready) {
      client_.enqueue({CommandType::request_queue_page,
                       static_cast<int32_t>(queue_page_start_)});
    } else {
      rebuild_queue(snapshot_);
    }
    return;
  }
  if (target == queue_filter_button_) {
    lv_obj_remove_flag(filter_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(filter_overlay_);
    return;
  }
  if (target == filter_close_button_) {
    lv_obj_add_flag(filter_overlay_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (target == filter_search_button_) {
    search_context_ = SearchContext::queue;
    lv_obj_add_flag(filter_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(search_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(search_overlay_);
    lv_obj_send_event(search_textarea_, LV_EVENT_FOCUSED, nullptr);
    return;
  }
  for (size_t i = 0; i < std::size(alphabet_buttons_); ++i) {
    if (target != alphabet_buttons_[i]) {
      continue;
    }
    lv_obj_add_flag(filter_overlay_, LV_OBJ_FLAG_HIDDEN);
    if (i == 27) {
      client_.enqueue({CommandType::clear_queue_filter});
    } else if (i == 0) {
      client_.enqueue({CommandType::filter_queue,
                       static_cast<int32_t>(PlaylistIndex::SearchMode::non_alpha), 0, ""});
    } else {
      std::string letter(1, static_cast<char>('A' + i - 1));
      client_.enqueue({CommandType::filter_queue,
                       static_cast<int32_t>(PlaylistIndex::SearchMode::prefix), 0,
                       std::move(letter)});
    }
    return;
  }
  if (target == search_keyboard_ || target == search_textarea_) {
    if (code == LV_EVENT_CANCEL) {
      lv_obj_add_flag(search_overlay_, LV_OBJ_FLAG_HIDDEN);
      if (search_context_ == SearchContext::library) {
        lv_obj_remove_flag(library_overlay_, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_remove_flag(filter_overlay_, LV_OBJ_FLAG_HIDDEN);
      }
    } else if (code == LV_EVENT_READY) {
      const char *query = lv_textarea_get_text(search_textarea_);
      if (query != nullptr && query[0] != '\0') {
        if (search_context_ == SearchContext::library) {
          library_root_visible_ = false;
          library_page_start_ = 0;
          lv_label_set_text(library_count_, "SEARCHING...");
          lv_obj_clean(library_list_);
          client_.enqueue({CommandType::search_library, 0, 0, query});
          lv_obj_remove_flag(library_overlay_, LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(library_overlay_);
        } else {
          client_.enqueue({CommandType::filter_queue,
                           static_cast<int32_t>(PlaylistIndex::SearchMode::contains), 0,
                           query});
        }
        lv_textarea_set_text(search_textarea_, "");
        lv_obj_add_flag(search_overlay_, LV_OBJ_FLAG_HIDDEN);
      }
    }
    return;
  }
  if (target == library_button_) {
    show_library_root();
    lv_obj_remove_flag(library_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(library_overlay_);
    return;
  }
  if (target == lyrics_button_) {
    lv_obj_remove_flag(lyrics_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lyrics_overlay_);
    if (snapshot_.connection == ConnectionState::online) {
      lyrics_requested_key_ = snapshot_.artwork_track_key;
      client_.enqueue({CommandType::request_lyrics});
    }
    return;
  }
  if (target == library_back_button_) {
    if (library_root_visible_) {
      lv_obj_add_flag(library_overlay_, LV_OBJ_FLAG_HIDDEN);
    } else {
      show_library_root();
    }
    return;
  }
  if (target == library_search_button_) {
    search_context_ = SearchContext::library;
    lv_obj_add_flag(library_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(search_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(search_overlay_);
    lv_obj_send_event(search_textarea_, LV_EVENT_FOCUSED, nullptr);
    return;
  }
  if (target == library_previous_page_ || target == library_next_page_) {
    constexpr size_t kLibraryPageSize = 25;
    if (target == library_previous_page_ && library_page_start_ >= kLibraryPageSize) {
      library_page_start_ -= kLibraryPageSize;
    } else if (target == library_next_page_ &&
               library_page_start_ + kLibraryPageSize < snapshot_.library_items->size()) {
      library_page_start_ += kLibraryPageSize;
    }
    rebuild_library(snapshot_);
    return;
  }
  if (snapshot_.connection != ConnectionState::online) {
    return;
  }
  if (target == play_button_) {
    if (code == LV_EVENT_LONG_PRESSED) {
      suppress_play_click_ = true;
      client_.enqueue({CommandType::stop});
    } else if (suppress_play_click_) {
      suppress_play_click_ = false;
    } else if (snapshot_.playback == PlaybackState::playing) {
      client_.enqueue({CommandType::pause});
    } else {
      client_.enqueue({CommandType::play});
    }
    return;
  }
  if (target == playlist_back_button_) {
    if (!playlist_folder_ids_.empty()) {
      playlist_folder_ids_.pop_back();
      playlist_folder_names_.pop_back();
    }
    const int32_t folder_id = playlist_folder_ids_.empty() ? -1 : playlist_folder_ids_.back();
    lv_label_set_text(playlist_title_,
                      playlist_folder_names_.empty() ? "PLAYLISTS"
                                                     : playlist_folder_names_.back().c_str());
    if (playlist_folder_ids_.empty()) {
      lv_obj_clear_flag(new_playlist_button_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(playlist_back_button_, LV_OBJ_FLAG_HIDDEN);
    }
    client_.enqueue({CommandType::request_playlists, folder_id});
    return;
  }
  const char *text = lv_obj_check_type(target, &lv_button_class) && lv_obj_get_child_count(target)
                         ? lv_label_get_text(lv_obj_get_child(target, 0))
                         : "";
  if (target == progress_slider_) {
    const int32_t target_ms = lv_slider_get_value(progress_slider_) * 100;
    const int64_t now_us = esp_timer_get_time();
    timeline_position_ms_ = target_ms;
    timeline_anchor_position_ms_ = target_ms;
    timeline_anchor_us_ = now_us;
    timeline_pending_seek_us_ = now_us;
    timeline_pending_seek_sent_us_ = 0;
    timeline_extrapolating_ = snapshot_.connection == ConnectionState::online &&
                              snapshot_.playback == PlaybackState::playing;
    update_timeline(now_us, true);
    client_.enqueue({CommandType::seek, target_ms});
  } else if (target == volume_slider_ || target == main_volume_slider_) {
    client_.enqueue({CommandType::volume, lv_slider_get_value(target) * 65535 / 100});
  } else if (target == shuffle_button_ || target == more_shuffle_) {
    client_.enqueue({CommandType::shuffle, snapshot_.shuffle ? 0 : 1});
  } else if (target == repeat_button_ || target == more_repeat_) {
    client_.enqueue({CommandType::repeat, (snapshot_.repeat + 1) % 3});
  } else if (std::strcmp(text, LV_SYMBOL_PREV) == 0) {
    client_.enqueue({CommandType::previous});
  } else if (std::strcmp(text, LV_SYMBOL_NEXT) == 0) {
    client_.enqueue({CommandType::next});
  } else if (std::strcmp(text, "STOP") == 0) {
    client_.enqueue({CommandType::stop});
  } else if (std::strcmp(text, "REFRESH FROM AIMP") == 0) {
    client_.enqueue({CommandType::request_playlist, -1});
    const int32_t folder = playlist_folder_ids_.empty() ? -1 : playlist_folder_ids_.back();
    client_.enqueue({CommandType::request_playlists, folder});
  } else if (std::strcmp(text, "CLEAR") == 0 && code == LV_EVENT_LONG_PRESSED &&
             !snapshot_.queue_from_persisted_cache) {
    client_.enqueue({CommandType::clear_playlist, snapshot_.active_playlist_id});
  } else if (std::strcmp(text, "+ ESP REMOTE") == 0 && code == LV_EVENT_LONG_PRESSED) {
    client_.enqueue({CommandType::create_playlist, -1, 0, "ESP Remote"});
  }
}

} // namespace musicremote

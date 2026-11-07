#include "music_ui.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>

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

} // namespace

MusicUi::MusicUi(Model &model, MonkeyMoteClient &client) : model_(model), client_(client) {
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
  show_page(static_cast<Page>(page));
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
      update_timeline(esp_timer_get_time());
      append_queue_rows(snapshot_);
      lv_timer_handler();
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
  lv_obj_add_flag(art_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(art_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_t *note = make_label(art_, LV_SYMBOL_AUDIO, &lv_font_montserrat_48, color(kAccent));
  lv_obj_center(note);
  rating_label_ = make_label(art_, "RATING 0 / 5", &lv_font_montserrat_14, color(kText));
  lv_obj_align(rating_label_, LV_ALIGN_BOTTOM_MID, 0, -8);

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
  queue_title_ = make_label(page, "CURRENT PLAYLIST", &lv_font_montserrat_22, color(kText));
  lv_obj_set_pos(queue_title_, 14, 4);
  lv_obj_set_width(queue_title_, 250);
  lv_label_set_long_mode(queue_title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
  queue_previous_page_ = make_button(page, LV_SYMBOL_LEFT, 48, 42);
  lv_obj_set_pos(queue_previous_page_, 278, 0);
  lv_obj_add_event_cb(queue_previous_page_, event_handler, LV_EVENT_CLICKED, this);
  queue_next_page_ = make_button(page, LV_SYMBOL_RIGHT, 48, 42);
  lv_obj_set_pos(queue_next_page_, 332, 0);
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
  lv_obj_t *title = make_label(page, "PLAYBACK", &lv_font_montserrat_28, color(kText));
  lv_obj_set_pos(title, 14, 4);
  more_status_ = make_label(page, "OFFLINE", &lv_font_montserrat_14, color(kMuted));
  lv_obj_set_pos(more_status_, 16, 42);

  more_shuffle_ = make_button(page, "SHUFFLE", 132, 52);
  more_repeat_ = make_button(page, "REPEAT", 132, 52);
  lv_obj_t *stop = make_button(page, "STOP", 132, 52);
  lv_obj_set_pos(more_shuffle_, 14, 72);
  lv_obj_set_pos(more_repeat_, 174, 72);
  lv_obj_set_pos(stop, 334, 72);
  lv_obj_add_flag(more_shuffle_, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_flag(more_repeat_, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_event_cb(more_shuffle_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(more_repeat_, event_handler, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(stop, event_handler, LV_EVENT_CLICKED, this);

  lv_obj_t *volume_text = make_label(page, "VOLUME", &lv_font_montserrat_18, color(kMuted));
  lv_obj_set_pos(volume_text, 14, 156);
  volume_slider_ = lv_slider_create(page);
  lv_obj_set_pos(volume_slider_, 110, 157);
  lv_obj_set_size(volume_slider_, 356, 22);
  lv_obj_set_ext_click_area(volume_slider_, 16);
  lv_slider_set_range(volume_slider_, 0, 100);
  lv_obj_set_style_bg_color(volume_slider_, color(0x28303D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(volume_slider_, color(kAccent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(volume_slider_, color(kText), LV_PART_KNOB);
  lv_obj_add_event_cb(volume_slider_, event_handler, LV_EVENT_RELEASED, this);
  lv_obj_t *refresh = make_button(page, "REFRESH FROM AIMP", 230, 44);
  lv_obj_align(refresh, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_add_event_cb(refresh, event_handler, LV_EVENT_CLICKED, this);
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
  char rating[16];
  std::snprintf(rating, sizeof(rating), "RATING %u / 5", snapshot.now_playing.rating);
  lv_label_set_text(rating_label_, rating);
  lv_label_set_text(play_icon_, snapshot.playback == PlaybackState::playing ? LV_SYMBOL_PAUSE
                                                                            : LV_SYMBOL_PLAY);
  const int32_t duration_deciseconds =
      std::max<int32_t>(1, snapshot.now_playing.duration_ms / 100);
  lv_slider_set_range(progress_slider_, 0, duration_deciseconds);
  update_timeline(esp_timer_get_time(), true);
  const int volume_percent = (static_cast<uint32_t>(snapshot.volume) * 100) / 65535;
  if (!lv_slider_is_dragged(volume_slider_)) {
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
  lv_label_set_text(queue_title_, snapshot.active_playlist_name.c_str());
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
    lv_obj_set_size(name, 330, 21);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 36, -8);
    lv_obj_t *artist = make_label(row, track.artist.c_str(), &lv_font_montserrat_14, color(kMuted));
    lv_obj_set_size(artist, 330, 17);
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, 36, 11);
    const std::string duration = format_time(track.duration_ms);
    lv_obj_t *time = make_label(row, duration.c_str(), &lv_font_montserrat_14, color(kMuted));
    lv_obj_align(time, LV_ALIGN_RIGHT_MID, -2, 0);
  }
  queue_build_index_ = end;
}

void MusicUi::update_queue_selection(const Snapshot &snapshot) {
  const uint32_t count = lv_obj_get_child_count(queue_list_);
  for (uint32_t child = 0; child < count; ++child) {
    const Track &track = snapshot.queue[snapshot.queue_index_ready ? child
                                                                   : queue_page_start_ + child];
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
    const int32_t item = ui->snapshot_.queue[index].index >= 0
                             ? ui->snapshot_.queue[index].index
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
    if (!playlist.folder && !playlist.read_only && !playlist.active) {
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

void MusicUi::handle_event(lv_event_t *event) {
  lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
  const lv_event_code_t code = lv_event_get_code(event);
  for (size_t i = 0; i < 4; ++i) {
    if (target == nav_buttons_[i]) {
      show_page(static_cast<Page>(i));
      return;
    }
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
  if (target == art_) {
    client_.enqueue({CommandType::rating, (snapshot_.now_playing.rating + 1) % 6});
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
  } else if (std::strcmp(text, "CLEAR") == 0 && code == LV_EVENT_LONG_PRESSED) {
    client_.enqueue({CommandType::clear_playlist, snapshot_.active_playlist_id});
  } else if (std::strcmp(text, "+ ESP REMOTE") == 0 && code == LV_EVENT_LONG_PRESSED) {
    client_.enqueue({CommandType::create_playlist, -1, 0, "ESP Remote"});
  }
}

} // namespace musicremote

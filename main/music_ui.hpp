#pragma once

#include "device_settings.hpp"
#include "monkeymote_client.hpp"
#include "music_model.hpp"

#include <functional>
#include <array>
#include <mutex>
#include <memory>
#include <vector>

#include "lvgl.h"

namespace musicremote {

class MusicUi {
public:
  MusicUi(Model &model, MonkeyMoteClient &client, DeviceSettings &settings,
          std::function<void(uint8_t)> brightness_callback);
  ~MusicUi();

  void start();

  // Captures the currently rendered 480x320 screen as packed RGB565. This is
  // used by the local-only diagnostic HTTP endpoint for visual regression
  // checks against the physical device.
  bool capture_rgb565(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height,
                      uint32_t &stride);
  bool select_page_for_diagnostics(uint8_t page);
  bool select_overlay_for_diagnostics(uint8_t overlay);
  bool select_library_category_for_diagnostics(uint8_t category);
  bool select_settings_for_diagnostics(uint8_t section);
  bool select_advanced_demo_for_diagnostics(uint8_t view);

private:
  enum class Page { now, queue, playlists, more };
  enum class SettingsSection { playback, connection, display, storage, advanced };
  enum class SettingField { none, wifi_ssid, wifi_password, server_host, server_port,
                            server_password };

  static void task_entry(void *context);
  static void event_handler(lv_event_t *event);
  static void queue_row_handler(lv_event_t *event);
  static void playlist_row_handler(lv_event_t *event);
  static void library_row_handler(lv_event_t *event);

  void run();
  void create_ui();
  void create_header();
  void create_navigation();
  void create_now_page();
  void create_queue_page();
  void create_playlists_page();
  void create_more_page();
  void create_settings_overlays();
  void create_filter_overlays();
  void create_library_overlay();
  void create_lyrics_overlay();
  void create_equalizer_overlay();
  void show_page(Page page);
  void update(const Snapshot &snapshot);
  void update_timeline(int64_t now_us, bool force = false);
  void rebuild_queue(const Snapshot &snapshot);
  void append_queue_rows(const Snapshot &snapshot);
  void update_queue_selection(const Snapshot &snapshot);
  void rebuild_playlists(const Snapshot &snapshot);
  void show_library_root();
  void rebuild_library(const Snapshot &snapshot);
  void update_lyrics(const Snapshot &snapshot);
  void show_settings_section(SettingsSection section);
  void begin_setting_edit(SettingField field, const char *title, bool password);
  void finish_setting_edit(bool save);
  void refresh_setting_labels();
  void update_advanced_controls(const Snapshot &snapshot);
  void rebuild_equalizer(const Snapshot &snapshot);
  void end_advanced_demo();
  void poll_display_timeout(int64_t now_us);
  void set_display_blank(bool blank);
  void handle_event(lv_event_t *event);

  lv_obj_t *make_button(lv_obj_t *parent, const char *text, int width, int height);
  lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       lv_color_t color);

  Model &model_;
  MonkeyMoteClient &client_;
  DeviceSettings &settings_;
  std::function<void(uint8_t)> brightness_callback_;
  void *task_{nullptr};
  std::recursive_mutex mutex_;
  Snapshot snapshot_;
  Page page_{Page::now};
  uint64_t rendered_revision_{UINT64_MAX};
  size_t rendered_queue_size_{SIZE_MAX};
  size_t rendered_playlist_size_{SIZE_MAX};
  uint64_t rendered_queue_revision_{UINT64_MAX};
  uint64_t rendered_playlist_revision_{UINT64_MAX};
  uint64_t rendered_artwork_revision_{UINT64_MAX};
  uint64_t rendered_library_revision_{UINT64_MAX};
  uint64_t rendered_lyrics_revision_{UINT64_MAX};
  uint64_t rendered_equalizer_revision_{UINT64_MAX};
  uint64_t rendered_sleep_timer_revision_{UINT64_MAX};
  ConnectionState rendered_lyrics_connection_{ConnectionState::offline};
  uint64_t rendered_lyrics_connection_generation_{UINT64_MAX};
  std::string rendered_lyrics_track_key_;
  size_t queue_build_index_{0};
  size_t queue_build_limit_{0};
  size_t queue_page_start_{0};
  int32_t rendered_active_queue_index_{-1};
  int64_t rendered_position_second_{-1};
  int64_t last_timeline_render_us_{0};
  int64_t timeline_position_ms_{0};
  int64_t timeline_anchor_position_ms_{0};
  int64_t timeline_anchor_us_{0};
  int64_t timeline_authoritative_updated_us_{0};
  int64_t timeline_pending_seek_us_{0};
  int64_t timeline_pending_seek_sent_us_{0};
  bool timeline_extrapolating_{false};
  bool suppress_play_click_{false};
  bool suppress_queue_click_{false};
  bool suppress_playlist_click_{false};
  bool suppress_library_click_{false};
  bool library_root_visible_{true};
  bool display_blank_{false};
  bool touch_was_pressed_{false};
  int64_t last_interaction_us_{0};
  SettingField editing_field_{SettingField::none};
  enum class SearchContext { queue, library };
  SearchContext search_context_{SearchContext::queue};
  size_t library_page_start_{0};
  std::string lyrics_requested_key_;
  std::vector<int32_t> playlist_folder_ids_;
  std::vector<std::string> playlist_folder_names_;

  lv_obj_t *root_{nullptr};
  lv_obj_t *status_dot_{nullptr};
  lv_obj_t *status_label_{nullptr};
  lv_obj_t *volume_slider_{nullptr};
  lv_obj_t *main_volume_slider_{nullptr};
  lv_obj_t *pages_[4]{};
  lv_obj_t *nav_buttons_[4]{};

  lv_obj_t *title_label_{nullptr};
  lv_obj_t *art_{nullptr};
  lv_obj_t *art_image_{nullptr};
  lv_obj_t *art_fallback_{nullptr};
  lv_obj_t *artist_label_{nullptr};
  lv_obj_t *album_label_{nullptr};
  lv_obj_t *time_label_{nullptr};
  lv_obj_t *main_volume_label_{nullptr};
  lv_obj_t *progress_slider_{nullptr};
  lv_obj_t *play_button_{nullptr};
  lv_obj_t *play_icon_{nullptr};
  lv_obj_t *shuffle_button_{nullptr};
  lv_obj_t *repeat_button_{nullptr};

  lv_obj_t *queue_title_{nullptr};
  lv_obj_t *queue_cache_label_{nullptr};
  lv_obj_t *queue_list_{nullptr};
  lv_obj_t *queue_previous_page_{nullptr};
  lv_obj_t *queue_next_page_{nullptr};
  lv_obj_t *queue_filter_button_{nullptr};
  lv_obj_t *playlist_list_{nullptr};
  lv_obj_t *playlist_title_{nullptr};
  lv_obj_t *playlist_back_button_{nullptr};
  lv_obj_t *new_playlist_button_{nullptr};
  lv_obj_t *more_status_{nullptr};
  lv_obj_t *more_shuffle_{nullptr};
  lv_obj_t *more_repeat_{nullptr};
  lv_obj_t *library_button_{nullptr};
  lv_obj_t *lyrics_button_{nullptr};
  lv_obj_t *playback_settings_button_{nullptr};
  lv_obj_t *connection_settings_button_{nullptr};
  lv_obj_t *display_settings_button_{nullptr};
  lv_obj_t *storage_settings_button_{nullptr};
  lv_obj_t *advanced_settings_button_{nullptr};
  lv_obj_t *capabilities_value_{nullptr};
  lv_obj_t *settings_storage_value_{nullptr};
  lv_obj_t *settings_overlay_{nullptr};
  lv_obj_t *settings_title_{nullptr};
  lv_obj_t *settings_notice_{nullptr};
  lv_obj_t *settings_back_button_{nullptr};
  lv_obj_t *settings_panels_[5]{};
  lv_obj_t *wifi_ssid_button_{nullptr};
  lv_obj_t *wifi_password_button_{nullptr};
  lv_obj_t *server_host_button_{nullptr};
  lv_obj_t *server_port_button_{nullptr};
  lv_obj_t *server_password_button_{nullptr};
  lv_obj_t *wifi_ssid_value_{nullptr};
  lv_obj_t *wifi_password_value_{nullptr};
  lv_obj_t *server_host_value_{nullptr};
  lv_obj_t *server_port_value_{nullptr};
  lv_obj_t *server_password_value_{nullptr};
  lv_obj_t *brightness_slider_{nullptr};
  lv_obj_t *brightness_value_{nullptr};
  lv_obj_t *auto_blank_button_{nullptr};
  lv_obj_t *auto_blank_value_{nullptr};
  lv_obj_t *setting_editor_{nullptr};
  lv_obj_t *setting_editor_title_{nullptr};
  lv_obj_t *setting_editor_textarea_{nullptr};
  lv_obj_t *setting_editor_keyboard_{nullptr};
  lv_obj_t *setting_editor_message_{nullptr};
  lv_obj_t *blank_overlay_{nullptr};
  lv_obj_t *advanced_status_{nullptr};
  lv_obj_t *equalizer_enable_button_{nullptr};
  lv_obj_t *equalizer_enable_value_{nullptr};
  lv_obj_t *equalizer_edit_button_{nullptr};
  lv_obj_t *sleep_timer_button_{nullptr};
  lv_obj_t *sleep_timer_value_{nullptr};
  lv_obj_t *power_title_{nullptr};
  std::array<lv_obj_t *, 3> power_buttons_{};
  lv_obj_t *equalizer_overlay_{nullptr};
  lv_obj_t *equalizer_list_{nullptr};
  lv_obj_t *equalizer_close_button_{nullptr};
  lv_obj_t *equalizer_overlay_toggle_{nullptr};
  std::vector<lv_obj_t *> equalizer_gain_sliders_;
  std::vector<lv_obj_t *> equalizer_gain_values_;
  lv_obj_t *power_confirm_overlay_{nullptr};
  lv_obj_t *power_confirm_message_{nullptr};
  lv_obj_t *power_confirm_button_{nullptr};
  lv_obj_t *power_cancel_button_{nullptr};
  int32_t pending_power_action_{-1};
  bool advanced_demo_active_{false};
  lv_obj_t *filter_overlay_{nullptr};
  lv_obj_t *filter_search_button_{nullptr};
  lv_obj_t *filter_close_button_{nullptr};
  lv_obj_t *alphabet_buttons_[28]{};
  lv_obj_t *search_overlay_{nullptr};
  lv_obj_t *search_textarea_{nullptr};
  lv_obj_t *search_keyboard_{nullptr};
  lv_obj_t *library_overlay_{nullptr};
  lv_obj_t *library_title_{nullptr};
  lv_obj_t *library_count_{nullptr};
  lv_obj_t *library_list_{nullptr};
  lv_obj_t *library_back_button_{nullptr};
  lv_obj_t *library_search_button_{nullptr};
  lv_obj_t *library_previous_page_{nullptr};
  lv_obj_t *library_next_page_{nullptr};
  lv_obj_t *lyrics_overlay_{nullptr};
  lv_obj_t *lyrics_title_{nullptr};
  lv_obj_t *lyrics_text_{nullptr};
  lv_image_dsc_t artwork_descriptor_{};
  std::shared_ptr<const ArtworkBitmap> displayed_artwork_;
};

} // namespace musicremote

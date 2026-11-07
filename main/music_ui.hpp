#pragma once

#include "monkeymote_client.hpp"
#include "music_model.hpp"

#include <mutex>
#include <vector>

#include "lvgl.h"

namespace musicremote {

class MusicUi {
public:
  MusicUi(Model &model, MonkeyMoteClient &client);
  ~MusicUi();

  void start();

  // Captures the currently rendered 480x320 screen as packed RGB565. This is
  // used by the local-only diagnostic HTTP endpoint for visual regression
  // checks against the physical device.
  bool capture_rgb565(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height,
                      uint32_t &stride);
  bool select_page_for_diagnostics(uint8_t page);

private:
  enum class Page { now, queue, playlists, more };

  static void task_entry(void *context);
  static void event_handler(lv_event_t *event);
  static void queue_row_handler(lv_event_t *event);
  static void playlist_row_handler(lv_event_t *event);

  void run();
  void create_ui();
  void create_header();
  void create_navigation();
  void create_now_page();
  void create_queue_page();
  void create_playlists_page();
  void create_more_page();
  void show_page(Page page);
  void update(const Snapshot &snapshot);
  void update_timeline(int64_t now_us, bool force = false);
  void rebuild_queue(const Snapshot &snapshot);
  void append_queue_rows(const Snapshot &snapshot);
  void update_queue_selection(const Snapshot &snapshot);
  void rebuild_playlists(const Snapshot &snapshot);
  void handle_event(lv_event_t *event);

  lv_obj_t *make_button(lv_obj_t *parent, const char *text, int width, int height);
  lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       lv_color_t color);

  Model &model_;
  MonkeyMoteClient &client_;
  void *task_{nullptr};
  std::recursive_mutex mutex_;
  Snapshot snapshot_;
  Page page_{Page::now};
  uint64_t rendered_revision_{UINT64_MAX};
  size_t rendered_queue_size_{SIZE_MAX};
  size_t rendered_playlist_size_{SIZE_MAX};
  uint64_t rendered_queue_revision_{UINT64_MAX};
  uint64_t rendered_playlist_revision_{UINT64_MAX};
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
  bool timeline_extrapolating_{false};
  bool suppress_play_click_{false};
  bool suppress_queue_click_{false};
  bool suppress_playlist_click_{false};
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
  lv_obj_t *rating_label_{nullptr};
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
  lv_obj_t *queue_list_{nullptr};
  lv_obj_t *queue_previous_page_{nullptr};
  lv_obj_t *queue_next_page_{nullptr};
  lv_obj_t *playlist_list_{nullptr};
  lv_obj_t *playlist_title_{nullptr};
  lv_obj_t *playlist_back_button_{nullptr};
  lv_obj_t *new_playlist_button_{nullptr};
  lv_obj_t *more_status_{nullptr};
  lv_obj_t *more_shuffle_{nullptr};
  lv_obj_t *more_repeat_{nullptr};
};

} // namespace musicremote

#include "device_settings.hpp"
#include "monkeymote_client.hpp"
#include "musicremote_config.hpp"
#include "music_model.hpp"
#include "music_ui.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <inttypes.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "smartpanlee-sc01-plus.hpp"

namespace {

constexpr char kTag[] = "musicremote";
constexpr EventBits_t kWifiConnected = BIT0;
EventGroupHandle_t wifi_events;

struct DiagnosticContext {
  musicremote::MusicUi *ui;
  musicremote::MonkeyMoteClient *client;
};

esp_err_t screenshot_handler(httpd_req_t *request) {
  auto *context = static_cast<DiagnosticContext *>(request->user_ctx);
  auto *ui = context == nullptr ? nullptr : context->ui;
  char query[192]{};
  char page[32]{};
  uint32_t render_delay_ms = 500;
  if (ui != nullptr && httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "page", page, sizeof(page)) == ESP_OK) {
    if (std::strcmp(page, "now") == 0) {
      ui->select_page_for_diagnostics(0);
    } else if (std::strcmp(page, "queue") == 0) {
      ui->select_page_for_diagnostics(1);
    } else if (std::strcmp(page, "playlists") == 0) {
      ui->select_page_for_diagnostics(2);
    } else if (std::strcmp(page, "more") == 0) {
      ui->select_page_for_diagnostics(3);
    } else if (std::strcmp(page, "az") == 0) {
      ui->select_overlay_for_diagnostics(1);
    } else if (std::strcmp(page, "search") == 0) {
      ui->select_overlay_for_diagnostics(2);
    } else if (std::strcmp(page, "library") == 0) {
      ui->select_overlay_for_diagnostics(3);
    } else if (std::strcmp(page, "lyrics") == 0) {
      ui->select_overlay_for_diagnostics(4);
    } else if (std::strcmp(page, "playback-settings") == 0) {
      ui->select_settings_for_diagnostics(0);
    } else if (std::strcmp(page, "connection-settings") == 0) {
      ui->select_settings_for_diagnostics(1);
    } else if (std::strcmp(page, "display-settings") == 0) {
      ui->select_settings_for_diagnostics(2);
    } else if (std::strcmp(page, "storage-settings") == 0) {
      ui->select_settings_for_diagnostics(3);
    } else if (std::strcmp(page, "artists") == 0) {
      ui->select_library_category_for_diagnostics(0);
      render_delay_ms = 2000;
    } else if (std::strcmp(page, "albums") == 0) {
      ui->select_library_category_for_diagnostics(1);
      render_delay_ms = 2000;
    } else if (std::strcmp(page, "genres") == 0) {
      ui->select_library_category_for_diagnostics(2);
      render_delay_ms = 2000;
    } else if (std::strcmp(page, "composers") == 0) {
      ui->select_library_category_for_diagnostics(3);
      render_delay_ms = 2000;
    }
    if (context->client != nullptr) {
      char start_text[16]{};
      char filter[8]{};
      char search[65]{};
      if (httpd_query_key_value(query, "start", start_text, sizeof(start_text)) == ESP_OK) {
        char *end = nullptr;
        const unsigned long start = std::strtoul(start_text, &end, 10);
        if (end != start_text && end != nullptr && *end == '\0' && start <= INT32_MAX) {
          context->client->enqueue(
              {musicremote::CommandType::request_queue_page, static_cast<int32_t>(start)});
        }
      }
      if (httpd_query_key_value(query, "filter", filter, sizeof(filter)) == ESP_OK) {
        if (std::strcmp(filter, "#") == 0) {
          context->client->enqueue(
              {musicremote::CommandType::filter_queue,
               static_cast<int32_t>(musicremote::PlaylistIndex::SearchMode::non_alpha)});
        } else if (filter[0] != '\0' && filter[1] == '\0' &&
                   ((filter[0] >= 'A' && filter[0] <= 'Z') ||
                    (filter[0] >= 'a' && filter[0] <= 'z'))) {
          context->client->enqueue(
              {musicremote::CommandType::filter_queue,
               static_cast<int32_t>(musicremote::PlaylistIndex::SearchMode::prefix), 0,
               filter});
        }
      }
      if (httpd_query_key_value(query, "search", search, sizeof(search)) == ESP_OK &&
          search[0] != '\0') {
        context->client->enqueue(
            {musicremote::CommandType::filter_queue,
             static_cast<int32_t>(musicremote::PlaylistIndex::SearchMode::contains), 0,
             search});
      }
    }
    vTaskDelay(pdMS_TO_TICKS(render_delay_ms));
  }
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  if (ui == nullptr || !ui->capture_rgb565(pixels, width, height, stride)) {
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "LVGL snapshot failed");
  }

  httpd_resp_set_type(request, "image/x-portable-pixmap");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  char header[40];
  const int header_length =
      std::snprintf(header, sizeof(header), "P6\n%lu %lu\n255\n",
                    static_cast<unsigned long>(width), static_cast<unsigned long>(height));
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_resp_send_chunk(request, header, header_length));

  std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *source = pixels.data() + static_cast<size_t>(y) * stride;
    for (uint32_t x = 0; x < width; ++x) {
      const uint16_t value = static_cast<uint16_t>(source[x * 2]) |
                             (static_cast<uint16_t>(source[x * 2 + 1]) << 8);
      row[x * 3] = static_cast<uint8_t>(((value >> 11) & 0x1f) * 255 / 31);
      row[x * 3 + 1] = static_cast<uint8_t>(((value >> 5) & 0x3f) * 255 / 63);
      row[x * 3 + 2] = static_cast<uint8_t>((value & 0x1f) * 255 / 31);
    }
    if (httpd_resp_send_chunk(request, reinterpret_cast<const char *>(row.data()), row.size()) !=
        ESP_OK) {
      return ESP_FAIL;
    }
  }
  return httpd_resp_send_chunk(request, nullptr, 0);
}

void start_diagnostics(musicremote::MusicUi &ui, musicremote::MonkeyMoteClient &client) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGW(kTag, "Could not start screenshot server");
    return;
  }
  static DiagnosticContext context{.ui = &ui, .client = &client};
  const httpd_uri_t screenshot = {.uri = "/screenshot.ppm",
                                  .method = HTTP_GET,
                                  .handler = screenshot_handler,
                                  .user_ctx = &context};
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &screenshot));
  ESP_LOGI(kTag, "Screenshot endpoint: http://musicremote.local/screenshot.ppm");
}

void wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto *event = static_cast<const ip_event_got_ip_t *>(data);
    ESP_LOGI(kTag, "Wi-Fi connected: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(wifi_events, kWifiConnected);
  }
}

bool connect_wifi(const musicremote::DeviceSettingsData &settings) {
  if (!musicremote::DeviceSettings::valid_wifi_ssid(settings.wifi_ssid) ||
      !musicremote::DeviceSettings::valid_wifi_password(settings.wifi_password)) {
    ESP_LOGE(kTag, "Stored Wi-Fi settings are invalid");
    return false;
  }
  wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *station = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(station, "musicremote");

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&config));
  esp_event_handler_instance_t wifi_instance;
  esp_event_handler_instance_t ip_instance;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event,
                                                       nullptr, &wifi_instance));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event,
                                                       nullptr, &ip_instance));

  wifi_config_t wifi_config{};
  std::memcpy(wifi_config.sta.ssid, settings.wifi_ssid.data(), settings.wifi_ssid.size());
  std::memcpy(wifi_config.sta.password, settings.wifi_password.data(),
              settings.wifi_password.size());
  wifi_config.sta.threshold.authmode = settings.wifi_password.empty() ? WIFI_AUTH_OPEN
                                                                      : WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());

  const EventBits_t bits = xEventGroupWaitBits(wifi_events, kWifiConnected, pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(30000));
  return (bits & kWifiConnected) != 0;
}

} // namespace

extern "C" void app_main() {
  esp_err_t nvs_status = nvs_flash_init();
  if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES || nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(nvs_status);
  }

  static musicremote::DeviceSettings settings;
  settings.load({.wifi_ssid = MUSICREMOTE_WIFI_SSID,
                 .wifi_password = MUSICREMOTE_WIFI_PASSWORD,
                 .server_host = MUSICREMOTE_AIMP_HOST,
                 .server_port = static_cast<uint16_t>(MUSICREMOTE_AIMP_PORT),
                 .server_password = MUSICREMOTE_AIMP_PASSWORD,
                 .brightness = 82,
                 .auto_blank_seconds = 300});
  const musicremote::DeviceSettingsData boot_settings = settings.snapshot();

  auto &board = espp::SmartPanleeSc01Plus::get();
  if (!board.initialize_lcd() || !board.initialize_display(board.display_width() * 40)) {
    ESP_LOGE(kTag, "SC01 Plus display initialization failed");
    return;
  }
  // The panel is physically 320x480. The espp board enum calls rotation 0
  // "landscape", but LVGL correctly reports that geometry as portrait. Rotate
  // once here so both the 480x320 UI and BSP touch conversion use landscape.
  lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);
  board.brightness(static_cast<float>(boot_settings.brightness));

  uint32_t sd_size_mb = 0;
  uint32_t sd_free_mb = 0;
  bool sd_ready = false;
  const bool flash_only = std::strcmp(MUSICREMOTE_PLAYLIST_STORAGE, "flash") == 0;
  const bool sd_required = std::strcmp(MUSICREMOTE_PLAYLIST_STORAGE, "sd") == 0;
  if (!flash_only) {
    for (int attempt = 0; attempt < 3 && !sd_ready; ++attempt) {
      sd_ready = board.initialize_sdcard({.format_if_mount_failed = false, .max_files = 8});
      if (!sd_ready) {
        vTaskDelay(pdMS_TO_TICKS(400));
      }
    }
  }
  if (!sd_ready) {
    ESP_LOGW(kTag, "microSD unavailable; large-playlist indexing will remain disabled");
  } else if (board.get_sd_card_info(&sd_size_mb, &sd_free_mb)) {
    ESP_LOGI(kTag, "microSD mounted at %s: %" PRIu32 " MB total, %" PRIu32 " MB free",
             espp::SmartPanleeSc01Plus::mount_point, sd_size_mb, sd_free_mb);
  } else {
    ESP_LOGW(kTag, "microSD mounted at %s but its capacity query failed",
             espp::SmartPanleeSc01Plus::mount_point);
  }

  bool flash_ready = false;
  if (!sd_ready && !sd_required) {
    const esp_vfs_spiffs_conf_t flash_config{.base_path = "/flash",
                                             .partition_label = "storage",
                                             .max_files = 8,
                                             .format_if_mount_failed = true};
    flash_ready = esp_vfs_spiffs_register(&flash_config) == ESP_OK;
    if (flash_ready) {
      size_t total = 0;
      size_t used = 0;
      if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(kTag, "Internal playlist storage mounted: %u KB total, %u KB free",
                 static_cast<unsigned>(total / 1024),
                 static_cast<unsigned>((total - used) / 1024));
      }
    } else {
      ESP_LOGW(kTag, "Internal playlist storage unavailable");
    }
  }

  const char *storage_directory = sd_ready ? "/sdcard/mremote"
                                  : flash_ready ? "/flash/mremote"
                                                : "";

  static musicremote::Model model;
  model.update([](musicremote::Snapshot &state) {
    state.connection = musicremote::ConnectionState::wifi;
    state.status = "CONNECTING WIFI";
  });
  static musicremote::MonkeyMoteClient client(
      model, {.host = boot_settings.server_host,
              .port = boot_settings.server_port,
              .password = boot_settings.server_password,
              .storage_directory = storage_directory,
              .storage_internal_flash = flash_ready});
  static musicremote::MusicUi ui(
      model, client, settings,
      [&board](uint8_t brightness) { board.brightness(static_cast<float>(brightness)); });

  if (!board.initialize_touch()) {
    ESP_LOGW(kTag, "Touch controller initialization failed");
  }
  ui.start();

  const bool wifi_ready = connect_wifi(boot_settings);
  if (!wifi_ready) {
    model.update([](musicremote::Snapshot &state) {
      state.connection = musicremote::ConnectionState::offline;
      state.status = "WAITING FOR WIFI";
    });
    ESP_LOGW(kTag, "Wi-Fi not ready yet; reconnect continues in background");
  }
  mdns_init();
  mdns_hostname_set("musicremote");
  client.start();
  start_diagnostics(ui, client);
}

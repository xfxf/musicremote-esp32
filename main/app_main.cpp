#include "monkeymote_client.hpp"
#include "music_model.hpp"
#include "music_ui.hpp"

#include <cstring>
#include <cstdio>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
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
constexpr EventBits_t kWifiFailed = BIT1;
EventGroupHandle_t wifi_events;
int retry_count = 0;

esp_err_t screenshot_handler(httpd_req_t *request) {
  auto *ui = static_cast<musicremote::MusicUi *>(request->user_ctx);
  char query[32]{};
  char page[16]{};
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
    }
    vTaskDelay(pdMS_TO_TICKS(40));
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

void start_diagnostics(musicremote::MusicUi &ui) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGW(kTag, "Could not start screenshot server");
    return;
  }
  const httpd_uri_t screenshot = {.uri = "/screenshot.ppm",
                                  .method = HTTP_GET,
                                  .handler = screenshot_handler,
                                  .user_ctx = &ui};
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &screenshot));
  ESP_LOGI(kTag, "Screenshot endpoint: http://musicremote.local/screenshot.ppm");
}

void wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (retry_count++ < 12) {
      esp_wifi_connect();
    } else {
      xEventGroupSetBits(wifi_events, kWifiFailed);
    }
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    retry_count = 0;
    const auto *event = static_cast<const ip_event_got_ip_t *>(data);
    ESP_LOGI(kTag, "Wi-Fi connected: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(wifi_events, kWifiConnected);
  }
}

bool connect_wifi() {
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
  std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), MUSICREMOTE_WIFI_SSID,
               sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password), MUSICREMOTE_WIFI_PASSWORD,
               sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());

  const EventBits_t bits = xEventGroupWaitBits(wifi_events, kWifiConnected | kWifiFailed, pdFALSE,
                                                pdFALSE, pdMS_TO_TICKS(30000));
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

  auto &board = espp::SmartPanleeSc01Plus::get();
  if (!board.initialize_lcd() || !board.initialize_display(board.display_width() * 40)) {
    ESP_LOGE(kTag, "SC01 Plus display initialization failed");
    return;
  }
  // The panel is physically 320x480. The espp board enum calls rotation 0
  // "landscape", but LVGL correctly reports that geometry as portrait. Rotate
  // once here so both the 480x320 UI and BSP touch conversion use landscape.
  lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);
  board.brightness(82.0f);

  static musicremote::Model model;
  model.update([](musicremote::Snapshot &state) {
    state.connection = musicremote::ConnectionState::wifi;
    state.status = "CONNECTING WIFI";
  });
  static musicremote::MonkeyMoteClient client(
      model, {.host = MUSICREMOTE_AIMP_HOST,
              .port = static_cast<uint16_t>(MUSICREMOTE_AIMP_PORT),
              .password = MUSICREMOTE_AIMP_PASSWORD});
  static musicremote::MusicUi ui(model, client);
  ui.start();

  if (!board.initialize_touch()) {
    ESP_LOGW(kTag, "Touch controller initialization failed");
  }
  if (!connect_wifi()) {
    model.update([](musicremote::Snapshot &state) {
      state.connection = musicremote::ConnectionState::offline;
      state.status = "WIFI UNAVAILABLE";
    });
    ESP_LOGE(kTag, "Could not connect to Wi-Fi");
    return;
  }
  mdns_init();
  mdns_hostname_set("musicremote");
  client.start();
  start_diagnostics(ui);
}

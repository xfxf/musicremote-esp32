#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "nvs.h"

namespace musicremote {

struct DeviceSettingsData {
  std::string wifi_ssid;
  std::string wifi_password;
  std::string server_host;
  uint16_t server_port{19371};
  std::string server_password;
  uint8_t brightness{82};
  uint32_t auto_blank_seconds{300};
  bool display_flipped{false};
};

class DeviceSettings {
public:
  DeviceSettings() = default;
  ~DeviceSettings();

  DeviceSettings(const DeviceSettings &) = delete;
  DeviceSettings &operator=(const DeviceSettings &) = delete;

  bool load(DeviceSettingsData defaults);
  DeviceSettingsData snapshot() const;
  bool persistence_available() const;

  bool set_wifi_ssid(const std::string &value);
  bool set_wifi_password(const std::string &value);
  bool set_server_host(const std::string &value);
  bool set_server_port(uint16_t value);
  bool set_server_password(const std::string &value);
  bool set_brightness(uint8_t value);
  bool set_auto_blank_seconds(uint32_t value);
  bool set_display_flipped(bool value);

  static bool valid_wifi_ssid(const std::string &value);
  static bool valid_wifi_password(const std::string &value);
  static bool valid_server_host(const std::string &value);
  static bool valid_server_password(const std::string &value);
  static bool valid_brightness(uint8_t value);
  static bool valid_auto_blank_seconds(uint32_t value);

private:
  bool set_string(const char *key, std::string DeviceSettingsData::*member,
                  const std::string &value, size_t maximum, bool allow_empty);
  bool commit_locked();
  bool seed_locked();
  bool load_string_locked(const char *key, std::string &destination, size_t maximum,
                          bool (*validator)(const std::string &));

  mutable std::mutex mutex_;
  DeviceSettingsData data_;
  nvs_handle_t handle_{0};
  bool persistence_available_{false};
};

} // namespace musicremote

#include "device_settings.hpp"
#include "settings_policy.hpp"

#include <vector>

#include "esp_log.h"

namespace musicremote {
namespace {

constexpr char kTag[] = "settings";
constexpr char kNamespace[] = "musicremote";
constexpr char kWifiSsid[] = "wifi_ssid";
constexpr char kWifiPassword[] = "wifi_pass";
constexpr char kServerHost[] = "aimp_host";
constexpr char kServerPort[] = "aimp_port";
constexpr char kServerPassword[] = "aimp_pass";
constexpr char kBrightness[] = "brightness";
constexpr char kAutoBlank[] = "auto_blank";

} // namespace

DeviceSettings::~DeviceSettings() {
  std::lock_guard lock(mutex_);
  if (handle_ != 0) {
    nvs_close(handle_);
  }
}

bool DeviceSettings::load(DeviceSettingsData defaults) {
  std::lock_guard lock(mutex_);
  if (!valid_wifi_ssid(defaults.wifi_ssid) ||
      !valid_wifi_password(defaults.wifi_password) ||
      !valid_server_host(defaults.server_host) ||
      !valid_server_password(defaults.server_password) || defaults.server_port == 0 ||
      !valid_brightness(defaults.brightness) ||
      !valid_auto_blank_seconds(defaults.auto_blank_seconds)) {
    ESP_LOGE(kTag, "Compiled setting defaults are invalid");
    return false;
  }
  data_ = std::move(defaults);
  if (nvs_open(kNamespace, NVS_READWRITE, &handle_) != ESP_OK) {
    ESP_LOGE(kTag, "Could not open persistent settings");
    handle_ = 0;
    return false;
  }

  load_string_locked(kWifiSsid, data_.wifi_ssid, 32, false);
  load_string_locked(kWifiPassword, data_.wifi_password, 64, true);
  load_string_locked(kServerHost, data_.server_host, 127, false);
  load_string_locked(kServerPassword, data_.server_password, 127, true);

  uint16_t port = data_.server_port;
  if (nvs_get_u16(handle_, kServerPort, &port) == ESP_OK && port != 0) {
    data_.server_port = port;
  }
  uint8_t brightness = data_.brightness;
  if (nvs_get_u8(handle_, kBrightness, &brightness) == ESP_OK &&
      valid_brightness(brightness)) {
    data_.brightness = brightness;
  }
  uint32_t auto_blank = data_.auto_blank_seconds;
  if (nvs_get_u32(handle_, kAutoBlank, &auto_blank) == ESP_OK &&
      valid_auto_blank_seconds(auto_blank)) {
    data_.auto_blank_seconds = auto_blank;
  }
  ESP_LOGI(kTag, "Persistent device settings loaded");
  return true;
}

DeviceSettingsData DeviceSettings::snapshot() const {
  std::lock_guard lock(mutex_);
  return data_;
}

bool DeviceSettings::valid_wifi_ssid(const std::string &value) {
  return settings_policy::valid_wifi_ssid(value);
}

bool DeviceSettings::valid_wifi_password(const std::string &value) {
  return settings_policy::valid_wifi_password(value);
}

bool DeviceSettings::valid_server_host(const std::string &value) {
  return settings_policy::valid_server_host(value);
}

bool DeviceSettings::valid_server_password(const std::string &value) {
  return settings_policy::valid_server_password(value);
}

bool DeviceSettings::valid_brightness(uint8_t value) {
  return settings_policy::valid_brightness(value);
}

bool DeviceSettings::valid_auto_blank_seconds(uint32_t value) {
  return settings_policy::valid_auto_blank_seconds(value);
}

void DeviceSettings::load_string_locked(const char *key, std::string &destination,
                                        size_t maximum, bool allow_empty) {
  size_t required = 0;
  if (nvs_get_str(handle_, key, nullptr, &required) != ESP_OK || required == 0 ||
      required > maximum + 1) {
    return;
  }
  std::vector<char> buffer(required);
  if (nvs_get_str(handle_, key, buffer.data(), &required) != ESP_OK) {
    return;
  }
  std::string value(buffer.data());
  if (settings_policy::valid_text(value, maximum, allow_empty)) {
    destination = std::move(value);
  }
}

bool DeviceSettings::commit_locked() {
  if (handle_ == 0 || nvs_commit(handle_) != ESP_OK) {
    ESP_LOGE(kTag, "Could not commit persistent settings");
    return false;
  }
  return true;
}

bool DeviceSettings::set_string(const char *key, std::string DeviceSettingsData::*member,
                                const std::string &value, size_t maximum,
                                bool allow_empty) {
  if (!settings_policy::valid_text(value, maximum, allow_empty)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (handle_ == 0 || nvs_set_str(handle_, key, value.c_str()) != ESP_OK || !commit_locked()) {
    return false;
  }
  data_.*member = value;
  return true;
}

bool DeviceSettings::set_wifi_ssid(const std::string &value) {
  return set_string(kWifiSsid, &DeviceSettingsData::wifi_ssid, value, 32, false);
}

bool DeviceSettings::set_wifi_password(const std::string &value) {
  return set_string(kWifiPassword, &DeviceSettingsData::wifi_password, value, 64, true);
}

bool DeviceSettings::set_server_host(const std::string &value) {
  return set_string(kServerHost, &DeviceSettingsData::server_host, value, 127, false);
}

bool DeviceSettings::set_server_password(const std::string &value) {
  return set_string(kServerPassword, &DeviceSettingsData::server_password, value, 127, true);
}

bool DeviceSettings::set_server_port(uint16_t value) {
  if (value == 0) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (handle_ == 0 || nvs_set_u16(handle_, kServerPort, value) != ESP_OK || !commit_locked()) {
    return false;
  }
  data_.server_port = value;
  return true;
}

bool DeviceSettings::set_brightness(uint8_t value) {
  if (!valid_brightness(value)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (handle_ == 0 || nvs_set_u8(handle_, kBrightness, value) != ESP_OK || !commit_locked()) {
    return false;
  }
  data_.brightness = value;
  return true;
}

bool DeviceSettings::set_auto_blank_seconds(uint32_t value) {
  if (!valid_auto_blank_seconds(value)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (handle_ == 0 || nvs_set_u32(handle_, kAutoBlank, value) != ESP_OK || !commit_locked()) {
    return false;
  }
  data_.auto_blank_seconds = value;
  return true;
}

} // namespace musicremote

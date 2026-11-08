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
constexpr char kSchemaVersion[] = "schema";
constexpr uint8_t kCurrentSchemaVersion = 1;

} // namespace

DeviceSettings::~DeviceSettings() {
  std::lock_guard lock(mutex_);
  if (handle_ != 0) {
    nvs_close(handle_);
  }
}

bool DeviceSettings::load(DeviceSettingsData defaults) {
  std::lock_guard lock(mutex_);
  persistence_available_ = false;
  if (handle_ != 0) {
    nvs_close(handle_);
    handle_ = 0;
  }
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

  uint8_t schema_version = 0;
  esp_err_t schema_status = nvs_get_u8(handle_, kSchemaVersion, &schema_version);
  if (schema_status == ESP_OK && schema_version > kCurrentSchemaVersion) {
    ESP_LOGE(kTag, "Settings schema %u is newer than supported schema %u; refusing downgrade",
             schema_version, kCurrentSchemaVersion);
    nvs_close(handle_);
    handle_ = 0;
    return false;
  }
  if (schema_status != ESP_OK && schema_status != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(kTag, "Repairing unreadable persistent settings schema: %s",
             esp_err_to_name(schema_status));
    if (nvs_erase_key(handle_, kSchemaVersion) != ESP_OK) {
      nvs_close(handle_);
      handle_ = 0;
      return false;
    }
    schema_status = ESP_ERR_NVS_NOT_FOUND;
  }

  bool complete = true;
  complete &= load_string_locked(kWifiSsid, data_.wifi_ssid, 32, valid_wifi_ssid);
  complete &= load_string_locked(kWifiPassword, data_.wifi_password, 64, valid_wifi_password);
  complete &= load_string_locked(kServerHost, data_.server_host, 127, valid_server_host);
  complete &=
      load_string_locked(kServerPassword, data_.server_password, 127, valid_server_password);

  uint16_t port = data_.server_port;
  const esp_err_t port_status = nvs_get_u16(handle_, kServerPort, &port);
  if (port_status == ESP_OK && port != 0) {
    data_.server_port = port;
  } else {
    complete = false;
    if (port_status != ESP_OK && port_status != ESP_ERR_NVS_NOT_FOUND) {
      nvs_erase_key(handle_, kServerPort);
    }
  }
  uint8_t brightness = data_.brightness;
  const esp_err_t brightness_status = nvs_get_u8(handle_, kBrightness, &brightness);
  if (brightness_status == ESP_OK && valid_brightness(brightness)) {
    data_.brightness = brightness;
  } else {
    complete = false;
    if (brightness_status != ESP_OK && brightness_status != ESP_ERR_NVS_NOT_FOUND) {
      nvs_erase_key(handle_, kBrightness);
    }
  }
  uint32_t auto_blank = data_.auto_blank_seconds;
  const esp_err_t auto_blank_status = nvs_get_u32(handle_, kAutoBlank, &auto_blank);
  if (auto_blank_status == ESP_OK && valid_auto_blank_seconds(auto_blank)) {
    data_.auto_blank_seconds = auto_blank;
  } else {
    complete = false;
    if (auto_blank_status != ESP_OK && auto_blank_status != ESP_ERR_NVS_NOT_FOUND) {
      nvs_erase_key(handle_, kAutoBlank);
    }
  }

  if (schema_status == ESP_ERR_NVS_NOT_FOUND ||
      (schema_status == ESP_OK && schema_version < kCurrentSchemaVersion) ||
      (schema_status == ESP_OK && schema_version == kCurrentSchemaVersion && !complete)) {
    // Upgrade pre-schema installs by preserving every valid key already read,
    // filling absent keys from this boot's defaults, and atomically committing
    // a complete snapshot. Later firmware builds can no longer replace fields
    // the user has not edited.
    if (!seed_locked()) {
      nvs_close(handle_);
      handle_ = 0;
      return false;
    }
    ESP_LOGI(kTag, "Persistent device settings initialized/migrated to schema %u",
             kCurrentSchemaVersion);
  } else if (schema_status != ESP_OK) {
    ESP_LOGE(kTag, "Could not read persistent settings schema: %s",
             esp_err_to_name(schema_status));
    nvs_close(handle_);
    handle_ = 0;
    return false;
  }
  persistence_available_ = true;
  ESP_LOGI(kTag, "Persistent device settings loaded");
  return true;
}

DeviceSettingsData DeviceSettings::snapshot() const {
  std::lock_guard lock(mutex_);
  return data_;
}

bool DeviceSettings::persistence_available() const {
  std::lock_guard lock(mutex_);
  return persistence_available_;
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

bool DeviceSettings::load_string_locked(const char *key, std::string &destination,
                                        size_t maximum,
                                        bool (*validator)(const std::string &)) {
  size_t required = 0;
  const esp_err_t size_status = nvs_get_str(handle_, key, nullptr, &required);
  if (size_status != ESP_OK || required == 0 || required > maximum + 1) {
    if (size_status != ESP_OK && size_status != ESP_ERR_NVS_NOT_FOUND) {
      nvs_erase_key(handle_, key);
    }
    return false;
  }
  std::vector<char> buffer(required);
  if (nvs_get_str(handle_, key, buffer.data(), &required) != ESP_OK) {
    nvs_erase_key(handle_, key);
    return false;
  }
  std::string value(buffer.data());
  if (settings_policy::valid_text(value, maximum, true) && validator(value)) {
    destination = std::move(value);
    return true;
  } else {
    ESP_LOGW(kTag, "Ignoring invalid persistent setting '%s'", key);
    return false;
  }
}

bool DeviceSettings::commit_locked() {
  if (handle_ == 0 || nvs_commit(handle_) != ESP_OK) {
    persistence_available_ = false;
    ESP_LOGE(kTag, "Could not commit persistent settings");
    return false;
  }
  persistence_available_ = true;
  return true;
}

bool DeviceSettings::seed_locked() {
  if (handle_ == 0 || nvs_set_str(handle_, kWifiSsid, data_.wifi_ssid.c_str()) != ESP_OK ||
      nvs_set_str(handle_, kWifiPassword, data_.wifi_password.c_str()) != ESP_OK ||
      nvs_set_str(handle_, kServerHost, data_.server_host.c_str()) != ESP_OK ||
      nvs_set_u16(handle_, kServerPort, data_.server_port) != ESP_OK ||
      nvs_set_str(handle_, kServerPassword, data_.server_password.c_str()) != ESP_OK ||
      nvs_set_u8(handle_, kBrightness, data_.brightness) != ESP_OK ||
      nvs_set_u32(handle_, kAutoBlank, data_.auto_blank_seconds) != ESP_OK ||
      nvs_set_u8(handle_, kSchemaVersion, kCurrentSchemaVersion) != ESP_OK) {
    persistence_available_ = false;
    ESP_LOGE(kTag, "Could not initialize persistent settings");
    return false;
  }
  return commit_locked();
}

bool DeviceSettings::set_string(const char *key, std::string DeviceSettingsData::*member,
                                const std::string &value, size_t maximum,
                                bool allow_empty) {
  if (!settings_policy::valid_text(value, maximum, allow_empty)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (handle_ == 0 || nvs_set_str(handle_, key, value.c_str()) != ESP_OK) {
    persistence_available_ = false;
    ESP_LOGE(kTag, "Could not write persistent setting '%s'", key);
    return false;
  }
  if (!commit_locked()) {
    return false;
  }
  data_.*member = value;
  return true;
}

bool DeviceSettings::set_wifi_ssid(const std::string &value) {
  return set_string(kWifiSsid, &DeviceSettingsData::wifi_ssid, value, 32, false);
}

bool DeviceSettings::set_wifi_password(const std::string &value) {
  if (!valid_wifi_password(value)) {
    return false;
  }
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
  if (handle_ == 0 || nvs_set_u16(handle_, kServerPort, value) != ESP_OK) {
    persistence_available_ = false;
    ESP_LOGE(kTag, "Could not write persistent server port");
    return false;
  }
  if (!commit_locked()) {
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
  if (handle_ == 0 || nvs_set_u8(handle_, kBrightness, value) != ESP_OK) {
    persistence_available_ = false;
    ESP_LOGE(kTag, "Could not write persistent brightness");
    return false;
  }
  if (!commit_locked()) {
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
  if (handle_ == 0 || nvs_set_u32(handle_, kAutoBlank, value) != ESP_OK) {
    persistence_available_ = false;
    ESP_LOGE(kTag, "Could not write persistent screen timeout");
    return false;
  }
  if (!commit_locked()) {
    return false;
  }
  data_.auto_blank_seconds = value;
  return true;
}

} // namespace musicremote

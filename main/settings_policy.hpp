#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace musicremote::settings_policy {

inline bool valid_text(std::string_view value, size_t maximum, bool allow_empty) {
  return value.size() <= maximum && (allow_empty || !value.empty()) &&
         value.find('\0') == std::string_view::npos;
}

inline bool valid_wifi_ssid(std::string_view value) { return valid_text(value, 32, false); }
inline bool valid_wifi_password(std::string_view value) {
  if (value.empty()) {
    return true;
  }
  if (value.size() == 64) {
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
      return (character >= '0' && character <= '9') ||
             (character >= 'a' && character <= 'f') ||
             (character >= 'A' && character <= 'F');
    });
  }
  return value.size() >= 8 && value.size() <= 63 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return character >= 0x20 && character <= 0x7e;
         });
}
inline bool valid_server_host(std::string_view value) { return valid_text(value, 127, false); }
inline bool valid_server_password(std::string_view value) { return valid_text(value, 127, true); }
inline bool valid_brightness(uint8_t value) { return value >= 5 && value <= 100; }

inline constexpr std::array<uint32_t, 5> auto_blank_seconds{0, 30, 60, 300, 900};

inline bool valid_auto_blank_seconds(uint32_t value) {
  return std::find(auto_blank_seconds.begin(), auto_blank_seconds.end(), value) !=
         auto_blank_seconds.end();
}

inline uint32_t next_auto_blank_seconds(uint32_t current) {
  const auto iterator = std::find(auto_blank_seconds.begin(), auto_blank_seconds.end(), current);
  if (iterator == auto_blank_seconds.end()) {
    return auto_blank_seconds.front();
  }
  const size_t index = static_cast<size_t>(iterator - auto_blank_seconds.begin());
  return auto_blank_seconds[(index + 1) % auto_blank_seconds.size()];
}

} // namespace musicremote::settings_policy

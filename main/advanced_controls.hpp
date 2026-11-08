#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace musicremote::advanced {

constexpr uint64_t kComputerPowerCapability = 1ULL << 17;
constexpr uint64_t kEqualizerCapability = 1ULL << 21;
constexpr uint64_t kSleepTimerCapability = 1ULL << 22;
constexpr size_t kMaximumEqualizerBands = 32;
constexpr size_t kMaximumEqualizerPresets = 128;

struct EqualizerBand {
  uint16_t frequency_hz{0};
  // MonkeyMote encodes -15.0 .. +15.0 dB as 0 .. 300.
  uint16_t gain_tenths_offset{150};
};

struct EqualizerState {
  bool known{false};
  bool enabled{false};
  uint16_t preamp_tenths_offset{150};
  std::vector<EqualizerBand> bands;
};

struct SleepTimerState {
  bool known{false};
  bool enabled{false};
  // Trigger: 1 = after the duration, 2 = at the end of the current track.
  uint8_t trigger{1};
  // Action: 1 = stop playback, 2 = exit player, 3 = shut down computer.
  uint8_t action{1};
  uint32_t seconds_remaining{0};
};

inline uint16_t read_u16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

inline uint32_t read_u32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

inline void write_u16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

inline void append_u32(std::vector<uint8_t> &data, uint32_t value) {
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value >> 16));
  data.push_back(static_cast<uint8_t>(value >> 24));
}

inline bool parse_equalizer(const std::vector<uint8_t> &payload,
                            EqualizerState &state) {
  if (payload.size() < 10) {
    return false;
  }
  const size_t count = payload[0];
  if (count > kMaximumEqualizerBands || payload.size() != 10 + count * 4 ||
      payload[1] > 1 || payload[2] != 15 || payload[3] != 0 ||
      read_u16(payload.data() + 4) != 300 ||
      read_u16(payload.data() + 8) != 0) {
    return false;
  }
  EqualizerState parsed;
  parsed.known = true;
  parsed.enabled = payload[1] != 0;
  parsed.preamp_tenths_offset = read_u16(payload.data() + 6);
  if (parsed.preamp_tenths_offset > 300) {
    return false;
  }
  parsed.bands.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const size_t offset = 10 + index * 4;
    EqualizerBand band{.frequency_hz = read_u16(payload.data() + offset),
                       .gain_tenths_offset = read_u16(payload.data() + offset + 2)};
    if (band.frequency_hz == 0 || band.gain_tenths_offset > 300) {
      return false;
    }
    parsed.bands.push_back(band);
  }
  state = std::move(parsed);
  return true;
}

inline bool set_equalizer_gain(std::vector<uint8_t> &payload, int32_t band_index,
                               uint16_t gain_tenths_offset) {
  EqualizerState parsed;
  if (gain_tenths_offset > 300 || !parse_equalizer(payload, parsed)) {
    return false;
  }
  if (band_index == -1) {
    write_u16(payload.data() + 6, gain_tenths_offset);
    return true;
  }
  if (band_index < 0 || static_cast<size_t>(band_index) >= parsed.bands.size()) {
    return false;
  }
  write_u16(payload.data() + 12 + static_cast<size_t>(band_index) * 4,
            gain_tenths_offset);
  return true;
}

inline bool parse_equalizer_presets(const std::vector<uint8_t> &payload,
                                    std::vector<std::string> &presets) {
  if (payload.size() < 4) {
    return false;
  }
  const size_t count = read_u16(payload.data());
  if (count > kMaximumEqualizerPresets || payload[2] != 0 || payload[3] != 0) {
    return false;
  }
  std::vector<std::string> parsed;
  parsed.reserve(count);
  size_t offset = 4;
  for (size_t index = 0; index < count; ++index) {
    const auto start = payload.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = std::find(start, payload.end(), uint8_t{0});
    if (end == payload.end() || end - start > 255) {
      return false;
    }
    parsed.emplace_back(start, end);
    offset = static_cast<size_t>(end - payload.begin()) + 1;
  }
  if (offset != payload.size()) {
    return false;
  }
  presets = std::move(parsed);
  return true;
}

inline bool parse_sleep_timer(const std::vector<uint8_t> &payload,
                              SleepTimerState &state) {
  if (payload.size() != 8 || (payload[1] != 1 && payload[1] != 2) ||
      payload[2] < 1 || payload[2] > 3) {
    return false;
  }
  state = {.known = true,
           .enabled = payload[0] != 0,
           .trigger = payload[1],
           .action = payload[2],
           .seconds_remaining = read_u32(payload.data() + 4)};
  return true;
}

inline std::vector<uint8_t> make_state_request(uint8_t subcommand) {
  if (subcommand < 17 || subcommand > 19) {
    return {};
  }
  std::vector<uint8_t> payload{subcommand};
  append_u32(payload, 0);
  return payload;
}

inline std::vector<uint8_t> make_sleep_timer(bool enabled, uint8_t trigger,
                                             uint8_t action, uint32_t seconds) {
  if (trigger < 1 || trigger > 2 || action < 1 || action > 3) {
    return {};
  }
  std::vector<uint8_t> payload{static_cast<uint8_t>(enabled), trigger, action, 0};
  append_u32(payload, seconds);
  return payload;
}

inline std::vector<uint8_t> make_computer_power(uint8_t action) {
  if (action > 2) {
    return {};
  }
  return {14, action};
}

} // namespace musicremote::advanced

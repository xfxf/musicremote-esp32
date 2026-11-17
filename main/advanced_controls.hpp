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

enum class StateAvailability : uint8_t {
  not_requested,
  loading,
  ready,
  unavailable,
};

constexpr uint8_t kEqualizerStateRequest = 1U << 0;
constexpr uint8_t kEqualizerPresetsRequest = 1U << 1;
constexpr uint8_t kSleepTimerStateRequest = 1U << 2;

struct AvailabilityState {
  StateAvailability equalizer{StateAvailability::not_requested};
  StateAvailability equalizer_presets{StateAvailability::not_requested};
  StateAvailability sleep_timer{StateAvailability::not_requested};

  uint8_t begin(uint8_t sent_mask, uint8_t failed_mask) {
    uint8_t changed = 0;
    update(kEqualizerStateRequest, sent_mask, failed_mask, equalizer, changed);
    update(kEqualizerPresetsRequest, sent_mask, failed_mask, equalizer_presets,
           changed);
    update(kSleepTimerStateRequest, sent_mask, failed_mask, sleep_timer, changed);
    return changed;
  }

  uint8_t complete(uint8_t response_mask) {
    uint8_t changed = 0;
    complete_one(kEqualizerStateRequest, response_mask, equalizer, changed);
    complete_one(kEqualizerPresetsRequest, response_mask, equalizer_presets,
                 changed);
    complete_one(kSleepTimerStateRequest, response_mask, sleep_timer, changed);
    return changed;
  }

  uint8_t expire(uint8_t expired_mask) {
    uint8_t changed = 0;
    expire_one(kEqualizerStateRequest, expired_mask, equalizer, changed);
    expire_one(kEqualizerPresetsRequest, expired_mask, equalizer_presets,
               changed);
    expire_one(kSleepTimerStateRequest, expired_mask, sleep_timer, changed);
    return changed;
  }

  void reset() { *this = {}; }

private:
  static void update(uint8_t bit, uint8_t sent_mask, uint8_t failed_mask,
                     StateAvailability &availability, uint8_t &changed) {
    StateAvailability next = availability;
    if ((sent_mask & bit) != 0) {
      next = StateAvailability::loading;
    } else if ((failed_mask & bit) != 0) {
      next = StateAvailability::unavailable;
    }
    if (next != availability) {
      availability = next;
      changed |= bit;
    }
  }

  static void complete_one(uint8_t bit, uint8_t response_mask,
                           StateAvailability &availability, uint8_t &changed) {
    if ((response_mask & bit) != 0 && availability != StateAvailability::ready) {
      availability = StateAvailability::ready;
      changed |= bit;
    }
  }

  static void expire_one(uint8_t bit, uint8_t expired_mask,
                         StateAvailability &availability, uint8_t &changed) {
    if ((expired_mask & bit) != 0 && availability == StateAvailability::loading) {
      availability = StateAvailability::unavailable;
      changed |= bit;
    }
  }
};

// Advanced state replies have no request identifier. Track only the current
// outstanding response kinds; any valid reply is authoritative when received.
class RequestTracker {
public:
  void begin(uint8_t request_mask, int64_t deadline_us) {
    pending_mask_ = request_mask;
    deadline_us_ = request_mask == 0 ? 0 : deadline_us;
  }

  void complete(uint8_t request_mask) {
    pending_mask_ &= static_cast<uint8_t>(~request_mask);
    if (pending_mask_ == 0) {
      deadline_us_ = 0;
    }
  }

  uint8_t expire(int64_t now_us) {
    if (deadline_us_ == 0 || now_us < deadline_us_) {
      return 0;
    }
    const uint8_t expired = pending_mask_;
    reset();
    return expired;
  }

  // Moves an outstanding deadline while a large frame blocks the stream.
  void defer(int64_t delta_us) {
    if (deadline_us_ != 0 && delta_us > 0) {
      deadline_us_ += delta_us;
    }
  }

  void reset() {
    pending_mask_ = 0;
    deadline_us_ = 0;
  }

  [[nodiscard]] uint8_t pending_mask() const { return pending_mask_; }
  [[nodiscard]] int64_t deadline_us() const { return deadline_us_; }

private:
  uint8_t pending_mask_{0};
  int64_t deadline_us_{0};
};

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

inline bool sleep_timer_mutation_allowed(StateAvailability availability,
                                         const SleepTimerState &state) {
  return availability == StateAvailability::ready && state.known;
}

// Every gain edit triggers a readback. An already validated band layout stays
// editable while it loads, so a second quick drag is neither rebuilt away nor
// discarded on release; the edit overlay keeps the readback from reverting it.
inline bool equalizer_mutation_allowed(StateAvailability availability,
                                       const EqualizerState &state) {
  return (availability == StateAvailability::ready ||
          availability == StateAvailability::loading) &&
         state.known;
}

// Gain sliders the equalizer page shows: the preamp plus one per band, or none
// while it can only show a status message.
inline size_t equalizer_slider_count(StateAvailability availability,
                                     const EqualizerState &state) {
  return equalizer_mutation_allowed(availability, state) ? state.bands.size() + 1
                                                         : 0;
}

enum class EqualizerRefresh { rebuild, update_in_place, defer };

// An equalizer reply can arrive while a gain slider is being dragged.
// Rebuilding deletes that slider and with it the command sent on release, so
// keep matching rows and update them in place, and postpone a structural
// rebuild until no slider is pressed.
inline EqualizerRefresh equalizer_refresh_action(size_t rendered_sliders,
                                                 size_t sliders,
                                                 bool slider_pressed) {
  if (sliders != 0 && sliders == rendered_sliders) {
    return EqualizerRefresh::update_in_place;
  }
  return slider_pressed ? EqualizerRefresh::defer : EqualizerRefresh::rebuild;
}

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

// The UI encodes end-of-track (trigger 2) with a zero duration. Only a
// zero-duration trigger-1 command means "off".
inline bool sleep_timer_command_enabled(int32_t seconds, uint8_t trigger) {
  return seconds > 0 || trigger == 2;
}

// Type-48 replies carry no request identifier, and a readback sent before a
// gain edit returns the pre-edit gains. Each sent edit is kept for a bounded
// period and applied over every reply received meanwhile, so the next setter,
// which carries all bands, cannot revert an earlier edit.
class EqualizerEditOverlay {
public:
  void record(int32_t band_index, uint16_t gain_tenths_offset, int64_t expires_us) {
    for (auto &edit : edits_) {
      if (edit.band_index == band_index) {
        edit.gain_tenths_offset = gain_tenths_offset;
        edit.expires_us = expires_us;
        return;
      }
    }
    if (edits_.size() <= kMaximumEqualizerBands) {
      edits_.push_back({band_index, gain_tenths_offset, expires_us});
    }
  }

  // Applies unexpired edits to a validated reply. Returns whether it changed.
  bool apply(std::vector<uint8_t> &payload, int64_t now_us) {
    EqualizerState reply;
    if (!parse_equalizer(payload, reply)) {
      return false;
    }
    bool changed = false;
    for (auto edit = edits_.begin(); edit != edits_.end();) {
      const bool preamp = edit->band_index == -1;
      const bool in_range =
          preamp || (edit->band_index >= 0 &&
                     static_cast<size_t>(edit->band_index) < reply.bands.size());
      if (now_us >= edit->expires_us || !in_range) {
        edit = edits_.erase(edit);
        continue;
      }
      const uint16_t reported =
          preamp ? reply.preamp_tenths_offset
                 : reply.bands[static_cast<size_t>(edit->band_index)].gain_tenths_offset;
      if (reported != edit->gain_tenths_offset &&
          set_equalizer_gain(payload, edit->band_index, edit->gain_tenths_offset)) {
        changed = true;
      }
      ++edit;
    }
    return changed;
  }

  // A reply queued behind a large frame arrives late but is still stale, so
  // the overlay's lifetime pauses with the stream (connection_policy.hpp).
  void defer(int64_t delta_us) {
    if (delta_us <= 0) {
      return;
    }
    for (auto &edit : edits_) {
      edit.expires_us += delta_us;
    }
  }

  void reset() { edits_.clear(); }
  [[nodiscard]] size_t size() const { return edits_.size(); }

private:
  struct Edit {
    int32_t band_index;
    uint16_t gain_tenths_offset;
    int64_t expires_us;
  };
  std::vector<Edit> edits_;
};

inline std::vector<uint8_t> make_computer_power(uint8_t action) {
  if (action > 2) {
    return {};
  }
  return {14, action};
}

} // namespace musicremote::advanced

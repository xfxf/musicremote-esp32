#include "advanced_controls.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using namespace musicremote::advanced;

int main() {
  std::vector<uint8_t> equalizer{
      2, 15, 0, 1, 44, 1, 150, 0, 0, 0,
      70, 0, 150, 0, 232, 3, 120, 0,
  };
  EqualizerState eq;
  assert(parse_equalizer(equalizer, eq));
  assert(eq.enabled && eq.bands.size() == 2);
  assert(eq.bands[1].frequency_hz == 1000);
  assert(eq.bands[1].gain_tenths_offset == 120);
  assert(set_equalizer_enabled(equalizer, false));
  assert((equalizer[3] & 1U) == 0);
  assert(set_equalizer_gain(equalizer, -1, 200));
  assert(set_equalizer_gain(equalizer, 1, 175));
  assert(read_u16(equalizer.data() + 6) == 200);
  assert(read_u16(equalizer.data() + 16) == 175);
  assert(!set_equalizer_gain(equalizer, 2, 150));
  auto malformed_equalizer = equalizer;
  malformed_equalizer.pop_back();
  assert(!parse_equalizer(malformed_equalizer, eq));
  malformed_equalizer = equalizer;
  malformed_equalizer[1] = 12;
  assert(!parse_equalizer(malformed_equalizer, eq));
  malformed_equalizer = equalizer;
  malformed_equalizer[8] = 1;
  assert(!parse_equalizer(malformed_equalizer, eq));

  std::vector<uint8_t> preset_payload{2, 0, 0, 0, 'F', 'l', 'a', 't', 0,
                                      'R', 'o', 'c', 'k', 0};
  std::vector<std::string> presets;
  assert(parse_equalizer_presets(preset_payload, presets));
  assert(presets.size() == 2 && presets[1] == "Rock");
  preset_payload.push_back(0);
  assert(!parse_equalizer_presets(preset_payload, presets));

  const auto sleep = make_sleep_timer(true, 1, 1, 900);
  SleepTimerState timer;
  assert(parse_sleep_timer(sleep, timer));
  assert(timer.enabled && timer.seconds_remaining == 900);
  assert(make_sleep_timer(true, 3, 1, 10).empty());
  assert(make_sleep_timer(true, 1, 4, 10).empty());

  const auto request = make_state_request(19);
  assert(request == std::vector<uint8_t>({19, 0, 0, 0, 0}));
  assert(make_state_request(16).empty());
  assert(make_computer_power(0) == std::vector<uint8_t>({14, 0}));
  assert(make_computer_power(3).empty());
}

#include "advanced_controls.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using namespace musicremote::advanced;

int main() {
  std::vector<uint8_t> equalizer{
      2, 1, 15, 0, 44, 1, 150, 0, 0, 0,
      70, 0, 150, 0, 232, 3, 120, 0,
  };
  EqualizerState eq;
  assert(parse_equalizer(equalizer, eq));
  assert(eq.enabled && eq.bands.size() == 2);
  assert(eq.bands[1].frequency_hz == 1000);
  assert(eq.bands[1].gain_tenths_offset == 120);
  assert(set_equalizer_gain(equalizer, -1, 200));
  assert(set_equalizer_gain(equalizer, 1, 175));
  assert(read_u16(equalizer.data() + 6) == 200);
  assert(read_u16(equalizer.data() + 16) == 175);
  assert(!set_equalizer_gain(equalizer, 2, 150));
  auto malformed_equalizer = equalizer;
  malformed_equalizer.pop_back();
  assert(!parse_equalizer(malformed_equalizer, eq));
  malformed_equalizer = equalizer;
  malformed_equalizer[2] = 12;
  assert(!parse_equalizer(malformed_equalizer, eq));
  malformed_equalizer = equalizer;
  malformed_equalizer[1] = 2;
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
  // End-of-track carries no duration but must still enable the timer.
  assert(sleep_timer_command_enabled(0, 2));
  assert(sleep_timer_command_enabled(900, 1));
  assert(!sleep_timer_command_enabled(0, 1));
  assert(!sleep_timer_command_enabled(-1, 1));

  {
    // A reply to a readback sent before an edit still reports the old gains.
    const std::vector<uint8_t> stale{
        2, 1, 15, 0, 44, 1, 150, 0, 0, 0,
        70, 0, 150, 0, 232, 3, 120, 0,
    };
    EqualizerEditOverlay overlay;
    overlay.record(0, 180, 100);
    overlay.record(-1, 160, 100);
    overlay.record(0, 190, 100);
    assert(overlay.size() == 2);
    auto merged = stale;
    assert(overlay.apply(merged, 50));
    EqualizerState merged_state;
    assert(parse_equalizer(merged, merged_state));
    assert(merged_state.bands[0].gain_tenths_offset == 190);
    assert(merged_state.bands[1].gain_tenths_offset == 120);
    assert(merged_state.preamp_tenths_offset == 160);
    // A reply that already reports the edits is left unchanged.
    auto confirmed = merged;
    assert(!overlay.apply(confirmed, 60));
    assert(confirmed == merged);
    // Expired edits are dropped and no longer override replies.
    auto later = stale;
    assert(!overlay.apply(later, 100));
    assert(later == stale && overlay.size() == 0);
    // Edits for bands absent from the reply are discarded.
    overlay.record(5, 200, 100);
    later = stale;
    assert(!overlay.apply(later, 10));
    assert(overlay.size() == 0);
    auto malformed = stale;
    malformed.pop_back();
    overlay.record(0, 200, 100);
    assert(!overlay.apply(malformed, 10));
    overlay.reset();
    assert(overlay.size() == 0);
  }

  const auto request = make_state_request(19);
  assert(request == std::vector<uint8_t>({19, 0, 0, 0, 0}));
  assert(make_state_request(16).empty());

  RequestTracker requests;
  const uint8_t all_requests = kEqualizerStateRequest |
                               kEqualizerPresetsRequest |
                               kSleepTimerStateRequest;
  requests.begin(all_requests, 100);
  assert(requests.pending_mask() == all_requests);
  assert(requests.deadline_us() == 100);
  requests.complete(kEqualizerStateRequest);
  assert(requests.pending_mask() ==
         (kEqualizerPresetsRequest | kSleepTimerStateRequest));
  assert(requests.expire(99) == 0);
  assert(requests.expire(100) ==
         (kEqualizerPresetsRequest | kSleepTimerStateRequest));
  assert(requests.pending_mask() == 0 && requests.deadline_us() == 0);

  // A retry gets a fresh deadline. A valid reply completes it even if an
  // earlier request for the same response kind already timed out.
  requests.begin(kSleepTimerStateRequest, 200);
  requests.complete(kSleepTimerStateRequest);
  assert(requests.expire(200) == 0);
  requests.begin(kSleepTimerStateRequest, 300);
  assert(requests.expire(300) == kSleepTimerStateRequest);
  requests.begin(all_requests, 400);
  requests.complete(kEqualizerStateRequest);
  requests.complete(kEqualizerPresetsRequest);
  requests.complete(kSleepTimerStateRequest);
  assert(requests.pending_mask() == 0 && requests.deadline_us() == 0);
  requests.begin(all_requests, 500);
  requests.reset();
  assert(requests.pending_mask() == 0 && requests.deadline_us() == 0);

  AvailabilityState availability;
  assert(availability.begin(all_requests, 0) == all_requests);
  assert(availability.equalizer == StateAvailability::loading);
  assert(availability.equalizer_presets == StateAvailability::loading);
  assert(availability.sleep_timer == StateAvailability::loading);
  assert(availability.complete(kEqualizerStateRequest) ==
         kEqualizerStateRequest);
  assert(availability.equalizer == StateAvailability::ready);
  assert(availability.expire(kEqualizerPresetsRequest |
                             kSleepTimerStateRequest) ==
         (kEqualizerPresetsRequest | kSleepTimerStateRequest));
  assert(availability.equalizer == StateAvailability::ready);
  assert(availability.equalizer_presets == StateAvailability::unavailable);
  assert(availability.sleep_timer == StateAvailability::unavailable);

  // A valid late response restores readiness. Retrying only selected request
  // kinds returns those controls to loading without disturbing ready state.
  assert(availability.complete(kSleepTimerStateRequest) ==
         kSleepTimerStateRequest);
  assert(availability.sleep_timer == StateAvailability::ready);
  assert(availability.begin(kEqualizerPresetsRequest |
                                kSleepTimerStateRequest,
                            0) ==
         (kEqualizerPresetsRequest | kSleepTimerStateRequest));
  assert(availability.equalizer == StateAvailability::ready);
  assert(availability.equalizer_presets == StateAvailability::loading);
  assert(availability.sleep_timer == StateAvailability::loading);
  assert(availability.complete(kSleepTimerStateRequest) ==
         kSleepTimerStateRequest);
  assert(availability.expire(kEqualizerPresetsRequest) ==
         kEqualizerPresetsRequest);
  availability.reset();
  assert(availability.equalizer == StateAvailability::not_requested);
  assert(availability.equalizer_presets == StateAvailability::not_requested);
  assert(availability.sleep_timer == StateAvailability::not_requested);

  SleepTimerState unknown_timer;
  assert(!sleep_timer_mutation_allowed(StateAvailability::loading,
                                       unknown_timer));
  assert(!sleep_timer_mutation_allowed(StateAvailability::ready,
                                       unknown_timer));
  timer.known = true;
  assert(sleep_timer_mutation_allowed(StateAvailability::ready, timer));
  assert(!sleep_timer_mutation_allowed(StateAvailability::unavailable, timer));
  EqualizerState known_equalizer;
  known_equalizer.known = true;
  assert(equalizer_mutation_allowed(StateAvailability::ready, known_equalizer));
  assert(!equalizer_mutation_allowed(StateAvailability::loading,
                                     known_equalizer));
  assert(!equalizer_mutation_allowed(StateAvailability::unavailable,
                                     known_equalizer));
  known_equalizer.bands = {{70, 150}, {180, 165}};
  assert(equalizer_slider_count(StateAvailability::ready, known_equalizer) == 3);
  assert(equalizer_slider_count(StateAvailability::loading, known_equalizer) == 0);
  // A reply with the same rows updates them in place, even mid-drag.
  assert(equalizer_refresh_action(3, 3, true) == EqualizerRefresh::update_in_place);
  assert(equalizer_refresh_action(3, 3, false) == EqualizerRefresh::update_in_place);
  // A structural change waits for the dragged slider to be released.
  assert(equalizer_refresh_action(3, 4, true) == EqualizerRefresh::defer);
  assert(equalizer_refresh_action(3, 0, true) == EqualizerRefresh::defer);
  assert(equalizer_refresh_action(3, 4, false) == EqualizerRefresh::rebuild);
  assert(equalizer_refresh_action(3, 0, false) == EqualizerRefresh::rebuild);
  // The status message has no sliders, so it is always rebuilt.
  assert(equalizer_refresh_action(0, 0, false) == EqualizerRefresh::rebuild);
  assert(equalizer_refresh_action(0, 3, false) == EqualizerRefresh::rebuild);
  assert(make_computer_power(0) == std::vector<uint8_t>({14, 0}));
  assert(make_computer_power(3).empty());
}

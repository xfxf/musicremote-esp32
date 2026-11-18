#include "connection_policy.hpp"

#include <cassert>
#include <cstdint>

using namespace musicremote::connection_policy;

int main() {
  // The client speaks at least every 4 s, well inside AIMP's ~10 s limit,
  // whether or not it is receiving data.
  assert(!keepalive_due(3'999'999, 0));
  assert(keepalive_due(4'000'000, 0));
  assert(kClientKeepaliveIntervalUs * 2 < 10'000'000);

  StreamPauseClock clock;
  // The first observation only starts the clock.
  assert(clock.advance(1'000, true) == 0);
  // Blocked at both ends: the whole interval is paused.
  assert(clock.advance(5'000, true) == 4'000);
  // The frame finished during this interval: still credited.
  assert(clock.advance(6'000, false) == 1'000);
  // Idle stream: deadlines run normally.
  assert(clock.advance(9'000, false) == 0);
  // A frame started during this interval.
  assert(clock.advance(9'500, true) == 500);
  // Time never runs backwards.
  assert(clock.advance(9'400, true) == 0);
  clock.reset();
  assert(clock.advance(20'000, true) == 0);

  // A 60 s large-frame transfer cannot expire a 15 s playlist deadline that
  // was armed just before the frame began.
  {
    StreamPauseClock transfer;
    int64_t deadline = 15'000'000;
    transfer.advance(0, false);
    for (int64_t now = 100'000; now <= 60'000'000; now += 100'000) {
      defer(deadline, transfer.advance(now, true));
      assert(now < deadline);
    }
    defer(deadline, transfer.advance(60'100'000, false));
    assert(deadline == 75'100'000);
  }

  int64_t none = 0;
  defer(none, 1'000);
  assert(none == 0);
  int64_t armed = 10;
  defer(armed, 0);
  defer(armed, -5);
  assert(armed == 10);
  defer(armed, 5);
  assert(armed == 15);
}

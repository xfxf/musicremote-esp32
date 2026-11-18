#pragma once

#include <cstdint>

// Timing rules for one AIMP session. Kept free of ESP-IDF types so the host
// tests in tools/test_connection_policy.cpp can exercise them.
namespace musicremote::connection_policy {

// AIMP sends a type-23 keepalive about every 6 s and closes a client that has
// sent it nothing for about 10 s (measured live, issue #48). A large playlist
// frame queues AIMP's own keepalives behind it, so the client cannot echo
// them until the frame is drained; it must speak first.
inline constexpr int64_t kClientKeepaliveIntervalUs = 4'000'000;

inline bool keepalive_due(int64_t now_us, int64_t last_send_us) {
  return now_us - last_send_us >= kClientKeepaliveIntervalUs;
}

// Request deadlines measure how long AIMP takes to answer. While a large
// frame is being received (streamed to storage or drained), every later reply
// is queued behind it on the same TCP stream, so that time must not count
// against any pending deadline. A stalled transfer is still caught by the
// session's no-data timeout.
class StreamPauseClock {
public:
  // Returns how far every pending deadline must move for the interval since
  // the previous call. An interval counts when the stream was blocked at
  // either end of it, which credits the whole transfer plus at most one loop
  // iteration.
  int64_t advance(int64_t now_us, bool stream_blocked) {
    int64_t delta = 0;
    if (started_ && (stream_blocked || previously_blocked_) && now_us > last_us_) {
      delta = now_us - last_us_;
    }
    started_ = true;
    last_us_ = now_us;
    previously_blocked_ = stream_blocked;
    return delta;
  }

  void reset() {
    started_ = false;
    previously_blocked_ = false;
    last_us_ = 0;
  }

private:
  bool started_{false};
  bool previously_blocked_{false};
  int64_t last_us_{0};
};

// Moves a pending deadline; zero means "no deadline" and stays zero.
inline void defer(int64_t &deadline_us, int64_t delta_us) {
  if (deadline_us != 0 && delta_us > 0) {
    deadline_us += delta_us;
  }
}

} // namespace musicremote::connection_policy

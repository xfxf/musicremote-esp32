#pragma once

#include <atomic>

namespace musicremote::index_worker_policy {

// Open the build gate before consuming the final supersession marker. A
// response that arrives just before the store is therefore observed by the
// exchange; one that arrives after it sees an open gate and is accepted.
inline bool finish(std::atomic<bool> &building, std::atomic<bool> &superseded,
                   bool previously_superseded) noexcept {
  building.store(false, std::memory_order_release);
  return superseded.exchange(false, std::memory_order_acq_rel) ||
         previously_superseded;
}

} // namespace musicremote::index_worker_policy

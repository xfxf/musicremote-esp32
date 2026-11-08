#include "index_worker_policy.hpp"

#include <atomic>
#include <cassert>

using musicremote::index_worker_policy::finish;

int main() {
  std::atomic<bool> building{true};
  std::atomic<bool> superseded{false};

  assert(!finish(building, superseded, false));
  assert(!building && !superseded);

  building = true;
  assert(finish(building, superseded, true));
  assert(!building && !superseded);

  // Models a response that marked the build superseded immediately before
  // the worker opened its gate. The final exchange must retain that retry.
  building = true;
  superseded = true;
  assert(finish(building, superseded, false));
  assert(!building && !superseded);

  // A consumed marker cannot leak into the next build.
  building = true;
  assert(!finish(building, superseded, false));
  assert(!building && !superseded);
}

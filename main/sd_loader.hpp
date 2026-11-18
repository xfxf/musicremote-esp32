#pragma once

namespace musicremote {

// Starts the USB microSD loader (issue #49). Installs the USB-Serial-JTAG
// driver, routes console output through it so logging and `idf.py monitor`
// keep working, and starts a low-priority task that accepts the framed upload
// protocol in sd_loader_policy.hpp. When sd_mounted is false every request is
// refused with "nosd". Returns false if the driver or task could not start;
// the rest of the firmware is unaffected either way.
bool start_sd_loader(bool sd_mounted);

}  // namespace musicremote

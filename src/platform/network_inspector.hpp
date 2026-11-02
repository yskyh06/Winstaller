#pragma once

#include "core/network_interface.hpp"

namespace litewin {

// Linux-only, read-only snapshot. Includes loopback, virtual, down, and
// addressless interfaces. Success with an empty inventory is not an error.
// Flags and IP addresses do not prove Internet/download-server connectivity.
// Missing/inaccessible sysfs classification information leaves kind unknown.
// Resource exhaustion can throw; OS inspection failures return an error.
[[nodiscard]] NetworkInventoryResult inspect_network_interfaces();

} // namespace litewin

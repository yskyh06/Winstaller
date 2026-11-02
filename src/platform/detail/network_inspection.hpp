#pragma once

#include "core/network_interface.hpp"

#include <filesystem>
#include <ifaddrs.h>

namespace litewin::detail {

// Internal test seam: synthetic ifaddrs lists and a temporary sysfs tree keep
// tests independent of network hardware, privileges, and network managers.
struct NetworkInspectionBackend {
    int (*get_addresses)(ifaddrs**);
    void (*free_addresses)(ifaddrs*);
    std::filesystem::path sys_class_net;
};

[[nodiscard]] NetworkInventoryResult inspect_network_interfaces_with(
    const NetworkInspectionBackend& backend);

} // namespace litewin::detail

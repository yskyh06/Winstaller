#pragma once

#include "platform/disk_inspector.hpp"
#include "platform/process_runner.hpp"

namespace litewin::detail {
// Test seam for read-only lsblk/wipefs calls; tests never access real devices.
using DiskCommandRunner = ProcessResult (*)(const std::string&,
                                           const std::vector<std::string>&,
                                           const std::string&);
[[nodiscard]] DiskInspectResult inspect_disks_with(DiskCommandRunner run);
} // namespace litewin::detail

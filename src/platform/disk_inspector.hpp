#pragma once

#include "core/disk_inventory.hpp"

#include <string>
#include <variant>

namespace litewin {

struct DiskInspectionError {
    std::string operation;
    std::string message;
};

using DiskInspectResult = std::variant<DiskInventory, DiskInspectionError>;

// Runs read-only lsblk and wipefs inspection via ProcessRunner.
// Missing/failed signature inspection leaves has_signatures unknown; such disks
// must not pass evaluate_disk_target(). Requires lsblk and wipefs (util-linux).
[[nodiscard]] DiskInspectResult inspect_disks();

} // namespace litewin

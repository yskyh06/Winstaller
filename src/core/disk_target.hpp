#pragma once

#include "core/disk_inventory.hpp"

#include <cstdint>
#include <string>

namespace litewin {

// Minimum required disk size: 32 GB (32 * 1024 * 1024 * 1024 bytes)
inline constexpr std::uint64_t kMinimumDiskSizeBytes = 32ULL * 1024 * 1024 * 1024;

enum class DiskRejectionReason {
    none,                 // Disk is eligible
    not_a_disk_type,      // loop, rom, dm, etc.
    too_small,            // Less than 32 GB
    mounted_in_use,       // Root, boot, or any mountpoints present (e.g. boot USB)
    usb_transport,        // USB removable / boot medium (excluded from target selection)
    has_partitions,       // Existing partitions found (clean disk required for v1)
    incomplete_metadata,  // Missing identity, mount state, or signature inspection
    existing_signatures,   // Filesystem, RAID, or partition-table signature found
};

struct DiskTargetEvaluation {
    bool is_eligible = false;
    DiskRejectionReason rejection_reason = DiskRejectionReason::none;
    std::string message;

    [[nodiscard]] bool can_install() const noexcept {
        return is_eligible;
    }
};

namespace detail {

inline bool has_any_mountpoint(const DiskInfo& dev) noexcept {
    if (dev.mountpoints.has_value() && !dev.mountpoints->empty()) {
        return true;
    }
    for (const auto& child : dev.children) {
        if (has_any_mountpoint(child)) {
            return true;
        }
    }
    return false;
}

} // namespace detail

[[nodiscard]] inline DiskTargetEvaluation evaluate_disk_target(const DiskInfo& disk) {
    DiskTargetEvaluation result;

    if (disk.name.empty() || disk.path.empty()) {
        result.is_eligible = false;
        result.rejection_reason = DiskRejectionReason::incomplete_metadata;
        result.message = "Device has incomplete metadata (missing name or path).";
        return result;
    }

    if (disk.type != "disk") {
        result.is_eligible = false;
        result.rejection_reason = DiskRejectionReason::not_a_disk_type;
        result.message = "Device is not a raw disk (type: " + disk.type + ").";
        return result;
    }

    if (disk.size_bytes < kMinimumDiskSizeBytes) {
        result.is_eligible = false;
        result.rejection_reason = DiskRejectionReason::too_small;
        result.message = "Disk size (" + std::to_string(disk.size_bytes / (1024 * 1024 * 1024)) +
                         " GB) is smaller than required 32 GB.";
        return result;
    }

    if (detail::has_any_mountpoint(disk)) {
        result.is_eligible = false;
        result.rejection_reason = DiskRejectionReason::mounted_in_use;
        result.message = "Disk or its subpartitions are mounted and in use by the system.";
        return result;
    }

    if (disk.transport.has_value() && (*disk.transport == "usb" || *disk.transport == "USB")) {
        result.is_eligible = false;
        result.rejection_reason = DiskRejectionReason::usb_transport;
        result.message = "USB removable media cannot be targeted for OS installation.";
        return result;
    }

    if (!disk.children.empty()) {
        result.is_eligible = false;
        result.rejection_reason = DiskRejectionReason::has_partitions;
        result.message = "Disk has existing partitions. Clean unallocated disk required for v1.";
        return result;
    }

    if (!disk.mountpoints || !disk.has_signatures.has_value()) {
        return {false, DiskRejectionReason::incomplete_metadata,
                "Mount state or disk signature inspection is unknown."};
    }
    if (*disk.has_signatures) {
        return {false, DiskRejectionReason::existing_signatures,
                "Disk contains existing filesystem, RAID, or partition-table signatures."};
    }

    // VMware / Virtual / Physical internal disks passing all checks
    result.is_eligible = true;
    result.rejection_reason = DiskRejectionReason::none;
    result.message = "Disk meets all safety and size criteria.";
    return result;
}

} // namespace litewin

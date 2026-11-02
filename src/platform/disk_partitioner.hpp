#pragma once

#include "core/partition_layout.hpp"

#include <string>
#include <system_error>

namespace litewin {

struct PartitionOperationResult {
    bool success = false;
    int exit_code = 0;
    std::string error_message;
    std::string details;
};

// Formats the partition path based on disk name (e.g. /dev/sda -> /dev/sda5, /dev/nvme0n1 -> /dev/nvme0n1p5)
[[nodiscard]] std::string get_partition_device_path(
    const std::string& disk_path,
    int partition_number
);

class DiskPartitioner {
public:
    // Applies GPT partitioning via sfdisk
    [[nodiscard]] static PartitionOperationResult apply_layout(
        const std::string& disk_path,
        const DiskLayout& layout,
        bool dry_run = false
    );

    // Formats a partition (supports exfat, fat32, ntfs)
    [[nodiscard]] static PartitionOperationResult format_partition(
        const std::string& partition_path,
        const std::string& filesystem,
        const std::string& label,
        bool dry_run = false
    );

    // Mounts the partition using Linux mount(2) syscall or mount command
    [[nodiscard]] static PartitionOperationResult mount_partition(
        const std::string& partition_path,
        const std::string& mount_point,
        const std::string& filesystem = "exfat",
        bool dry_run = false
    );

    // Unmounts the mount point using umount2(2)
    [[nodiscard]] static PartitionOperationResult unmount_partition(
        const std::string& mount_point,
        bool dry_run = false
    );
};

} // namespace litewin

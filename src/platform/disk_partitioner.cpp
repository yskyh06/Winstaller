#include "platform/disk_partitioner.hpp"
#include "platform/process_runner.hpp"

#include <cctype>
#include <filesystem>
#include <sys/mount.h>
#include <unistd.h>

namespace litewin {

std::string get_partition_device_path(const std::string& disk_path, int partition_number) {
    if (disk_path.empty()) {
        return "";
    }
    // If disk ends with a digit (e.g. /dev/nvme0n1, /dev/loop0, /dev/mmcblk0), append 'p'
    char last_char = disk_path.back();
    if (std::isdigit(static_cast<unsigned char>(last_char))) {
        return disk_path + "p" + std::to_string(partition_number);
    }
    // Otherwise standard append (e.g. /dev/sda -> /dev/sda1)
    return disk_path + std::to_string(partition_number);
}

PartitionOperationResult DiskPartitioner::apply_layout(
    const std::string& disk_path,
    const DiskLayout& layout,
    bool dry_run
) {
    PartitionOperationResult result;
    std::string script = generate_sfdisk_script(layout);
    result.details = script;

    if (dry_run) {
        result.success = true;
        result.exit_code = 0;
        return result;
    }

    auto proc = ProcessRunner::run("sfdisk", {"--force", disk_path}, script);
    result.success = proc.success();
    result.exit_code = proc.exit_code;
    result.error_message = proc.error_message.empty() ? proc.stderr_output : proc.error_message;

    if (result.success) {
        // Refresh kernel partition table and wait for udev device nodes
        (void)ProcessRunner::run("partprobe", {disk_path});
        (void)ProcessRunner::run("udevadm", {"settle"});
    }

    return result;
}

PartitionOperationResult DiskPartitioner::format_partition(
    const std::string& partition_path,
    const std::string& filesystem,
    const std::string& label,
    bool dry_run
) {
    PartitionOperationResult result;

    if (dry_run) {
        result.success = true;
        result.exit_code = 0;
        result.details = "Dry run: formatting " + partition_path + " as " + filesystem + " label=" + label;
        return result;
    }

    ProcessResult proc;
    if (filesystem == "exfat") {
        proc = ProcessRunner::run("mkfs.exfat", {"-n", label, partition_path});
    } else if (filesystem == "fat32") {
        proc = ProcessRunner::run("mkfs.fat", {"-F32", "-n", label, partition_path});
    } else if (filesystem == "ntfs") {
        proc = ProcessRunner::run("mkfs.ntfs", {"-f", "-L", label, partition_path});
    } else {
        result.success = false;
        result.error_message = "Unsupported filesystem for format: " + filesystem;
        return result;
    }

    result.success = proc.success();
    result.exit_code = proc.exit_code;
    result.error_message = proc.error_message.empty() ? proc.stderr_output : proc.error_message;
    result.details = proc.stdout_output;
    return result;
}

PartitionOperationResult DiskPartitioner::mount_partition(
    const std::string& partition_path,
    const std::string& mount_point,
    const std::string& filesystem,
    bool dry_run
) {
    PartitionOperationResult result;

    if (dry_run) {
        result.success = true;
        result.exit_code = 0;
        result.details = "Dry run: mounting " + partition_path + " to " + mount_point;
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(mount_point, ec);
    if (ec) {
        result.success = false;
        result.error_message = "Failed to create mount point directory: " + ec.message();
        return result;
    }

    // Ensure any stale mount at mount_point is unmounted first
    (void)::umount2(mount_point.c_str(), MNT_DETACH);

    std::string kernel_fs = filesystem;
    if (filesystem == "fat32") {
        kernel_fs = "vfat";
    } else if (filesystem == "ntfs") {
        kernel_fs = "ntfs3";
    }

    // Try direct Linux kernel mount(2) syscall first (with MS_NOATIME for fast file extraction)
    if (::mount(partition_path.c_str(), mount_point.c_str(), kernel_fs.c_str(), MS_NOATIME, nullptr) == 0) {
        result.success = true;
        result.exit_code = 0;
        return result;
    }

    // Fallback to external mount command with mapped filesystem type
    auto proc = ProcessRunner::run("mount", {"-t", kernel_fs, "-o", "noatime", partition_path, mount_point});
    if (!proc.success() && filesystem == "ntfs") {
        // Secondary fallback to FUSE ntfs-3g / auto mount
        proc = ProcessRunner::run("mount", {"-t", "ntfs-3g", "-o", "noatime,big_writes", partition_path, mount_point});
        if (!proc.success()) {
            proc = ProcessRunner::run("mount", {partition_path, mount_point});
        }
    }

    result.success = proc.success();
    result.exit_code = proc.exit_code;
    result.error_message = proc.error_message.empty() ? proc.stderr_output : proc.error_message;
    return result;
}

PartitionOperationResult DiskPartitioner::unmount_partition(
    const std::string& mount_point,
    bool dry_run
) {
    PartitionOperationResult result;

    if (dry_run) {
        result.success = true;
        result.exit_code = 0;
        result.details = "Dry run: unmounting " + mount_point;
        return result;
    }

    if (::umount2(mount_point.c_str(), MNT_FORCE) == 0) {
        result.success = true;
        result.exit_code = 0;
        return result;
    }

    auto proc = ProcessRunner::run("umount", {"-f", mount_point});
    result.success = proc.success();
    result.exit_code = proc.exit_code;
    result.error_message = proc.error_message.empty() ? proc.stderr_output : proc.error_message;
    return result;
}

} // namespace litewin

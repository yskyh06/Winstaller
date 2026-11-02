#include "core/partition_layout.hpp"
#include "platform/disk_partitioner.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

void test_partition_path_naming() {
    // SCSI / SATA style (ends with letter)
    assert(litewin::get_partition_device_path("/dev/sda", 1) == "/dev/sda1");
    assert(litewin::get_partition_device_path("/dev/sda", 5) == "/dev/sda5");
    assert(litewin::get_partition_device_path("/dev/vda", 4) == "/dev/vda4");

    // NVMe / MMC / Loop style (ends with digit)
    assert(litewin::get_partition_device_path("/dev/nvme0n1", 1) == "/dev/nvme0n1p1");
    assert(litewin::get_partition_device_path("/dev/nvme0n1", 5) == "/dev/nvme0n1p5");
    assert(litewin::get_partition_device_path("/dev/mmcblk0", 2) == "/dev/mmcblk0p2");
    assert(litewin::get_partition_device_path("/dev/loop0", 5) == "/dev/loop0p5");

    assert(litewin::get_partition_device_path("", 1).empty());
}

void test_dry_run_apply_layout() {
    auto layout_res = litewin::calculate_disk_layout(32ULL * 1024 * 1024 * 1024);
    assert(layout_res.success);

    auto res = litewin::DiskPartitioner::apply_layout("/dev/sda", layout_res.layout, /*dry_run=*/true);
    assert(res.success);
    assert(res.exit_code == 0);
    assert(res.details.find("label: gpt") != std::string::npos);
    assert(res.details.find("name=\"LitewinTemp\"") != std::string::npos);
}

void test_dry_run_format_partition() {
    auto res_exfat = litewin::DiskPartitioner::format_partition(
        "/dev/sda5", "exfat", "LitewinTemp", /*dry_run=*/true);
    assert(res_exfat.success);
    assert(res_exfat.details.find("exfat") != std::string::npos);

    auto res_invalid = litewin::DiskPartitioner::format_partition(
        "/dev/sda5", "invalid_fs", "test", /*dry_run=*/false);
    assert(!res_invalid.success);
    assert(!res_invalid.error_message.empty());
}

void test_dry_run_mount_unmount() {
    auto res_mount = litewin::DiskPartitioner::mount_partition(
        "/dev/sda5", "/mnt/litewin_temp", "exfat", /*dry_run=*/true);
    assert(res_mount.success);

    auto res_umount = litewin::DiskPartitioner::unmount_partition(
        "/mnt/litewin_temp", /*dry_run=*/true);
    assert(res_umount.success);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: disk_partitioner_tests <scenario>\n";
        return 1;
    }

    const std::string_view scenario = argv[1];
    if (scenario == "partition_path_naming") {
        test_partition_path_naming();
    } else if (scenario == "dry_run_apply_layout") {
        test_dry_run_apply_layout();
    } else if (scenario == "dry_run_format_partition") {
        test_dry_run_format_partition();
    } else if (scenario == "dry_run_mount_unmount") {
        test_dry_run_mount_unmount();
    } else {
        std::cerr << "Unknown scenario: " << scenario << "\n";
        return 1;
    }

    return 0;
}

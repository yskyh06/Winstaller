#include "core/disk_target.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

// 60 GB VMware Virtual SCSI Disk (Clean, no partitions)
void test_vmware_scsi_eligible() {
    litewin::DiskInfo dev{
        .name = "sda",
        .path = "/dev/sda",
        .type = "disk",
        .size_bytes = 60ULL * 1024 * 1024 * 1024,
        .model = "VMware Virtual S",
        .serial = "00000000000000000001",
        .transport = "scsi",
        .mountpoints = std::vector<std::string>{},
        .children = {},
        .has_signatures = false,
    };

    auto res = litewin::evaluate_disk_target(dev);
    assert(res.can_install());
    assert(res.rejection_reason == litewin::DiskRejectionReason::none);
}

// Exactly 32 GB VMware NVMe Disk
void test_vmware_nvme_32gb() {
    litewin::DiskInfo dev{
        .name = "nvme0n1",
        .path = "/dev/nvme0n1",
        .type = "disk",
        .size_bytes = 32ULL * 1024 * 1024 * 1024,
        .model = "VMware Virtual NVMe",
        .serial = "VMware-NVMe-1234",
        .transport = "nvme",
        .mountpoints = std::vector<std::string>{},
        .children = {},
        .has_signatures = false,
    };

    auto res = litewin::evaluate_disk_target(dev);
    assert(res.can_install());
}

// 20 GB Disk (Too small)
void test_too_small_disk() {
    litewin::DiskInfo dev{
        .name = "sdb",
        .path = "/dev/sdb",
        .type = "disk",
        .size_bytes = 20ULL * 1024 * 1024 * 1024,
        .model = "VMware Virtual S",
        .serial = "00000000000000000002",
        .transport = "scsi",
        .mountpoints = std::vector<std::string>{},
        .children = {},
        .has_signatures = false,
    };

    auto res = litewin::evaluate_disk_target(dev);
    assert(!res.can_install());
    assert(res.rejection_reason == litewin::DiskRejectionReason::too_small);
}

// CD-ROM / ISO device
void test_cdrom_device() {
    litewin::DiskInfo dev{
        .name = "sr0",
        .path = "/dev/sr0",
        .type = "rom",
        .size_bytes = 4ULL * 1024 * 1024 * 1024,
        .model = "NECVMWar VMware SATA CD00",
        .serial = {},
        .transport = "sata",
        .mountpoints = std::vector<std::string>{"/media/cdrom"},
        .children = {},
        .has_signatures = false,
    };

    auto res = litewin::evaluate_disk_target(dev);
    assert(!res.can_install());
    assert(res.rejection_reason == litewin::DiskRejectionReason::not_a_disk_type);
}

// Boot USB device mounted at /run/live
void test_mounted_boot_device() {
    litewin::DiskInfo dev{
        .name = "sdc",
        .path = "/dev/sdc",
        .type = "disk",
        .size_bytes = 64ULL * 1024 * 1024 * 1024,
        .model = "SanDisk Ultra",
        .serial = "SD123456",
        .transport = "usb",
        .mountpoints = std::vector<std::string>{},
        .children = {
            litewin::DiskInfo{
                .name = "sdc1",
                .path = "/dev/sdc1",
                .type = "part",
                .size_bytes = 64ULL * 1024 * 1024 * 1024,
                .model = {},
                .serial = {},
                .transport = {},
                .mountpoints = std::vector<std::string>{"/run/live"},
                .children = {},
            },
        },
    };

    auto res = litewin::evaluate_disk_target(dev);
    assert(!res.can_install());
    assert(res.rejection_reason == litewin::DiskRejectionReason::mounted_in_use);
}

// Disk with existing partitions
void test_has_existing_partitions() {
    litewin::DiskInfo dev{
        .name = "sdd",
        .path = "/dev/sdd",
        .type = "disk",
        .size_bytes = 100ULL * 1024 * 1024 * 1024,
        .model = "VMware Virtual S",
        .serial = "00000000000000000003",
        .transport = "scsi",
        .mountpoints = std::vector<std::string>{},
        .children = {
            litewin::DiskInfo{
                .name = "sdd1",
                .path = "/dev/sdd1",
                .type = "part",
                .size_bytes = 50ULL * 1024 * 1024 * 1024,
                .model = {},
                .serial = {},
                .transport = {},
                .mountpoints = std::vector<std::string>{},
                .children = {},
            },
        },
    };

    auto res = litewin::evaluate_disk_target(dev);
    assert(!res.can_install());
    assert(res.rejection_reason == litewin::DiskRejectionReason::has_partitions);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: disk_target_tests <scenario>\n";
        return 1;
    }

    const std::string_view scenario = argv[1];
    if (scenario == "vmware_scsi_eligible") {
        test_vmware_scsi_eligible();
    } else if (scenario == "vmware_nvme_32gb") {
        test_vmware_nvme_32gb();
    } else if (scenario == "too_small") {
        test_too_small_disk();
    } else if (scenario == "cdrom_device") {
        test_cdrom_device();
    } else if (scenario == "mounted_boot_device") {
        test_mounted_boot_device();
    } else if (scenario == "has_existing_partitions") {
        test_has_existing_partitions();
    } else {
        std::cerr << "Unknown scenario: " << scenario << "\n";
        return 1;
    }

    return 0;
}

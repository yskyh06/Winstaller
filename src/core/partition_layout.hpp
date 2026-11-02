#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace litewin {

// Standard GPT Partition Type GUIDs
inline constexpr const char* kGptTypeEfi = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
inline constexpr const char* kGptTypeMsr = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE";
inline constexpr const char* kGptTypeBasicData = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
inline constexpr const char* kGptTypeRecovery = "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC";

enum class PartitionRole {
    recovery,      // WinRE Recovery partition (placed at front)
    efi_system,    // ESP FAT32
    msr,           // Microsoft Reserved
    windows_os,    // Main Windows NTFS (C: drive)
    litewin_temp,  // Temporary ISO & WinPE storage (exFAT, placed at end of disk)
};

struct PartitionSpec {
    int partition_number = 0; // 1-based
    PartitionRole role;
    std::string name;
    std::string type_guid;
    std::string filesystem; // "fat32", "ntfs", "exfat", or "" for MSR
    std::uint64_t size_bytes = 0;
    std::uint64_t start_sector = 0;
    std::uint64_t sector_count = 0;
};

struct DiskLayout {
    std::uint64_t disk_size_bytes = 0;
    std::uint32_t sector_size = 512;
    std::vector<PartitionSpec> partitions;

    [[nodiscard]] const PartitionSpec* find_by_role(PartitionRole role) const noexcept {
        for (const auto& part : partitions) {
            if (part.role == role) {
                return &part;
            }
        }
        return nullptr;
    }
};

struct LayoutOptions {
    std::uint64_t recovery_size_bytes = 800ULL * 1024 * 1024;    // 800 MB
    std::uint64_t efi_size_bytes = 1024ULL * 1024 * 1024;        // 1024 MB (1 GiB, holds WinPE boot.wim and boot.sdi)
    std::uint64_t msr_size_bytes = 16ULL * 1024 * 1024;          // 16 MB
    std::uint64_t temp_partition_size_bytes = 8ULL * 1024 * 1024 * 1024; // 8 GB (for ISO + WinPE)
    std::uint32_t sector_size = 512;
    std::uint64_t alignment_sectors = 2048; // 1 MiB alignment
};

struct LayoutCalculationResult {
    bool success = false;
    std::string error_message;
    DiskLayout layout;
};

[[nodiscard]] inline LayoutCalculationResult calculate_disk_layout(
    std::uint64_t total_disk_bytes,
    const LayoutOptions& options = LayoutOptions{}
) {
    LayoutCalculationResult result;

    auto fail = [&]() {
        result.error_message = "Invalid layout options or insufficient usable disk space.";
        return result;
    };
    if ((options.sector_size != 512 && options.sector_size != 4096) ||
        options.alignment_sectors == 0) {
        return fail();
    }
    const auto total_sectors = total_disk_bytes / options.sector_size;
    // GPT reserves a header and at least 16 KiB of partition entries at each end.
    const std::uint64_t gpt_reserved = 1 + (16384 + options.sector_size - 1) / options.sector_size;
    if (total_sectors <= 2 * gpt_reserved + 1) return fail();
    const auto usable_end = total_sectors - gpt_reserved; // exclusive
    auto align_down = [&](std::uint64_t sectors) {
        return sectors - sectors % options.alignment_sectors;
    };
    auto align_up = [&](std::uint64_t sectors) -> std::uint64_t {
        const auto remainder = sectors % options.alignment_sectors;
        if (!remainder) return sectors;
        const auto extra = options.alignment_sectors - remainder;
        if (sectors > UINT64_MAX - extra) return 0;
        return sectors + extra;
    };
    auto bytes_to_sectors = [&](std::uint64_t bytes) -> std::uint64_t {
        return align_up(bytes / options.sector_size + (bytes % options.sector_size != 0));
    };
    const auto rec_sectors = bytes_to_sectors(options.recovery_size_bytes);
    const auto efi_sectors = bytes_to_sectors(options.efi_size_bytes);
    const auto msr_sectors = bytes_to_sectors(options.msr_size_bytes);
    const auto temp_sectors = bytes_to_sectors(options.temp_partition_size_bytes);
    std::uint64_t current_sector = align_up(gpt_reserved + 1);
    if (!current_sector || current_sector >= usable_end) return fail();
    auto remaining = usable_end - current_sector;
    for (auto count : {rec_sectors, efi_sectors, msr_sectors, temp_sectors}) {
        if (!count || count > remaining) return fail();
        remaining -= count;
    }
    const auto win_sectors = align_down(remaining);
    constexpr std::uint64_t kMinimumWindowsPartBytes = 16ULL * 1024 * 1024 * 1024;
    if (win_sectors < kMinimumWindowsPartBytes / options.sector_size) return fail();

    DiskLayout layout;
    layout.disk_size_bytes = total_disk_bytes;
    layout.sector_size = options.sector_size;

    // 1. Recovery Partition (at the front)
    layout.partitions.push_back(PartitionSpec{
        .partition_number = 1,
        .role = PartitionRole::recovery,
        .name = "Recovery",
        .type_guid = kGptTypeRecovery,
        .filesystem = "ntfs",
        .size_bytes = rec_sectors * options.sector_size,
        .start_sector = current_sector,
        .sector_count = rec_sectors,
    });
    current_sector += rec_sectors;

    // 2. EFI System Partition
    layout.partitions.push_back(PartitionSpec{
        .partition_number = 2,
        .role = PartitionRole::efi_system,
        .name = "EFI system partition",
        .type_guid = kGptTypeEfi,
        .filesystem = "fat32",
        .size_bytes = efi_sectors * options.sector_size,
        .start_sector = current_sector,
        .sector_count = efi_sectors,
    });
    current_sector += efi_sectors;

    // 3. Microsoft Reserved (MSR)
    layout.partitions.push_back(PartitionSpec{
        .partition_number = 3,
        .role = PartitionRole::msr,
        .name = "Microsoft reserved partition",
        .type_guid = kGptTypeMsr,
        .filesystem = "",
        .size_bytes = msr_sectors * options.sector_size,
        .start_sector = current_sector,
        .sector_count = msr_sectors,
    });
    current_sector += msr_sectors;

    // 4. Windows OS (C: drive)
    layout.partitions.push_back(PartitionSpec{
        .partition_number = 4,
        .role = PartitionRole::windows_os,
        .name = "Basic data partition",
        .type_guid = kGptTypeBasicData,
        .filesystem = "ntfs",
        .size_bytes = win_sectors * options.sector_size,
        .start_sector = current_sector,
        .sector_count = win_sectors,
    });
    current_sector += win_sectors;

    // 5. Temporary Partition (Litewin Temp, NTFS for WinPE bootmgfw compatibility, placed at End of Disk)
    layout.partitions.push_back(PartitionSpec{
        .partition_number = 5,
        .role = PartitionRole::litewin_temp,
        .name = "LitewinTemp",
        .type_guid = kGptTypeBasicData,
        .filesystem = "ntfs",
        .size_bytes = temp_sectors * options.sector_size,
        .start_sector = current_sector,
        .sector_count = temp_sectors,
    });

    result.success = true;
    result.layout = std::move(layout);
    return result;
}

[[nodiscard]] inline std::string generate_sfdisk_script(const DiskLayout& layout) {
    std::string script = "label: gpt\nunit: sectors\nsector-size: " +
                         std::to_string(layout.sector_size) + "\n\n";
    for (const auto& part : layout.partitions) {
        script += std::to_string(part.partition_number) + " : " +
                  "start=" + std::to_string(part.start_sector) + ", " +
                  "size=" + std::to_string(part.sector_count) + ", " +
                  "type=" + part.type_guid + ", " +
                  "name=\"" + part.name + "\"\n";
    }
    return script;
}

} // namespace litewin

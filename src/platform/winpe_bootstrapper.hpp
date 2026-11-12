#pragma once

#include <cstdint>
#include <string>

namespace litewin {

struct WinpeBootstrapOptions {
    std::string windows_mount_dir = "/mnt/windows";
    std::string esp_mount_dir = "/mnt/esp";
    std::string temp_mount_dir = "/mnt/temp";
    std::string disk_device = "/dev/nvme0n1";
    int esp_partition_number = 2;
    int temp_partition_number = 5;
    bool dry_run = false;
    bool strict_image_selector = false;
    std::string esd_source_path = "";
    std::string locale = "ko-kr";
};

struct WinpeBootstrapResult {
    bool success = false;
    std::string boot_entry_id;
    std::string error_message;
};

class WinpeBootstrapper {
public:
    // Generates the WinPE automation startnet.cmd script
    [[nodiscard]] static std::string generate_startnet_cmd(
        const std::string& locale = "ko-kr"
    );

    // Generates the diskpart script used inside WinPE to guarantee drive letters
    [[nodiscard]] static std::string generate_winpe_diskpart_script(
        int esp_part_num = 2,
        int windows_part_num = 4
    );

    // Parses the newly created Boot entry number from efibootmgr stdout (e.g. Boot0002)
    [[nodiscard]] static std::string parse_created_boot_entry(const std::string& efibootmgr_stdout);

    // Orchestrates staging bootloader on ESP, winre on Temp, and registering BootNext
    [[nodiscard]] static WinpeBootstrapResult bootstrap(
        const WinpeBootstrapOptions& options
    );
};

} // namespace litewin

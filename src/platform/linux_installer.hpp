#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litewin {

enum class LinuxBootFamily {
    Debian,
    RedHat,
    Arch
};

[[nodiscard]] std::string_view to_string(LinuxBootFamily family) noexcept;

enum class LinuxImageFormat {
    SingleSquashFS,
    LayeredSquashFS,
    Erofs
};

[[nodiscard]] std::string_view to_string(LinuxImageFormat format) noexcept;

enum class LinuxPipelineStep {
    None,
    PreCheck,
    PartitionDisk,
    MountVolumes,
    AcquireImage,
    ExtractRootFS,
    ConfigureChroot,
    InstallBootloader,
    UnmountVolumes,
    Complete
};

[[nodiscard]] std::string_view to_string(LinuxPipelineStep step) noexcept;

enum class ForeignOsType {
    Windows,
    Linux
};

struct ForeignOsEntry {
    ForeignOsType type = ForeignOsType::Windows;
    std::string os_name;
    std::string esp_device_path;
    std::string esp_uuid;
    std::string efi_loader_path;
};

struct LinuxPartitionPlan {
    std::string esp_part;
    std::string root_part;
    std::string sfdisk_script;
};

struct LinuxInstallOptions {
    std::string target_disk_path; // e.g. "/dev/nvme0n1", "/dev/sda"
    std::uint64_t target_disk_size = 64ULL * 1024 * 1024 * 1024;
    LinuxBootFamily boot_family = LinuxBootFamily::Debian;
    LinuxImageFormat image_format = LinuxImageFormat::SingleSquashFS;
    std::vector<std::string> image_layer_paths; // paths to .squashfs or .erofs files
    std::string download_url;
    std::string expected_sha256;
    std::string kernel_source_path; // optional relative path inside ISO (e.g. "arch/boot/x86_64/vmlinuz-linux")
    std::string mount_root = "/mnt/litewin_linux";
    std::string hostname = "litewin-pc";
    std::string username = "user";
    std::string password = "";
    std::string locale = "en_US.UTF-8";
    std::vector<ForeignOsEntry> foreign_os_entries;
    bool dry_run = false;
    std::function<void(LinuxPipelineStep step, int percent, const std::string& message)> progress_callback;
};

struct LinuxInstallResult {
    bool success = false;
    LinuxPipelineStep last_completed_step = LinuxPipelineStep::None;
    LinuxPipelineStep failed_step = LinuxPipelineStep::None;
    std::string error_message;
    std::vector<std::string> step_logs;
    std::vector<std::string> executed_commands;
};

class LinuxInstaller {
public:
    // Wraps an argument in single quotes and escapes any internal single quotes
    [[nodiscard]] static std::string shell_quote(std::string_view s);

    // Validates Unix identifiers (usernames, hostnames): 1-64 chars [a-zA-Z0-9_.-], starts with [a-zA-Z0-9_]
    [[nodiscard]] static bool is_valid_unix_identifier(std::string_view s);

    // Updates credentials on an installed Linux target partition
    [[nodiscard]] static bool update_installed_user_credentials(const LinuxInstallOptions& opts);

    // Builds a GPT partition plan with 512 MiB ESP and remaining space for Linux root
    [[nodiscard]] static std::optional<LinuxPartitionPlan> build_partition_plan(
        std::string_view disk_path,
        std::uint64_t disk_size_bytes
    );

    // Builds commands for extracting squashfs/erofs image layers into the target mount
    [[nodiscard]] static std::vector<std::string> build_extraction_commands(
        LinuxImageFormat format,
        const std::vector<std::string>& layer_paths,
        std::string_view target_mount
    );

    // Builds chroot configuration commands for host setup and distro-specific adapters
    [[nodiscard]] static std::vector<std::string> build_chroot_commands(
        const LinuxInstallOptions& opts
    );

    // Exposed helpers for foreign OS discovery and GRUB configuration
    [[nodiscard]] static std::vector<ForeignOsEntry> discover_foreign_os_targets(
        std::string_view target_disk_path
    );

    [[nodiscard]] static std::string build_grub_default_config(
        bool is_dual_boot
    );

    [[nodiscard]] static std::string build_grub_custom_entries(
        const std::vector<ForeignOsEntry>& foreign_os_entries
    );

    [[nodiscard]] static std::string build_forwarding_grub_cfg(
        std::string_view root_uuid,
        std::string_view kernel_filename = "vmlinuz-generic",
        std::string_view initrd_filename = "initrd.img-generic"
    );

    [[nodiscard]] static std::string build_main_grub_cfg(
        std::string_view root_uuid,
        bool is_dual_boot,
        const std::vector<ForeignOsEntry>& foreign_os_entries = {},
        std::string_view kernel_filename = "vmlinuz-generic",
        std::string_view initrd_filename = "initrd.img-generic"
    );

    // Executes the complete Linux installation pipeline (supports dry-run)
    [[nodiscard]] static LinuxInstallResult execute(
        const LinuxInstallOptions& opts
    );
};

} // namespace litewin

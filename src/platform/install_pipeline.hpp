#pragma once

#include "core/disk_target.hpp"
#include "core/esd_catalog.hpp"
#include "core/partition_layout.hpp"
#include "core/unattend_generator.hpp"
#include "platform/disk_partitioner.hpp"
#include "platform/http_downloader.hpp"
#include "platform/wim_deployer.hpp"
#include "platform/winpe_bootstrapper.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace litewin {

enum class PipelineStep {
    None,
    PreCheck,
    PartitionDisk,
    MountVolumes,
    AcquireImage,
    DeployWindows,
    InjectConfiguration,
    BootstrapWinPE,
    UnmountVolumes,
    Complete
};

[[nodiscard]] std::string_view to_string(PipelineStep step);

struct PipelineOptions {
    // Target disk device path (e.g. /dev/nvme0n1, /dev/sda)
    std::string target_disk_path;
    // Total disk size in bytes (defaults to 128 GiB if not specified)
    std::uint64_t target_disk_size = 128ULL * 1024 * 1024 * 1024;
    // Windows ESD Target (version, language, architecture, download URL)
    EsdTarget esd_target = get_default_esd_target();
    // Index within the ESD/WIM image (typically 1 for Home/Pro depending on ESD)
    int image_index = 1;
    // Preferred edition name (e.g., "Pro", "Home") for automatic Full ESD index resolution
    std::string edition_preference = "Pro";
    // When true, preserves Partition 5 (LitewinTemp) and reuses existing install.esd on P5
    bool reuse_existing_esd = false;
    // Unattended installation customization options
    UnattendOptions unattend_options;
    // Mount root directory where subdirectories p2, p4, p5 are created
    std::string mount_root = "/mnt/litewin";
    // Optional pre-downloaded ESD path to bypass download in testing/offline mode
    std::string custom_esd_path;
    // When true, simulates all hardware modifications (partitioning, formatting, mount, efibootmgr)
    bool dry_run = false;
    // Optional callback for user progress reporting: (step, percent 0-100, message)
    std::function<void(PipelineStep step, int percent, const std::string& message)> progress_callback;
};

struct PipelineResult {
    bool success = false;
    PipelineStep last_completed_step = PipelineStep::None;
    PipelineStep failed_step = PipelineStep::None;
    std::string error_message;
    std::vector<std::string> step_logs;
};

class InstallPipeline {
public:
    // Executes the complete 9-stage installation pipeline from RAM disk
    [[nodiscard]] static PipelineResult execute(const PipelineOptions& options);

private:
    static void log_progress(
        const PipelineOptions& options,
        PipelineResult& result,
        PipelineStep step,
        int percent,
        const std::string& message
    );
};

} // namespace litewin

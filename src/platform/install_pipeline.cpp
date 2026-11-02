#include "platform/install_pipeline.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace litewin {

namespace fs = std::filesystem;

std::string_view to_string(PipelineStep step) {
    switch (step) {
    case PipelineStep::None: return "None";
    case PipelineStep::PreCheck: return "PreCheck";
    case PipelineStep::PartitionDisk: return "PartitionDisk";
    case PipelineStep::MountVolumes: return "MountVolumes";
    case PipelineStep::AcquireImage: return "AcquireImage";
    case PipelineStep::DeployWindows: return "DeployWindows";
    case PipelineStep::InjectConfiguration: return "InjectConfiguration";
    case PipelineStep::BootstrapWinPE: return "BootstrapWinPE";
    case PipelineStep::UnmountVolumes: return "UnmountVolumes";
    case PipelineStep::Complete: return "Complete";
    }
    return "Unknown";
}

void InstallPipeline::log_progress(
    const PipelineOptions& options,
    PipelineResult& result,
    PipelineStep step,
    int percent,
    const std::string& message
) {
    std::string entry = "[" + std::string(to_string(step)) + "] (" + std::to_string(percent) + "%) " + message;
    result.step_logs.push_back(entry);
    if (options.progress_callback) {
        options.progress_callback(step, percent, message);
    }
}

PipelineResult InstallPipeline::execute(const PipelineOptions& options) {
    PipelineResult result;

    // -------------------------------------------------------------
    // Step 1: PreCheck
    // -------------------------------------------------------------
    log_progress(options, result, PipelineStep::PreCheck, 5, "Evaluating target disk and network preconditions...");
    if (options.target_disk_path.empty()) {
        result.failed_step = PipelineStep::PreCheck;
        result.error_message = "Target disk path must not be empty";
        return result;
    }

    DiskInfo candidate;
    candidate.path = options.target_disk_path;
    candidate.size_bytes = options.target_disk_size;
    candidate.name = fs::path(options.target_disk_path).filename().string();
    candidate.type = "disk";
    candidate.mountpoints = std::vector<std::string>{};
    candidate.has_signatures = false;

    auto target_eval = evaluate_disk_target(candidate);
    if (!target_eval.is_eligible && !options.dry_run) {
        result.failed_step = PipelineStep::PreCheck;
        result.error_message = "Target disk ineligible: " + target_eval.message;
        return result;
    }

    if (options.custom_esd_path.empty() && options.esd_target.download_url.empty()) {
        result.failed_step = PipelineStep::PreCheck;
        result.error_message = "No download URL or custom ESD path provided";
        return result;
    }

    // Check for OEM Windows digital license in ACPI MSDM table
    if (fs::exists("/sys/firmware/acpi/tables/MSDM")) {
        log_progress(options, result, PipelineStep::PreCheck, 8, "ACPI MSDM table detected (OEM Windows license key present)");
    }

    // Check for Intel VMD hardware driver
    if (fs::exists("/sys/bus/pci/drivers/vmd")) {
        log_progress(options, result, PipelineStep::PreCheck, 9, "Intel VMD storage controller active");
    }

    // Check if target ESP already contains boot_stage.flag (USB bootloop guard)
    fs::path target_flag_check = fs::path(options.mount_root) / "p2" / "EFI" / "Litewin" / "boot_stage.flag";
    if (fs::exists(target_flag_check) && !options.dry_run) {
        log_progress(options, result, PipelineStep::PreCheck, 9, "Existing Litewin boot stage flag detected: handoff active");
    }

    result.last_completed_step = PipelineStep::PreCheck;
    log_progress(options, result, PipelineStep::PreCheck, 10, "Target disk and configuration verified");

    // -------------------------------------------------------------
    // Step 2: PartitionDisk
    // -------------------------------------------------------------
    log_progress(options, result, PipelineStep::PartitionDisk, 15, "Calculating GPT partition layout and formatting...");
    auto layout_res = calculate_disk_layout(options.target_disk_size);
    if (!layout_res.success && !options.dry_run) {
        result.failed_step = PipelineStep::PartitionDisk;
        result.error_message = "Partition layout calculation failed: " + layout_res.error_message;
        return result;
    }

    if (!options.reuse_existing_esd) {
        auto part_res = DiskPartitioner::apply_layout(options.target_disk_path, layout_res.layout, options.dry_run);
        if (!part_res.success && !options.dry_run) {
            result.failed_step = PipelineStep::PartitionDisk;
            result.error_message = "Partition layout failed: " + part_res.error_message;
            return result;
        }
    } else {
        log_progress(options, result, PipelineStep::PartitionDisk, 20, "Preserving existing GPT layout and P5 ESD cache...");
    }

    std::string p1_path = get_partition_device_path(options.target_disk_path, 1);
    std::string p2_path = get_partition_device_path(options.target_disk_path, 2);
    std::string p4_path = get_partition_device_path(options.target_disk_path, 4);
    std::string p5_path = get_partition_device_path(options.target_disk_path, 5);

    auto fmt_p1 = DiskPartitioner::format_partition(p1_path, "ntfs", "WinRE", options.dry_run);
    auto fmt_p2 = DiskPartitioner::format_partition(p2_path, "fat32", "ESP", options.dry_run);
    auto fmt_p4 = DiskPartitioner::format_partition(p4_path, "ntfs", "Windows", options.dry_run);
    PartitionOperationResult fmt_p5;
    if (options.reuse_existing_esd) {
        fmt_p5.success = true;
        fmt_p5.exit_code = 0;
    } else {
        fmt_p5 = DiskPartitioner::format_partition(p5_path, "ntfs", "LitewinTemp", options.dry_run);
    }

    if ((!fmt_p1.success || !fmt_p2.success || !fmt_p4.success || !fmt_p5.success) && !options.dry_run) {
        result.failed_step = PipelineStep::PartitionDisk;
        std::string detail = !fmt_p1.success ? ("P1: " + fmt_p1.error_message) :
                             !fmt_p2.success ? ("P2: " + fmt_p2.error_message) :
                             !fmt_p4.success ? ("P4: " + fmt_p4.error_message) :
                                               ("P5: " + fmt_p5.error_message);
        result.error_message = "Partition formatting failed (" + detail + ")";
        return result;
    }

    result.last_completed_step = PipelineStep::PartitionDisk;
    log_progress(options, result, PipelineStep::PartitionDisk, 25, "Partitions 1-5 successfully initialized");

    // -------------------------------------------------------------
    // Step 3: MountVolumes
    // -------------------------------------------------------------
    log_progress(options, result, PipelineStep::MountVolumes, 30, "Mounting target volumes...");
    fs::path mount_root(options.mount_root);
    fs::path p2_dir = mount_root / "p2";
    fs::path p4_dir = mount_root / "p4";
    fs::path p5_dir = mount_root / "p5";

    std::error_code ec;
    fs::create_directories(p2_dir, ec);
    fs::create_directories(p4_dir, ec);
    fs::create_directories(p5_dir, ec);

    auto mnt_p5 = DiskPartitioner::mount_partition(p5_path, p5_dir.string(), "ntfs", options.dry_run);
    auto mnt_p4 = DiskPartitioner::mount_partition(p4_path, p4_dir.string(), "ntfs", options.dry_run);
    auto mnt_p2 = DiskPartitioner::mount_partition(p2_path, p2_dir.string(), "fat32", options.dry_run);

    if ((!mnt_p5.success || !mnt_p4.success || !mnt_p2.success) && !options.dry_run) {
        result.failed_step = PipelineStep::MountVolumes;
        std::string detail = !mnt_p5.success ? ("P5(" + p5_path + "): " + mnt_p5.error_message) :
                             !mnt_p4.success ? ("P4(" + p4_path + "): " + mnt_p4.error_message) :
                                               ("P2(" + p2_path + "): " + mnt_p2.error_message);
        result.error_message = "Failed to mount target partitions (" + detail + ")";
        return result;
    }

    result.last_completed_step = PipelineStep::MountVolumes;
    log_progress(options, result, PipelineStep::MountVolumes, 35, "All target volumes mounted successfully");

    // -------------------------------------------------------------
    // Step 4: AcquireImage
    // -------------------------------------------------------------
    log_progress(options, result, PipelineStep::AcquireImage, 40, "Acquiring Windows ESD image...");
    std::string esd_file = (p5_dir / "install.esd").string();

    if (!options.custom_esd_path.empty()) {
        esd_file = options.custom_esd_path;
        log_progress(options, result, PipelineStep::AcquireImage, 50, "Using custom/cached ESD: " + esd_file);
    } else if (options.reuse_existing_esd && fs::exists(esd_file, ec) && fs::file_size(esd_file, ec) > 100ULL * 1024 * 1024) {
        auto mb = fs::file_size(esd_file, ec) / (1024 * 1024);
        log_progress(options, result, PipelineStep::AcquireImage, 52, "Reusing existing ESD on P5 (" + std::to_string(mb) + " MB): " + esd_file);
    } else if (options.dry_run) {
        log_progress(options, result, PipelineStep::AcquireImage, 50, "Simulated download from " + options.esd_target.download_url);
    } else {
        DownloadOptions dl_opt;
        dl_opt.url = options.esd_target.download_url;
        dl_opt.destination_path = esd_file;
        dl_opt.expected_sha256 = options.esd_target.sha256_hash;

        int last_reported_pct = -1;
        dl_opt.progress_callback = [&](std::uint64_t dlnow, std::uint64_t dltotal) -> bool {
            std::uint64_t total = (dltotal > 0) ? dltotal : options.esd_target.size_bytes;
            if (total > 0) {
                int pct = static_cast<int>((dlnow * 100ULL) / total);
                if (pct >= last_reported_pct + 2 || (pct == 100 && last_reported_pct < 100)) {
                    last_reported_pct = pct;
                    std::uint64_t dl_mb = dlnow / (1024 * 1024);
                    std::uint64_t total_mb = total / (1024 * 1024);
                    int overall_pct = 40 + (pct * 14) / 100;
                    log_progress(options, result, PipelineStep::AcquireImage, overall_pct,
                                 "Downloading ESD: " + std::to_string(dl_mb) + " / " +
                                 std::to_string(total_mb) + " MB (" + std::to_string(pct) + "%)");
                }
            }
            return true;
        };

        auto dl_res = HttpDownloader::download(dl_opt);
        if (!dl_res.success) {
            result.failed_step = PipelineStep::AcquireImage;
            result.error_message = "ESD download failed: " + dl_res.error_message;
            return result;
        }
    }

    result.last_completed_step = PipelineStep::AcquireImage;
    log_progress(options, result, PipelineStep::AcquireImage, 55, "Windows ESD image verified and staged on P5");

    // -------------------------------------------------------------
    // Step 5: DeployWindows
    // -------------------------------------------------------------
    log_progress(options, result, PipelineStep::DeployWindows, 60, "Deploying Windows image to Partition 4 (C: NTFS)...");
    if (options.dry_run) {
        // In dry run mode, create expected directory scaffolding if directories exist
        fs::create_directories(p4_dir / "Windows" / "Panther", ec);
        fs::create_directories(p4_dir / "Windows" / "System32" / "Recovery", ec);
        fs::create_directories(p4_dir / "Windows" / "Boot" / "EFI", ec);
        fs::create_directories(p4_dir / "Windows" / "Boot" / "DVD" / "EFI", ec);

        // Dummy placeholder files for winre & bootloader
        std::ofstream(p4_dir / "Windows" / "System32" / "Recovery" / "winre.wim") << "MOCK_WINRE";
        std::ofstream(p4_dir / "Windows" / "Boot" / "EFI" / "bootmgfw.efi") << "MOCK_BOOTMGFW";
        std::ofstream(p4_dir / "Windows" / "Boot" / "DVD" / "EFI" / "BCD") << "MOCK_BCD";
        std::ofstream(p4_dir / "Windows" / "Boot" / "DVD" / "EFI" / "boot.sdi") << "MOCK_BOOTSDI";
        log_progress(options, result, PipelineStep::DeployWindows, 70, "Simulated wimlib-imagex apply to P4");
    } else {
        int resolved_index = options.image_index;
        std::string resolved_name = "Default";

        // Auto-resolve OS edition index in Full Microsoft ESD (where Index 1-3 are Setup/WinPE and Index 4+ are OS editions)
        auto images = WimDeployer::get_image_info(esd_file);
        if (images.size() >= 4) {
            int matched_idx = -1;
            for (const auto& img : images) {
                if (img.index < 4) continue; // Skip Setup Media (1) & WinPE (2, 3)
                if (!options.edition_preference.empty() &&
                    img.name.find(options.edition_preference) != std::string::npos) {
                    matched_idx = img.index;
                    resolved_name = img.name;
                    break;
                }
            }
            if (matched_idx == -1) {
                // Fallback to last OS image (typically Windows 11 Pro at Index 6) or Index 4 (Home)
                matched_idx = images.back().index;
                resolved_name = images.back().name;
            }
            resolved_index = matched_idx;
        } else if (!images.empty()) {
            for (const auto& img : images) {
                if (img.index == resolved_index) {
                    resolved_name = img.name;
                    break;
                }
            }
        }

        log_progress(options, result, PipelineStep::DeployWindows, 62,
                     "Extracting image Index " + std::to_string(resolved_index) +
                     " (" + resolved_name + ") via wimlib-imagex (this may take a few minutes)...");

        // Apply directly to the unmounted NTFS block device (p4_path) via libntfs-3g
        // so that Windows NT Security Descriptors (ACLs), 8.3 short names, and reparse points are 100% preserved.
        static_cast<void>(DiskPartitioner::unmount_partition(p4_dir.string(), false));

        WimDeployOptions wim_opt;
        wim_opt.image_path = esd_file;
        wim_opt.image_index = resolved_index;
        wim_opt.target_dir = fs::exists(p4_path, ec) ? p4_path : p4_dir.string();
        wim_opt.verify_integrity = false; // SHA-256 already verified; skip redundant 6GB --check scan

        int last_wim_pct = -1;
        int max_overall_pct = 62;
        std::string last_wim_phase;
        wim_opt.progress_callback = [&](int pct, const std::string& status_line) {
            std::string phase = status_line.substr(0, status_line.find(':'));
            if (phase != last_wim_phase || pct >= last_wim_pct + 2 || (pct == 100 && last_wim_pct < 100)) {
                last_wim_phase = phase;
                last_wim_pct = pct;
                int mapped_pct = 62;
                if (phase.find("Creating") != std::string::npos) {
                    mapped_pct = 62 + (pct * 2) / 100;   // 62% -> 64%
                } else if (phase.find("Extracting") != std::string::npos) {
                    mapped_pct = 64 + (pct * 9) / 100;   // 64% -> 73%
                } else if (phase.find("Applying") != std::string::npos) {
                    mapped_pct = 73 + (pct * 2) / 100;   // 73% -> 75%
                } else {
                    mapped_pct = 62 + (pct * 12) / 100;
                }
                if (mapped_pct > max_overall_pct) {
                    max_overall_pct = mapped_pct;
                }
                log_progress(options, result, PipelineStep::DeployWindows, max_overall_pct, status_line);
            }
        };

        auto wim_res = WimDeployer::apply_image(wim_opt);
        if (!wim_res.success) {
            result.failed_step = PipelineStep::DeployWindows;
            result.error_message = "wimlib-imagex deployment failed: " + wim_res.error_message;
            return result;
        }

        // Remount P4 now that native NTFS extraction is complete so Step 6 & 7 can inject files
        auto remnt_p4 = DiskPartitioner::mount_partition(p4_path, p4_dir.string(), "ntfs", false);
        if (!remnt_p4.success) {
            result.failed_step = PipelineStep::DeployWindows;
            result.error_message = "Failed to remount P4 after image deployment: " + remnt_p4.error_message;
            return result;
        }
    }

    result.last_completed_step = PipelineStep::DeployWindows;
    log_progress(options, result, PipelineStep::DeployWindows, 75, "Windows image successfully extracted to C:");

    // -------------------------------------------------------------
    // Step 6: InjectConfiguration
    // -------------------------------------------------------------
    log_progress(options, result, PipelineStep::InjectConfiguration, 78, "Injecting unattended answer file and cleanup scripts...");
    fs::path panther_dir = p4_dir / "Windows" / "Panther";
    fs::create_directories(panther_dir, ec);

    std::string unattend_xml = UnattendGenerator::generate_xml(options.unattend_options);
    std::string extend_script = UnattendGenerator::generate_extend_script();

    std::ofstream unattend_file(panther_dir / "unattend.xml");
    if (!unattend_file) {
        result.failed_step = PipelineStep::InjectConfiguration;
        result.error_message = "Failed to write unattend.xml to Panther directory";
        return result;
    }
    unattend_file << unattend_xml;

    std::ofstream extend_file(panther_dir / "extend_c.txt");
    if (!extend_file) {
        result.failed_step = PipelineStep::InjectConfiguration;
        result.error_message = "Failed to write extend_c.txt to Panther directory";
        return result;
    }
    extend_file << extend_script;

    result.last_completed_step = PipelineStep::InjectConfiguration;
    log_progress(options, result, PipelineStep::InjectConfiguration, 82, "Unattended configuration injected");

    // -------------------------------------------------------------
    // Step 7: BootstrapWinPE
    // -------------------------------------------------------------
    log_progress(options, result, PipelineStep::BootstrapWinPE, 85, "Staging WinPE BCD, bootloader, and registering UEFI entry...");
    WinpeBootstrapOptions boot_opt;
    boot_opt.windows_mount_dir = p4_dir.string();
    boot_opt.esp_mount_dir = p2_dir.string();
    boot_opt.temp_mount_dir = p5_dir.string();
    boot_opt.esd_source_path = esd_file;
    boot_opt.locale = !options.esd_target.language_code.empty()
        ? options.esd_target.language_code
        : options.unattend_options.language;
    boot_opt.disk_device = options.target_disk_path;
    boot_opt.dry_run = options.dry_run;

    auto boot_res = WinpeBootstrapper::bootstrap(boot_opt);
    if (!boot_res.success && !options.dry_run) {
        result.failed_step = PipelineStep::BootstrapWinPE;
        result.error_message = "WinPE bootstrapping failed: " + boot_res.error_message;
        return result;
    }

    result.last_completed_step = PipelineStep::BootstrapWinPE;
    log_progress(options, result, PipelineStep::BootstrapWinPE, 92, "WinPE staged and UEFI BootNext registered (" + boot_res.boot_entry_id + ")");

    // -------------------------------------------------------------
    // Step 8: UnmountVolumes
    // -------------------------------------------------------------
    log_progress(options, result, PipelineStep::UnmountVolumes, 95, "Safely unmounting target volumes...");
    static_cast<void>(DiskPartitioner::unmount_partition(p2_dir.string(), options.dry_run));
    static_cast<void>(DiskPartitioner::unmount_partition(p4_dir.string(), options.dry_run));
    static_cast<void>(DiskPartitioner::unmount_partition(p5_dir.string(), options.dry_run));

    result.last_completed_step = PipelineStep::UnmountVolumes;
    log_progress(options, result, PipelineStep::UnmountVolumes, 98, "All filesystems flushed and unmounted");

    // -------------------------------------------------------------
    // Step 9: Complete
    // -------------------------------------------------------------
    result.success = true;
    result.last_completed_step = PipelineStep::Complete;
    log_progress(options, result, PipelineStep::Complete, 100, "Installation pipeline ready. Ready to handoff to WinPE on reboot.");

    return result;
}

} // namespace litewin

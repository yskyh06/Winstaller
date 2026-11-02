#include "core/disk_inventory.hpp"
#include "core/disk_target.hpp"
#include "core/esd_catalog.hpp"
#include "platform/disk_inspector.hpp"
#include "platform/install_pipeline.hpp"
#include "platform/network_probe.hpp"
#include "platform/process_runner.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace litewin;

static void print_banner() {
    std::cout << "===============================================================\n"
              << "       Litewin - Zero-Touch Windows 11 RAM Deployer\n"
              << "===============================================================\n\n";
}

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --dry-run               Simulate all operations without touching disk\n"
              << "  --target-disk <path>    Force target disk path (e.g. /dev/nvme0n1)\n"
              << "  --esd <path>            Use local pre-downloaded ESD image file\n"
              << "  --edition <Home|Pro>    Select Windows edition (default: Pro)\n"
              << "  --non-interactive       Skip confirmation prompts\n"
              << "  -h, --help              Show this help menu\n\n";
}

static std::uint64_t detect_cached_esd_mb(const std::string& disk_path, const std::string& mount_root, bool dry_run) {
    if (dry_run || disk_path.empty()) {
        return 0;
    }
    std::string p5_path = get_partition_device_path(disk_path, 5);
    std::error_code ec;
    if (!fs::exists(p5_path, ec)) {
        return 0;
    }
    fs::path probe_dir = fs::path(mount_root) / "p5";
    auto mnt = DiskPartitioner::mount_partition(p5_path, probe_dir.string(), "ntfs", false);
    if (!mnt.success) {
        return 0;
    }
    std::uint64_t mb = 0;
    fs::path esd_path = probe_dir / "install.esd";
    if (fs::exists(esd_path, ec)) {
        auto sz = fs::file_size(esd_path, ec);
        if (!ec && sz > 100ULL * 1024 * 1024) {
            mb = sz / (1024 * 1024);
        }
    }
    static_cast<void>(DiskPartitioner::unmount_partition(probe_dir.string(), false));
    return mb;
}

static void prompt_cache_and_wipe_mode(PipelineOptions& options, bool non_interactive) {
    std::uint64_t cached_mb = detect_cached_esd_mb(options.target_disk_path, options.mount_root, options.dry_run);
    if (cached_mb == 0) {
        options.reuse_existing_esd = false;
        return;
    }
    if (non_interactive) {
        options.reuse_existing_esd = true;
        return;
    }
    std::string p5_path = get_partition_device_path(options.target_disk_path, 5);
    std::cout << "\n>> [CACHE DETECTED] Existing Windows ESD image found on " << p5_path
              << " (" << cached_mb << " MB).\n"
              << "   [1] Reuse existing ESD file (Skip 6 GB download, reformat only P1/P2/P4)\n"
              << "   [2] Full wipe & fresh download (Delete all partitions P1-P5 and re-download)\n"
              << "   Select option [1-2]: ";
    int cache_choice = 1;
    if (std::cin >> cache_choice && cache_choice == 2) {
        options.reuse_existing_esd = false;
    } else {
        if (!std::cin) {
            std::cin.clear();
            std::cin.ignore(10000, '\n');
        }
        options.reuse_existing_esd = true;
    }
}

static bool confirm_disk_formatting(const PipelineOptions& options, bool non_interactive) {
    if (non_interactive || options.dry_run) {
        return true;
    }
    std::cout << "   [WARNING] Partitioning and formatting " << options.target_disk_path << "\n";
    if (options.reuse_existing_esd) {
        std::cout << "             will format P1(WinRE), P2(ESP), and P4(Windows C:) while preserving P5(ESD Cache).\n";
    } else {
        std::cout << "             will PERMANENTLY DESTROY ALL DATA and partitions (P1-P5) on this disk!\n";
    }
    std::cout << "   Proceed with disk formatting and installation? [y/N]: ";
    std::string ans;
    if (!(std::cin >> ans)) {
        std::cin.clear();
        return false;
    }
    return (ans == "y" || ans == "Y" || ans == "yes" || ans == "YES");
}

int main(int argc, char* argv[]) {
    print_banner();

    PipelineOptions options;
    bool non_interactive = false;

    // 1. Parse CLI arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "--non-interactive") {
            non_interactive = true;
        } else if (arg == "--target-disk" && i + 1 < argc) {
            options.target_disk_path = argv[++i];
        } else if (arg == "--esd" && i + 1 < argc) {
            options.custom_esd_path = argv[++i];
        } else if (arg == "--edition" && i + 1 < argc) {
            std::string ed = argv[++i];
            if (ed == "Home" || ed == "home") {
                options.image_index = 1;
                options.edition_preference = "Home";
            } else {
                options.image_index = 3; // Pro
                options.edition_preference = "Pro";
            }
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (options.dry_run) {
        options.mount_root = (fs::temp_directory_path() / "litewin_mounts").string();
        std::cout << ">> [INFO] Running in DRY-RUN mode. Hardware will not be modified.\n\n";
    }

    // 2. Hardware Pre-scan: Network Probe
    std::cout << ">> [1/4] Checking network connectivity...\n";
    auto probe_res = probe_internet(default_internet_probe_targets(), true);
    if (!probe_res.is_online && options.custom_esd_path.empty()) {
        std::cout << "   [WARNING] Internet connection not detected!\n"
                  << "   Please connect an Ethernet cable or connect your smartphone via USB Tethering.\n";
        if (!options.dry_run) {
            std::cerr << "   Cannot download ESD without active internet. Aborting.\n";
            return 1;
        }
    } else {
        std::cout << "   Internet connection verified.\n";
    }

    // 3. Hardware Pre-scan: Disk Inventory & Auto-Targeting
    std::cout << "\n>> [2/4] Scanning internal storage devices...\n";
    if (options.target_disk_path.empty()) {
        auto inv_res = inspect_disks();
        if (std::holds_alternative<DiskInventory>(inv_res)) {
            const auto& disks = std::get<DiskInventory>(inv_res);
            std::vector<DiskInfo> eligible_disks;
            for (const auto& d : disks) {
                auto eval = evaluate_disk_target(d);
                if (eval.is_eligible ||
                    eval.rejection_reason == DiskRejectionReason::has_partitions ||
                    eval.rejection_reason == DiskRejectionReason::existing_signatures) {
                    eligible_disks.push_back(d);
                }
            }

            if (eligible_disks.empty()) {
                if (fs::exists("/sys/bus/pci/drivers/vmd")) {
                    std::cout << "   [NOTICE] Intel VMD controller is active, but no NVMe drives are visible.\n"
                              << "   Please disable Intel VMD/RST in BIOS setup if your disk is hidden.\n";
                }
                if (options.dry_run) {
                    options.target_disk_path = "/dev/nvme0n1";
                    options.target_disk_size = 256ULL * 1024 * 1024 * 1024;
                    std::cout << "   [DRY-RUN] Fallback to simulated target: " << options.target_disk_path << " (256 GB)\n";
                } else {
                    std::cerr << "   No eligible internal install disk found (>= 32 GB, unmounted). Aborting.\n";
                    return 1;
                }
            } else if (eligible_disks.size() == 1 && eligible_disks[0].children.empty()) {
                options.target_disk_path = eligible_disks[0].path;
                options.target_disk_size = eligible_disks[0].size_bytes;
                std::cout << "   [AUTO-SELECTED] " << options.target_disk_path
                          << " (" << (options.target_disk_size / (1024 * 1024 * 1024)) << " GB"
                          << (eligible_disks[0].model ? ", " + *eligible_disks[0].model : "") << ")\n";
            } else {
                std::cout << "   Eligible disks found:\n";
                for (std::size_t i = 0; i < eligible_disks.size(); ++i) {
                    std::cout << "   [" << (i + 1) << "] " << eligible_disks[i].path
                              << " (" << (eligible_disks[i].size_bytes / (1024 * 1024 * 1024)) << " GB"
                              << (eligible_disks[i].model ? ", " + *eligible_disks[i].model : "")
                              << (!eligible_disks[i].children.empty() ? " - Existing Partitions" : "") << ")\n";
                }
                if (non_interactive) {
                    options.target_disk_path = eligible_disks[0].path;
                    options.target_disk_size = eligible_disks[0].size_bytes;
                } else {
                    std::cout << "   Select disk number [1-" << eligible_disks.size() << "]: ";
                    int choice = 1;
                    if (std::cin >> choice && choice >= 1 && choice <= static_cast<int>(eligible_disks.size())) {
                        options.target_disk_path = eligible_disks[choice - 1].path;
                        options.target_disk_size = eligible_disks[choice - 1].size_bytes;
                    } else {
                        std::cin.clear();
                        std::cin.ignore(10000, '\n');
                        options.target_disk_path = eligible_disks[0].path;
                        options.target_disk_size = eligible_disks[0].size_bytes;
                    }
                }
            }
        } else if (options.dry_run) {
            options.target_disk_path = "/dev/nvme0n1";
            options.target_disk_size = 256ULL * 1024 * 1024 * 1024;
            std::cout << "   [DRY-RUN] Fallback target: " << options.target_disk_path << "\n";
        }
    } else {
        std::cout << "   Manual target disk specified: " << options.target_disk_path << "\n";
    }

    // 4. Target Windows Configuration & Cache/Wipe Confirmation
    prompt_cache_and_wipe_mode(options, non_interactive);

    std::cout << "\n>> [3/4] Deployment Target Configuration:\n"
              << "   - OS Target: Windows 11 26H2 (Korean ko-kr, x64, " << options.edition_preference << ")\n"
              << "   - Destination: " << options.target_disk_path << "\n"
              << "   - ESD Mode: " << (options.reuse_existing_esd ? "Reuse Cached ESD on P5" : "Full Disk Format & Download") << "\n"
              << "   - BitLocker Protection: Auto-Disabled (PreventDeviceEncryption)\n"
              << "   - Privacy: Telemetry Disabled, Advertising ID Disabled\n"
              << "   - First User: " << options.unattend_options.username << " (Auto-Logon)\n\n";

    if (!confirm_disk_formatting(options, non_interactive)) {
        std::cout << "   Installation cancelled by user. Exiting to shell.\n";
        return 0;
    }
    std::cout << "\n";

    // 5. Connect Progress Bar & Execute Pipeline (with interactive Retry/Abort loop)
    options.progress_callback = [](PipelineStep step, int pct, const std::string& msg) {
        std::cout << "   [" << to_string(step) << "] (" << pct << "%) " << msg << "\n";
    };

    while (true) {
        std::cout << ">> [4/4] Executing Zero-Touch Installation Pipeline...\n";
        if (!options.dry_run) {
            fs::path mroot(options.mount_root);
            static_cast<void>(DiskPartitioner::unmount_partition((mroot / "p2").string(), false));
            static_cast<void>(DiskPartitioner::unmount_partition((mroot / "p4").string(), false));
            static_cast<void>(DiskPartitioner::unmount_partition((mroot / "p5").string(), false));
            if (!options.reuse_existing_esd) {
                for (int p = 1; p <= 5; ++p) {
                    std::string ppath = get_partition_device_path(options.target_disk_path, p);
                    static_cast<void>(ProcessRunner::run("wipefs", {"-a", "-f", ppath}));
                }
                static_cast<void>(ProcessRunner::run("sfdisk", {"--delete", options.target_disk_path}));
                static_cast<void>(ProcessRunner::run("wipefs", {"-a", "-f", options.target_disk_path}));
                static_cast<void>(ProcessRunner::run("partprobe", {options.target_disk_path}));
                static_cast<void>(ProcessRunner::run("udevadm", {"settle"}));
            }
        }

        auto result = InstallPipeline::execute(options);

        std::cout << "\n===============================================================\n";
        if (result.success) {
            std::cout << ">> [SUCCESS] Windows 11 26H2 successfully installed onto " << options.target_disk_path << "!\n"
                      << "   WinPE staged on ESP. Next reboot will run bcdboot in 3 seconds.\n"
                      << "===============================================================\n\n";

            if (!options.dry_run) {
                std::cout << ">> Rebooting system into Windows in 3 seconds...\n";
                std::this_thread::sleep_for(std::chrono::seconds(3));
                static_cast<void>(ProcessRunner::run("systemctl", {"reboot"}));
            }
            return 0;
        }

        std::cerr << ">> [FAILED] Installation stopped at " << to_string(result.failed_step) << "!\n"
                  << "   Error: " << result.error_message << "\n"
                  << "===============================================================\n";

        if (non_interactive || options.dry_run) {
            return 1;
        }

        std::cout << "\n   How would you like to proceed?\n"
                  << "   [1] Retry installation\n"
                  << "   [2] Abort and exit to shell\n"
                  << "   Select option [1-2]: ";
        int retry_choice = 2;
        if (!(std::cin >> retry_choice) || retry_choice != 1) {
            std::cin.clear();
            std::cout << "   Aborting installation. Exiting to shell.\n";
            return 0;
        }

        prompt_cache_and_wipe_mode(options, non_interactive);
        if (!confirm_disk_formatting(options, non_interactive)) {
            std::cout << "   Retry cancelled by user. Exiting to shell.\n";
            return 0;
        }
        std::cout << "\n   Retrying installation pipeline...\n\n";
    }
}

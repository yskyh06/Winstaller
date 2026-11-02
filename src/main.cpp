#include "core/disk_inventory.hpp"
#include "core/disk_target.hpp"
#include "core/esd_catalog.hpp"
#include "platform/disk_inspector.hpp"
#include "platform/install_pipeline.hpp"
#include "platform/network_probe.hpp"
#include "platform/process_runner.hpp"
#include "ui/ansi_terminal.hpp"
#include "ui/winstaller_tui.hpp"

#include <unistd.h>

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

    TuiUserData user_data;
    int active_tui_field = 0;

    // Interactive TUI Flow (Screens 1 to 4) when run in terminal
    if (!non_interactive && isatty(STDIN_FILENO)) {
        AnsiTerminal term;
        WinstallerTui tui(term);

        enum class SetupStep {
            Eula = 0,
            Wifi = 1,
            Disk = 2,
            Edition = 3,
            Done = 4
        };

        SetupStep current_step = SetupStep::Eula;
        std::vector<DiskInfo> eligible_disks;
        std::string chosen_disk;

        auto reboot_and_exit = [&]() {
            term.clear();
            std::cout << "\033[2J\033[H>> [Winstaller] Installation aborted. Rebooting system...\n";
            if (!options.dry_run) {
                static_cast<void>(ProcessRunner::run("systemctl", {"reboot"}));
            }
            std::exit(0);
        };

        while (current_step != SetupStep::Done) {
            switch (current_step) {
            case SetupStep::Eula: {
                auto act = tui.prompt_eula();
                if (act == ScreenAction::Exit) {
                    reboot_and_exit();
                }
                current_step = SetupStep::Wifi;
                break;
            }
            case SetupStep::Wifi: {
                auto act = tui.prompt_wifi();
                if (act == ScreenAction::Exit) {
                    reboot_and_exit();
                } else if (act == ScreenAction::Back) {
                    current_step = SetupStep::Eula;
                } else {
                    current_step = SetupStep::Disk;
                }
                break;
            }
            case SetupStep::Disk: {
                if (eligible_disks.empty()) {
                    auto inv_res = inspect_disks();
                    if (std::holds_alternative<DiskInventory>(inv_res)) {
                        const auto& disks = std::get<DiskInventory>(inv_res);
                        for (const auto& d : disks) {
                            auto eval = evaluate_disk_target(d);
                            if (eval.is_eligible ||
                                eval.rejection_reason == DiskRejectionReason::has_partitions ||
                                eval.rejection_reason == DiskRejectionReason::existing_signatures) {
                                eligible_disks.push_back(d);
                            }
                        }
                    }

                    if (eligible_disks.empty()) {
                        if (options.dry_run) {
                            DiskInfo mock_nvme;
                            mock_nvme.path = "/dev/nvme0n1";
                            mock_nvme.name = "nvme0n1";
                            mock_nvme.model = "Samsung 980 PRO (Dry-Run)";
                            mock_nvme.size_bytes = 256ULL * 1024 * 1024 * 1024;
                            mock_nvme.transport = "nvme";
                            eligible_disks.push_back(mock_nvme);
                        } else {
                            term.clear();
                            std::cerr << ">> [ERROR] No eligible internal disks (>= 32 GB) found. Rebooting in 5s...\n";
                            std::this_thread::sleep_for(std::chrono::seconds(5));
                            reboot_and_exit();
                        }
                    }
                }

                auto [act, disk] = tui.prompt_disk_selection(eligible_disks);
                if (act == ScreenAction::Exit) {
                    reboot_and_exit();
                } else if (act == ScreenAction::Back) {
                    current_step = SetupStep::Wifi;
                } else {
                    chosen_disk = disk;
                    options.target_disk_path = chosen_disk;
                    for (const auto& d : eligible_disks) {
                        if (d.path == chosen_disk) {
                            options.target_disk_size = d.size_bytes;
                            break;
                        }
                    }
                    current_step = SetupStep::Edition;
                }
                break;
            }
            case SetupStep::Edition: {
                std::string oem_str;
                if (fs::exists("/sys/firmware/acpi/tables/MSDM")) {
                    oem_str = "Windows 11 Pro";
                }
                std::uint64_t cached_mb = detect_cached_esd_mb(options.target_disk_path, options.mount_root, options.dry_run);
                auto [act, choice] = tui.prompt_edition_selection(oem_str, cached_mb > 0);
                if (act == ScreenAction::Exit) {
                    reboot_and_exit();
                } else if (act == ScreenAction::Back) {
                    current_step = SetupStep::Disk;
                } else {
                    options.image_index = choice.edition_index;
                    options.edition_preference = choice.edition_name;
                    options.unattend_options.language = choice.language;
                    options.reuse_existing_esd = choice.reuse_existing_esd;
                    current_step = SetupStep::Done;
                }
                break;
            }
            default:
                break;
            }
        }

        term.clear();
    } else {
        // Fallback for non-interactive / pipe mode
        if (options.target_disk_path.empty()) {
            options.target_disk_path = "/dev/nvme0n1";
            options.target_disk_size = 64ULL * 1024 * 1024 * 1024;
        }
        prompt_cache_and_wipe_mode(options, non_interactive);
    }

    // 5. Connect Progress Bar & Execute Pipeline
    bool is_interactive_tui = (!non_interactive && isatty(STDIN_FILENO));
    std::unique_ptr<AnsiTerminal> pipeline_term;
    std::unique_ptr<WinstallerTui> pipeline_tui;
    if (is_interactive_tui) {
        pipeline_term = std::make_unique<AnsiTerminal>();
        pipeline_tui = std::make_unique<WinstallerTui>(*pipeline_term);
    }

    options.progress_callback = [&](PipelineStep step, int pct, const std::string& msg) {
        if (pipeline_tui) {
            pipeline_tui->handle_user_data_input(user_data, active_tui_field);
            if (!user_data.username.empty()) options.unattend_options.username = user_data.username;
            if (!user_data.password.empty()) options.unattend_options.password = user_data.password;
            if (!user_data.computer_name.empty()) options.unattend_options.computer_name = user_data.computer_name;

            int step_pct = pct;
            int overall_pct = (pct * 95) / 100;
            pipeline_tui->render_progress(std::string(to_string(step)), step_pct, overall_pct, msg, user_data, active_tui_field);
        } else {
            std::cout << "   [" << to_string(step) << "] (" << pct << "%) " << msg << "\n";
        }
    };

    while (true) {
        if (!pipeline_tui) {
            std::cout << ">> [4/4] Executing Zero-Touch Installation Pipeline...\n";
        }
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

        if (result.success) {
            if (pipeline_tui) {
                bool auto_reboot = pipeline_tui->prompt_reboot_countdown(10);
                if (!auto_reboot) {
                    pipeline_term->clear();
                    std::cout << ">> [Winstaller] Reboot countdown paused. Press Enter to reboot now: ";
                    std::string dummy;
                    std::getline(std::cin, dummy);
                }
            } else {
                std::cout << "\n===============================================================\n"
                          << ">> [SUCCESS] Windows 11 successfully installed onto " << options.target_disk_path << "!\n"
                          << "   WinPE staged on ESP. Next reboot will run bcdboot.\n"
                          << "===============================================================\n\n";
            }

            if (!options.dry_run) {
                static_cast<void>(ProcessRunner::run("systemctl", {"reboot"}));
            }
            return 0;
        }

        if (pipeline_term) {
            pipeline_term->clear();
        }
        std::cerr << ">> [FAILED] Installation stopped at " << to_string(result.failed_step) << "!\n"
                  << "   Error: " << result.error_message << "\n"
                  << "===============================================================\n";

        if (non_interactive || options.dry_run) {
            return 1;
        }

        std::cout << "\n   How would you like to proceed?\n"
                  << "   [1] Retry installation\n"
                  << "   [2] Abort and reboot system\n"
                  << "   Select option [1-2]: ";
        int retry_choice = 2;
        if (!(std::cin >> retry_choice) || retry_choice != 1) {
            std::cin.clear();
            std::cout << "   Aborting installation. Rebooting system...\n";
            if (!options.dry_run) {
                static_cast<void>(ProcessRunner::run("systemctl", {"reboot"}));
            }
            return 0;
        }

        prompt_cache_and_wipe_mode(options, non_interactive);
        if (!confirm_disk_formatting(options, non_interactive)) {
            std::cout << "   Retry cancelled by user. Rebooting system...\n";
            if (!options.dry_run) {
                static_cast<void>(ProcessRunner::run("systemctl", {"reboot"}));
            }
            return 0;
        }
        std::cout << "\n   Retrying installation pipeline...\n\n";
    }
}

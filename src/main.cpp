#include "core/disk_inventory.hpp"
#include "core/disk_target.hpp"
#include "core/esd_catalog.hpp"
#include "platform/disk_inspector.hpp"
#include "platform/install_pipeline.hpp"
#include "platform/network_inspector.hpp"
#include "platform/network_probe.hpp"
#include "platform/process_runner.hpp"
#include "ui/ansi_terminal.hpp"
#include "ui/winstaller_tui.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
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
              << "  --edition <Home|Pro>    Select Windows edition (default: Home)\n"
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
    fs::create_directories(probe_dir, ec);
    auto ro_proc = ProcessRunner::run("mount", {"-o", "ro,noatime", p5_path, probe_dir.string()});
    if (!ro_proc.success()) {
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
                options.image_index = 4;
                options.edition_preference = "Windows 11 Home";
            } else {
                options.image_index = 6; // Pro
                options.edition_preference = "Windows 11 Pro";
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

    // Interactive TUI Flow (Streamlined 2-Screen Setup) when run in terminal
    if (!non_interactive && isatty(STDIN_FILENO)) {
        AnsiTerminal term;
        WinstallerTui tui(term);

        // Pre-check network: if online via Ethernet or Wi-Fi, bypass Wi-Fi setup screen
        bool initial_online = false;
        auto inv_res = inspect_network_interfaces();
        if (const auto* inv = std::get_if<NetworkInventory>(&inv_res)) {
            initial_online = has_active_ethernet(*inv);
        }
        if (!initial_online) {
            auto probe = probe_internet(default_internet_probe_targets(), true);
            initial_online = probe.is_online;
        }

        enum class SetupStep {
            Wifi = 0,
            Disk = 1,
            Edition = 2,
            Done = 3
        };

        SetupStep current_step = initial_online ? SetupStep::Disk : SetupStep::Wifi;
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
            case SetupStep::Wifi: {
                auto act = tui.prompt_wifi();
                if (act == ScreenAction::Exit) {
                    reboot_and_exit();
                } else if (act == ScreenAction::Back) {
                    reboot_and_exit();
                } else {
                    current_step = SetupStep::Disk;
                }
                break;
            }
            case SetupStep::Disk: {
                eligible_disks.clear();
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

                if (eligible_disks.empty() && options.dry_run) {
                    DiskInfo mock_nvme;
                    mock_nvme.path = "/dev/nvme0n1";
                    mock_nvme.name = "nvme0n1";
                    mock_nvme.model = "Samsung 980 PRO (Dry-Run)";
                    mock_nvme.size_bytes = 256ULL * 1024 * 1024 * 1024;
                    mock_nvme.transport = "nvme";
                    eligible_disks.push_back(mock_nvme);
                }

                auto [act, disk] = tui.prompt_disk_selection(eligible_disks);
                if (act == ScreenAction::Exit) {
                    reboot_and_exit();
                } else if (act == ScreenAction::Back) {
                    current_step = SetupStep::Wifi;
                } else if (!disk.empty()) {
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
                    std::ifstream msdm_in("/sys/firmware/acpi/tables/MSDM", std::ios::binary);
                    std::string msdm_raw((std::istreambuf_iterator<char>(msdm_in)), std::istreambuf_iterator<char>());
                    if (msdm_raw.find("Pro") != std::string::npos || msdm_raw.find("PRO") != std::string::npos) {
                        oem_str = "Windows 11 Pro";
                    } else {
                        oem_str = "Windows 11 Home";
                    }
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
                    if (auto matched = find_esd_target(WindowsVersion::Win11_26H2, choice.language, "x64")) {
                        options.esd_target = *matched;
                    }
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
            if (options.dry_run) {
                options.target_disk_path = "/dev/nvme0n1";
                options.target_disk_size = 64ULL * 1024 * 1024 * 1024;
            } else {
                std::cerr << ">> [ERROR] Non-interactive mode requires explicit --target-disk <path> and --non-interactive.\n";
                return 1;
            }
        } else if (!non_interactive && !options.dry_run) {
            std::cerr << ">> [ERROR] Standard input is not a TTY. Pass --non-interactive to confirm automated deployment.\n";
            return 1;
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

    std::mutex tui_mtx;
    std::string cur_step_name = "PreCheck";
    int cur_step_pct = 0;
    int cur_overall_pct = 0;
    std::string cur_detail_msg = "Initializing installation pipeline...";

    auto sync_user_data_to_options = [&]() {
        options.unattend_options.username = user_data.username.empty() ? "User" : user_data.username;
        if (user_data.password_matches()) {
            options.unattend_options.password = user_data.password;
        }
        options.unattend_options.computer_name = user_data.computer_name;
    };

    options.progress_callback = [&](PipelineStep step, int pct, const std::string& msg) {
        if (pipeline_tui) {
            std::lock_guard<std::mutex> lock(tui_mtx);
            pipeline_tui->handle_user_data_input(user_data, active_tui_field);
            sync_user_data_to_options();

            cur_step_name = std::string(to_string(step));
            cur_step_pct = pct;
            cur_overall_pct = (pct * 95) / 100;
            cur_detail_msg = msg;
            pipeline_tui->render_progress(cur_step_name, cur_step_pct, cur_overall_pct, cur_detail_msg, user_data, active_tui_field);
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

        std::atomic<bool> pipeline_active{true};
        std::thread input_thread;
        if (pipeline_tui) {
            input_thread = std::thread([&]() {
                while (pipeline_active.load()) {
                    {
                        std::lock_guard<std::mutex> lock(tui_mtx);
                        if (pipeline_tui->handle_user_data_input(user_data, active_tui_field)) {
                            pipeline_tui->render_progress(cur_step_name, cur_step_pct, cur_overall_pct, cur_detail_msg, user_data, active_tui_field);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                }
            });
        }

        auto result = InstallPipeline::execute(options);
        pipeline_active.store(false);
        if (input_thread.joinable()) {
            input_thread.join();
        }

        if (result.success) {
            if (pipeline_tui) {
                while (true) {
                    while (!user_data.password_matches()) {
                        pipeline_tui->render_progress("Complete", 100, 100, "Password mismatch! Please match passwords and press Enter.", user_data, active_tui_field);
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        pipeline_tui->handle_user_data_input(user_data, active_tui_field);
                    }
                    bool auto_reboot = pipeline_tui->prompt_reboot_countdown(10);
                    if (auto_reboot) {
                        break;
                    }
                    user_data.finalized = false;
                    while (!user_data.finalized || !user_data.password_matches()) {
                        pipeline_tui->render_progress("Complete", 100, 100, "Editing user settings. Press Enter on field 4 to save & reboot.", user_data, active_tui_field);
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        pipeline_tui->handle_user_data_input(user_data, active_tui_field);
                    }
                    break;
                }
                sync_user_data_to_options();
                if (!options.dry_run) {
                    fs::path p4_dir = fs::path(options.mount_root) / "p4";
                    std::string p4_path = get_partition_device_path(options.target_disk_path, 4);
                    auto mnt = DiskPartitioner::mount_partition(p4_path, p4_dir.string(), "ntfs", false);
                    if (mnt.success) {
                        std::error_code ec;
                        fs::path panther_dir = p4_dir / "Windows" / "Panther";
                        fs::create_directories(panther_dir, ec);
                        std::ofstream unattend_file(panther_dir / "unattend.xml");
                        if (unattend_file) {
                            unattend_file << UnattendGenerator::generate_xml(options.unattend_options);
                        }
                        static_cast<void>(DiskPartitioner::unmount_partition(p4_dir.string(), false));
                    }
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
            pipeline_term->update_size();
            int w = 70;
            int h = 12;
            int col = std::max(2, (pipeline_term->cols() - w) / 2);
            int row = std::max(4, (pipeline_term->rows() - h) / 2);
            pipeline_term->draw_box(row, col, w, h, "INSTALLATION FAILED", true);
            pipeline_term->print_at(row + 2, col + 4, AnsiTerminal::color_bold() + AnsiTerminal::color_red() +
                                    "Failed at step: " + std::string(to_string(result.failed_step)) + AnsiTerminal::color_reset());
            std::string err_line = result.error_message.substr(0, w - 8);
            pipeline_term->print_at(row + 4, col + 4, err_line);
            pipeline_term->print_at(row + 6, col + 4, AnsiTerminal::color_slate() + "Press Enter to Retry, or Esc to Reboot..." + AnsiTerminal::color_reset());
            pipeline_term->print_at(row + 9, col + 12, AnsiTerminal::bg_teal() + " [ Retry (Enter) ] " + AnsiTerminal::color_reset());
            pipeline_term->print_at(row + 9, col + 38, " [ Reboot (Esc) ] ");

            while (true) {
                auto ev = pipeline_term->read_key(-1);
                if (ev.key == Key::Enter) {
                    break;
                } else if (ev.key == Key::Escape) {
                    pipeline_term->clear();
                    std::cout << "\033[2J\033[H>> [Winstaller] Rebooting system...\n";
                    if (!options.dry_run) {
                        static_cast<void>(ProcessRunner::run("systemctl", {"reboot"}));
                    }
                    return 0;
                }
            }
        } else {
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

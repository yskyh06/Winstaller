#include "core/disk_inventory.hpp"
#include "core/disk_target.hpp"
#include "core/esd_catalog.hpp"
#include "core/linux_catalog.hpp"
#include "platform/disk_inspector.hpp"
#include "platform/install_pipeline.hpp"
#include "platform/windows_compatibility.hpp"
#include "platform/configuration_writer.hpp"
#include "platform/linux_installer.hpp"
#include "platform/network_inspector.hpp"
#include "platform/network_probe.hpp"
#include "platform/process_runner.hpp"
#include "platform/update_service.hpp"
#include "platform/update_media.hpp"
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
    TuiEditionChoice selected_choice;

    // Interactive TUI Flow (Streamlined 2-Screen Setup) when run in terminal
    if (!non_interactive && isatty(STDIN_FILENO)) {
        const bool boot_progress = fs::exists("/run/winstaller-boot/enabled") &&
                                   fs::exists("/usr/local/bin/winstaller-boot");
        auto report_boot = [&](int percent, const std::string& message) {
            if (boot_progress) static_cast<void>(ProcessRunner::run("/usr/local/bin/winstaller-boot",
                {"--state", std::to_string(percent), message}));
        };
        report_boot(95, "Initializing the installer");
        report_boot(97, "Checking network connectivity");
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

        report_boot(99, "Preparing the installation screen");
        if (boot_progress) static_cast<void>(ProcessRunner::run("/usr/local/bin/winstaller-boot", {"--handoff"}));
        AnsiTerminal term;
        WinstallerTui tui(term);
        UpdateConfig update_config;
        try {update_config=UpdateConfig::load();}catch(const std::exception&){}
        UpdateService updates(std::move(update_config));
        tui.configure_updates(&updates);
        updates.check();
        if (boot_progress) static_cast<void>(ProcessRunner::run("/usr/local/bin/winstaller-boot", {"--ready"}));

        enum class SetupStep {
            Wifi = 0,
            Edition = 1,
            Disk = 2,
            Account = 3,
            Done = 4
        };

        SetupStep current_step = initial_online ? SetupStep::Edition : SetupStep::Wifi;
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
                    current_step = SetupStep::Edition;
                }
                break;
            }
            case SetupStep::Edition: {
                updates.check();
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
                    current_step = SetupStep::Wifi;
                } else {
                    selected_choice = choice;
                    options.image_index = choice.edition_index;
                    options.edition_preference = choice.edition_name;
                    options.unattend_options.language = choice.language;
                    options.reuse_existing_esd = choice.reuse_existing_esd;
                    options.strict_image_selector=choice.remote_target.has_value();
                    if(choice.remote_target && choice.remote_target->windows) {
                        options.esd_target=choice.remote_target->esd;
                        options.edition_preference=choice.remote_target->image_name;
                    } else if (!choice.is_linux) {
                        if (auto matched = find_esd_target(WindowsVersion::Win11_26H2, choice.language, "x64")) {
                            options.esd_target = *matched;
                        }
                    }
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
                        if(!options.dry_run && boot_target_status(d.path)!=BootTargetStatus::Allowed)continue;
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
                    current_step = SetupStep::Edition;
                } else if (!disk.empty()) {
                    chosen_disk = disk;
                    options.target_disk_path = chosen_disk;
                    for (const auto& d : eligible_disks) {
                        if (d.path == chosen_disk) {
                            options.target_disk_size = d.size_bytes;
                            break;
                        }
                    }
                    current_step = selected_choice.is_linux ? SetupStep::Account : SetupStep::Done;
                }
                break;
            }
            case SetupStep::Account: {
                auto act = tui.prompt_user_account(user_data, selected_choice.is_linux);
                if (act == ScreenAction::Exit) {
                    reboot_and_exit();
                } else if (act == ScreenAction::Back) {
                    current_step = SetupStep::Disk;
                } else {
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

    // Freeze Linux input before starting any worker. Windows drafts never mutate pipeline options.
    const TuiUserData frozen_linux_user = user_data;
    const auto compatibility = inspect_windows_compatibility();
    options.unattend_options.bypass_tpm_secureboot = false;
    options.unattend_options.bypass_tpm = compatibility.bypass_tpm();
    options.unattend_options.bypass_secure_boot = compatibility.bypass_secure_boot();
    WindowsSettingsSession windows_settings(options.unattend_options);
    if (pipeline_tui) pipeline_tui->configure_progress(selected_choice.is_linux ? nullptr : &windows_settings,
                "TPM: " + to_string(compatibility.tpm) + " | Secure Boot: " + to_string(compatibility.secure_boot) +
                (compatibility.secure_boot_enabled.has_value() ? (*compatibility.secure_boot_enabled ? " (on)" : " (off, capable)") : ""));
    options.download_status_callback = [&](const DownloadStatus& status) {
        if (!pipeline_tui) return;
        std::lock_guard<std::mutex> lock(tui_mtx);
        pipeline_tui->set_download_status(status);
        pipeline_tui->render_progress(cur_step_name, cur_step_pct, cur_overall_pct, cur_detail_msg, user_data, active_tui_field);
    };

    options.progress_callback = [&](PipelineStep step, int pct, const std::string& msg) {
        if (pipeline_tui) {
            std::lock_guard<std::mutex> lock(tui_mtx);

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
        if (!options.dry_run && !selected_choice.is_linux) {
            fs::path mroot(options.mount_root);
            static_cast<void>(DiskPartitioner::unmount_partition((mroot / "p2").string(), false));
            static_cast<void>(DiskPartitioner::unmount_partition((mroot / "p4").string(), false));
            static_cast<void>(DiskPartitioner::unmount_partition((mroot / "p5").string(), false));
        }

        windows_settings.reset_for_retry();
        options.unattend_options = windows_settings.effective();
        if (pipeline_tui) {
            pipeline_tui->configure_progress(selected_choice.is_linux ? nullptr : &windows_settings,
                "TPM: " + to_string(compatibility.tpm) + " | Secure Boot: " + to_string(compatibility.secure_boot) +
                (compatibility.secure_boot_enabled.has_value() ? (*compatibility.secure_boot_enabled ? " (on)" : " (off, capable)") : ""));
            pipeline_tui->set_download_status(DownloadStatus{});
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

        PipelineResult result;
        std::string failed_step_label;
        LinuxInstallOptions lopts;
        if (selected_choice.is_linux) {
            lopts.target_disk_path = options.target_disk_path;
            lopts.target_disk_size = options.target_disk_size;
            lopts.mount_root = options.mount_root + "_linux";
            lopts.dry_run = options.dry_run;
            lopts.download_status_callback = options.download_status_callback;
            std::string linux_user = frozen_linux_user.username.empty() ? "user" : frozen_linux_user.username;
            std::transform(linux_user.begin(), linux_user.end(), linux_user.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            lopts.username = linux_user;
            lopts.password = frozen_linux_user.password;
            std::string linux_host = frozen_linux_user.computer_name.empty() ? "litewin-pc" : frozen_linux_user.computer_name;
            std::transform(linux_host.begin(), linux_host.end(), linux_host.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            lopts.hostname = linux_host;
            lopts.locale = (selected_choice.language == "ko-KR" || selected_choice.language == "ko-kr") ? "ko_KR.UTF-8" : "en_US.UTF-8";

            auto frozen_linux=selected_choice.remote_target?std::optional<LinuxTarget>(selected_choice.remote_target->linux_target):find_linux_target_by_id(selected_choice.linux_distro_id);
            if (auto ltarget = frozen_linux) {
                lopts.strict_official_source=selected_choice.remote_target.has_value();
                lopts.expected_size=ltarget->size_bytes;
                lopts.download_url = ltarget->download_url;
                lopts.expected_sha256 = ltarget->sha256_hash;
                lopts.image_layer_paths = ltarget->image_layers;
                lopts.kernel_source_path = ltarget->kernel_path;
                lopts.boot_family = (ltarget->family == LinuxDistroFamily::RedHat) ? LinuxBootFamily::RedHat :
                                    (ltarget->family == LinuxDistroFamily::Arch)   ? LinuxBootFamily::Arch :
                                                                                     LinuxBootFamily::Debian;
                lopts.image_format = (ltarget->extractor_mode == LinuxExtractorMode::LayeredSquashFS) ? LinuxImageFormat::LayeredSquashFS :
                                     (ltarget->extractor_mode == LinuxExtractorMode::Erofs)           ? LinuxImageFormat::Erofs :
                                                                                                        LinuxImageFormat::SingleSquashFS;

                lopts.progress_callback = [&](LinuxPipelineStep lstep, int pct, const std::string& msg) {
                    if (pipeline_tui) {
                        std::lock_guard<std::mutex> lock(tui_mtx);
                        cur_step_name = std::string(to_string(lstep));
                        cur_step_pct = pct;
                        cur_overall_pct = (pct * 95) / 100;
                        cur_detail_msg = msg;
                        pipeline_tui->render_progress(cur_step_name, cur_step_pct, cur_overall_pct, cur_detail_msg, user_data, active_tui_field);
                    } else {
                        std::cout << "   [" << to_string(lstep) << "] (" << pct << "%) " << msg << "\n";
                    }
                };
                auto lres = LinuxInstaller::execute(lopts);
                result.success = lres.success;
                result.error_message = lres.error_message;
                result.step_logs = std::move(lres.step_logs);
                failed_step_label = std::string(to_string(lres.failed_step));
            } else {
                result.success = false;
                result.error_message = "Unknown Linux distribution ID: " + selected_choice.linux_distro_id;
                failed_step_label = "PreCheck";
            }
        } else {
            result = InstallPipeline::execute(options);
            failed_step_label = std::string(to_string(result.failed_step));
        }
        pipeline_active.store(false);
        if (input_thread.joinable()) {
            input_thread.join();
        }

        if (result.success) {
            if (pipeline_tui) {
                if (selected_choice.is_linux) {
                    pipeline_tui->prompt_reboot_countdown(10);
                } else {
                    windows_settings.begin_completion(WindowsSettingsSession::Clock::now(), 10);
                    int last_remaining = -1;
                    bool redraw = true;
                    while (windows_settings.poll_completion(WindowsSettingsSession::Clock::now()) == SettingsDecision::Pending) {
                        const auto now = WindowsSettingsSession::Clock::now();
                        const int remaining = windows_settings.remaining_seconds(now);
                        if (redraw || remaining != last_remaining) {
                            pipeline_tui->render_progress("Complete", 100, 100,
                                "Image deployment and boot preparation finished.", user_data, active_tui_field);
                            last_remaining = remaining;
                        }
                        redraw = pipeline_tui->handle_user_data_input(user_data, active_tui_field);
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    }
                    const auto final_options = windows_settings.effective();
                    if (!options.dry_run) {
                        const auto p4_dir = fs::path(options.mount_root) / "p4";
                        const auto p4_path = get_partition_device_path(options.target_disk_path, 4);
                        const auto mounted = DiskPartitioner::mount_partition(p4_path, p4_dir.string(), "ntfs", false);
                        bool configured = mounted.success;
                        if (configured) {
                            configured = write_final_unattend(p4_dir, final_options);
                            const auto unmounted = DiskPartitioner::unmount_partition(p4_dir.string(), false);
                            configured = unmounted.success && configured;
                        }
                        if (!configured) {
                            pipeline_term->clear();
                            std::cerr << "Final Windows configuration could not be committed. Automatic reboot is blocked.\n"
                                      << "The installation was preserved; do not retry disk formatting to fix this error.\n";
                            // Keep the failure visible. Exiting does not call reboot or rerun the pipeline.
                            pipeline_term->print_at(3, 2, "Configuration write failed. Reboot blocked. Press Enter to exit.");
                            while (pipeline_term->read_key(100).key != Key::Enter) {}
                            return 1;
                        }
                    }
                }
            } else {
                if (selected_choice.is_linux) {
                    std::cout << "\n===============================================================\n"
                              << ">> [SUCCESS] " << selected_choice.edition_name << " successfully installed onto " << options.target_disk_path << "!\n"
                              << "   UEFI GRUB bootloader registered. Ready to reboot.\n"
                              << "===============================================================\n\n";
                } else {
                    std::cout << "\n===============================================================\n"
                              << ">> [SUCCESS] Windows 11 successfully installed onto " << options.target_disk_path << "!\n"
                              << "   WinPE staged on ESP. Next reboot will run bcdboot.\n"
                              << "===============================================================\n\n";
                }
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
                                    "Failed at step: " + failed_step_label + AnsiTerminal::color_reset());
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
            std::cerr << ">> [FAILED] Installation stopped at " << failed_step_label << "!\n"
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

        if (!selected_choice.is_linux) {
            prompt_cache_and_wipe_mode(options, non_interactive);
        }
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

#include "platform/winpe_bootstrapper.hpp"
#include "platform/process_runner.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace litewin {

std::string WinpeBootstrapper::generate_startnet_cmd(const std::string& locale) {
    std::string loc_arg = locale.empty() ? "" : (" /l " + locale);
    return "@echo off\r\n"
           "echo ==================================================\r\n"
           "echo [Litewin] Bootstrapping Windows UEFI BCD Store...\r\n"
           "echo ==================================================\r\n"
           "diskpart /s X:\\assign_letters.txt > X:\\diskpart.log 2>&1\r\n"
           "if not exist \"S:\\EFI\\Litewin\\boot_stage.flag\" (\r\n"
           "    mountvol S: /d >nul 2>&1\r\n"
           "    mountvol S: /s >nul 2>&1\r\n"
           ")\r\n"
           "\r\n"
           "set TARGET_DRIVE=\r\n"
           "for %%d in (C W D E F G H I J K L M N O P Q R T U V) do (\r\n"
           "    if exist \"%%d:\\Windows\\System32\\ntoskrnl.exe\" (\r\n"
           "        set TARGET_DRIVE=%%d\r\n"
           "        goto :found_os\r\n"
           "    )\r\n"
           ")\r\n"
           ":found_os\r\n"
           "if defined TARGET_DRIVE (\r\n"
           "    echo [Litewin] Found Windows installation at %TARGET_DRIVE%:\\Windows\r\n"
           "    attrib -r -s -h S:\\EFI\\Microsoft\\Boot\\BCD* >nul 2>&1\r\n"
           "    del /f /q S:\\EFI\\Microsoft\\Boot\\BCD* >nul 2>&1\r\n"
           "    bcdboot %TARGET_DRIVE%:\\Windows /s S: /f UEFI" + loc_arg + " > X:\\bcdboot.log 2>&1\r\n"
           "    if errorlevel 1 (\r\n"
           "        bcdboot %TARGET_DRIVE%:\\Windows /s S: /f UEFI >> X:\\bcdboot.log 2>&1\r\n"
           "    )\r\n"
           "    reg load HKLM\\LITEWIN_BCD S:\\EFI\\Microsoft\\Boot\\BCD >nul 2>&1\r\n"
           "    reg add HKLM\\LITEWIN_BCD\\Description /v System /t REG_DWORD /d 1 /f >nul 2>&1\r\n"
           "    reg add HKLM\\LITEWIN_BCD\\Description /v TreatAsSystem /t REG_DWORD /d 1 /f >nul 2>&1\r\n"
           "    reg unload HKLM\\LITEWIN_BCD >nul 2>&1\r\n"
           "    copy /y S:\\EFI\\Microsoft\\Boot\\bootmgfw.efi S:\\EFI\\BOOT\\BOOTX64.EFI >nul 2>&1\r\n"
           "    del /f /q S:\\sources\\boot.wim >nul 2>&1\r\n"
           "    for %%v in (C W D E F G H I J K L M N O P Q R T U V) do (\r\n"
           "        if exist \"%%v:\\sources\\boot.wim\" del /f /q \"%%v:\\sources\\boot.wim\" >nul 2>&1\r\n"
           "    )\r\n"
           "    echo [Litewin] UEFI Bootloader successfully configured.\r\n"
           ") else (\r\n"
           "    echo [Litewin] ERROR: Windows installation not found!\r\n"
           ")\r\n"
           "echo [Litewin] Rebooting into Windows Setup...\r\n"
           "wpeutil reboot\r\n"
           "ping 127.0.0.1 -n 10 >nul\r\n";
}

std::string WinpeBootstrapper::generate_winpe_diskpart_script(
    int esp_part_num,
    int windows_part_num
) {
    std::ostringstream oss;
    oss << "automount enable\r\n";
    oss << "select disk 0\r\n";
    oss << "select partition " << esp_part_num << "\r\n";
    oss << "assign letter=S noerr\r\n";
    oss << "select partition " << windows_part_num << "\r\n";
    oss << "assign letter=C noerr\r\n";
    oss << "select disk 1\r\n";
    oss << "select partition " << esp_part_num << "\r\n";
    oss << "assign letter=S noerr\r\n";
    oss << "select partition " << windows_part_num << "\r\n";
    oss << "assign letter=W noerr\r\n";
    oss << "exit\r\n";
    return oss.str();
}

std::string WinpeBootstrapper::parse_created_boot_entry(const std::string& efibootmgr_stdout) {
    // Looks for "BootOrder: 0002,0001..." or "Boot0002* Litewin-Setup"
    std::regex re_named(R"(Boot([0-9A-Fa-f]{4})\*?\s+Litewin-Setup)");
    std::smatch m;
    if (std::regex_search(efibootmgr_stdout, m, re_named)) {
        return m[1].str();
    }

    std::regex re_order(R"(BootOrder:\s*([0-9A-Fa-f]{4}))");
    if (std::regex_search(efibootmgr_stdout, m, re_order)) {
        return m[1].str();
    }

    return "";
}

WinpeBootstrapResult WinpeBootstrapper::bootstrap(const WinpeBootstrapOptions& options) {
    WinpeBootstrapResult result;

    namespace fs = std::filesystem;

    if (options.dry_run && !fs::exists(options.windows_mount_dir)) {
        result.success = true;
        result.boot_entry_id = "0002";
        return result;
    }

    // 1. Verify source Windows installation
    fs::path win_dir(options.windows_mount_dir);
    if (!fs::exists(win_dir)) {
        result.error_message = "Windows mount dir does not exist: " + options.windows_mount_dir;
        return result;
    }

    auto find_case_insensitive = [](const fs::path& base, std::initializer_list<const char*> rel_parts) -> fs::path {
        fs::path cur = base;
        for (const char* part : rel_parts) {
            fs::path direct = cur / part;
            if (fs::exists(direct)) {
                cur = direct;
                continue;
            }
            bool matched = false;
            std::string target_lower(part);
            for (char& c : target_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::error_code ec;
            if (fs::exists(cur, ec) && fs::is_directory(cur, ec)) {
                for (const auto& entry : fs::directory_iterator(cur, ec)) {
                    std::string name_lower = entry.path().filename().string();
                    for (char& c : name_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (name_lower == target_lower) {
                        cur = entry.path();
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                return {};
            }
        }
        return cur;
    };

    fs::path winre_path = find_case_insensitive(win_dir, {"Windows", "System32", "Recovery", "winre.wim"});
    if (winre_path.empty()) {
        winre_path = find_case_insensitive(win_dir, {"Recovery", "WindowsRE", "winre.wim"});
    }

    fs::path temp_dir(options.temp_mount_dir);
    if (!fs::exists(temp_dir)) {
        result.error_message = "Temp mount dir does not exist: " + options.temp_mount_dir;
        return result;
    }

    fs::path esd_fallback = options.esd_source_path.empty() ? (temp_dir / "install.esd") : fs::path(options.esd_source_path);
    fs::path temp_sources = temp_dir / "sources";
    fs::path target_boot_wim = temp_sources / "boot.wim";

    try {
        fs::create_directories(temp_sources);
        if (!winre_path.empty() && fs::exists(winre_path)) {
            if (!fs::exists(target_boot_wim)) {
                fs::copy_file(winre_path, target_boot_wim, fs::copy_options::overwrite_existing);
            }
        } else if (!options.dry_run && fs::exists(esd_fallback)) {
            // Fallback: Export Index 2 (Microsoft Windows PE) directly from Full ESD
            auto exp_proc = ProcessRunner::run("wimlib-imagex", {
                "export", esd_fallback.string(), "2", target_boot_wim.string(), "--boot"
            });
            if (exp_proc.success() && fs::exists(target_boot_wim)) {
                winre_path = target_boot_wim;
            }
        }
    } catch (const std::exception& e) {
        result.error_message = "Failed to stage boot.wim on Temp partition: " + std::string(e.what());
        return result;
    }

    if (winre_path.empty() || !fs::exists(winre_path)) {
        result.error_message = "winre.wim not found in Windows directory and ESD WinPE export failed";
        return result;
    }

    // 3. Stage UEFI Bootloader on 2번 Partition (ESP FAT32)
    fs::path esp_dir(options.esp_mount_dir);
    if (!fs::exists(esp_dir)) {
        result.error_message = "ESP mount dir does not exist: " + options.esp_mount_dir;
        return result;
    }

    fs::path esp_efi_boot = esp_dir / "EFI" / "BOOT";
    fs::path esp_ms_boot = esp_dir / "EFI" / "Microsoft" / "Boot";
    fs::path src_bootmgfw = find_case_insensitive(win_dir, {"Windows", "Boot", "EFI", "bootmgfw.efi"});

    try {
        fs::create_directories(esp_efi_boot);
        fs::create_directories(esp_ms_boot);
        if (!src_bootmgfw.empty() && fs::exists(src_bootmgfw)) {
            // Dual placement: both fallback default (BOOTX64.EFI) and native MS path (bootmgfw.efi)
            fs::copy_file(src_bootmgfw, esp_efi_boot / "BOOTX64.EFI", fs::copy_options::overwrite_existing);
            fs::copy_file(src_bootmgfw, esp_ms_boot / "bootmgfw.efi", fs::copy_options::overwrite_existing);
        }

        // Copy seed BCD template from Windows DVD EFI directory
        fs::path src_bcd = find_case_insensitive(win_dir, {"Windows", "Boot", "DVD", "EFI", "BCD"});
        if (!src_bcd.empty() && fs::exists(src_bcd)) {
            fs::copy_file(src_bcd, esp_ms_boot / "BCD", fs::copy_options::overwrite_existing);
        }

        // Copy boot.sdi if available
        fs::path src_sdi = find_case_insensitive(win_dir, {"Windows", "Boot", "DVD", "EFI", "boot.sdi"});
        if (src_sdi.empty()) {
            src_sdi = find_case_insensitive(win_dir, {"Windows", "Boot", "EFI", "boot.sdi"});
        }
        if (!src_sdi.empty() && fs::exists(src_sdi)) {
            fs::create_directories(esp_dir / "Boot");
            fs::copy_file(src_sdi, esp_dir / "Boot" / "boot.sdi", fs::copy_options::overwrite_existing);
            fs::copy_file(src_sdi, esp_ms_boot / "boot.sdi", fs::copy_options::overwrite_existing);
        }

        // Copy multilingual boot fonts (kor_boot.ttf, jpn_boot.ttf, chs_boot.ttf, malgun_boot.ttf, etc.)
        fs::path src_fonts = find_case_insensitive(win_dir, {"Windows", "Boot", "Fonts"});
        if (!src_fonts.empty() && fs::exists(src_fonts)) {
            std::error_code font_ec;
            fs::create_directories(esp_ms_boot / "Fonts", font_ec);
            fs::create_directories(esp_dir / "Boot" / "Fonts", font_ec);
            fs::copy(src_fonts, esp_ms_boot / "Fonts",
                     fs::copy_options::overwrite_existing | fs::copy_options::recursive, font_ec);
            fs::copy(src_fonts, esp_dir / "Boot" / "Fonts",
                     fs::copy_options::overwrite_existing | fs::copy_options::recursive, font_ec);
        }

        // Copy Boot Resources (bootres.dll) for localized boot strings
        fs::path src_res = find_case_insensitive(win_dir, {"Windows", "Boot", "Resources"});
        if (!src_res.empty() && fs::exists(src_res)) {
            std::error_code res_ec;
            fs::create_directories(esp_ms_boot / "Resources", res_ec);
            fs::copy(src_res, esp_ms_boot / "Resources",
                     fs::copy_options::overwrite_existing | fs::copy_options::recursive, res_ec);
        }

        // Stage boot.wim directly onto P2 (ESP 1024MB FAT32) to eliminate cross-partition locate issues
        fs::path esp_sources = esp_dir / "sources";
        fs::create_directories(esp_sources);
        fs::copy_file(winre_path, esp_sources / "boot.wim", fs::copy_options::overwrite_existing);

        // Stage boot_stage.flag to guard against reboot loops if USB is left attached
        fs::path litewin_meta = esp_dir / "EFI" / "Litewin";
        fs::create_directories(litewin_meta);
        std::ofstream flag_file(litewin_meta / "boot_stage.flag");
        flag_file << "STAGE2_WINPE_READY\n";

        // Write diskpart, startnet, and winpeshl.ini scripts (override recenv.exe in winre.wim)
        fs::path dp_script_path = esp_dir / "assign_letters.txt";
        fs::path sn_script_path = esp_dir / "startnet.cmd";
        fs::path winpeshl_path = esp_dir / "winpeshl.ini";
        {
            std::ofstream ofs_dp(dp_script_path);
            ofs_dp << generate_winpe_diskpart_script(options.esp_partition_number, 4);

            std::ofstream ofs_sn(sn_script_path);
            ofs_sn << generate_startnet_cmd(options.locale);

            std::ofstream ofs_wp(winpeshl_path);
            ofs_wp << "[LaunchApps]\r\n"
                   << "%SYSTEMROOT%\\System32\\cmd.exe, /c X:\\Windows\\System32\\startnet.cmd\r\n";
        }

        if (!options.dry_run) {
            std::string wim_cmd =
                "delete --force '\\Windows\\System32\\winpeshl.ini'\n"
                "add '" + winpeshl_path.string() + "' '\\Windows\\System32\\winpeshl.ini'\n"
                "add '" + dp_script_path.string() + "' '\\assign_letters.txt'\n"
                "add '" + sn_script_path.string() + "' '\\Windows\\System32\\startnet.cmd'\n";
            auto upd_proc = ProcessRunner::run("wimlib-imagex", {
                "update", (esp_sources / "boot.wim").string(), "1"
            }, wim_cmd);
            if (upd_proc.success()) {
                fs::copy_file(esp_sources / "boot.wim", target_boot_wim, fs::copy_options::overwrite_existing);
            }
        }
    } catch (const std::exception& e) {
        result.error_message = "Failed to stage bootloader on ESP: " + std::string(e.what());
        return result;
    }

    if (options.dry_run) {
        result.success = true;
        result.boot_entry_id = "0002";
        return result;
    }

    // 4. Register UEFI Boot entry & Set BootNext + BootOrder fallback via efibootmgr
    auto create_proc = ProcessRunner::run("efibootmgr", {
        "-c",
        "-d", options.disk_device,
        "-p", std::to_string(options.esp_partition_number),
        "-L", "Litewin-Setup",
        "-l", "\\EFI\\BOOT\\BOOTX64.EFI"
    });

    if (!create_proc.success()) {
        // If efibootmgr fails (e.g. non-UEFI boot mode or locked NVRAM), rely on \EFI\BOOT\BOOTX64.EFI fallback
        result.boot_entry_id = "FALLBACK_BOOTX64";
        result.success = true;
        return result;
    }

    std::string boot_num = parse_created_boot_entry(create_proc.stdout_output);
    result.boot_entry_id = boot_num;

    if (!boot_num.empty()) {
        // Step A: Set BootNext
        (void)ProcessRunner::run("efibootmgr", {"-n", boot_num});

        // Step B: Also prepend to BootOrder as fallback if firmware ignores BootNext
        std::regex re_order(R"(BootOrder:\s*([0-9A-Fa-f,]+))");
        std::smatch m;
        if (std::regex_search(create_proc.stdout_output, m, re_order)) {
            std::string current_order = m[1].str();
            std::string new_order = boot_num + "," + current_order;
            (void)ProcessRunner::run("efibootmgr", {"-o", new_order});
        }
    }

    result.success = true;
    return result;
}

} // namespace litewin

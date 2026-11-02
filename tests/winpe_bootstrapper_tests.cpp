#include "platform/winpe_bootstrapper.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

void test_script_generation() {
    std::string cmd = litewin::WinpeBootstrapper::generate_startnet_cmd();
    assert(cmd.find("bcdboot %TARGET_DRIVE%:\\Windows /s S: /f UEFI") != std::string::npos);
    assert(cmd.find("wpeutil reboot") != std::string::npos);
    assert(cmd.find("diskpart /s X:\\assign_letters.txt") != std::string::npos);

    std::string dp = litewin::WinpeBootstrapper::generate_winpe_diskpart_script(2, 4);
    assert(dp.find("select partition 2") != std::string::npos);
    assert(dp.find("assign letter=S") != std::string::npos);
    assert(dp.find("select partition 4") != std::string::npos);
    assert(dp.find("assign letter=C") != std::string::npos);

    std::cout << "[PASS] test_script_generation\n";
}

void test_efibootmgr_stdout_parsing() {
    std::string sample1 =
        "BootCurrent: 0001\n"
        "Timeout: 0 seconds\n"
        "BootOrder: 0003,0001,0000\n"
        "Boot0000* Windows Boot Manager\n"
        "Boot0001* Linux\n"
        "Boot0003* Litewin-Setup\n";

    std::string num1 = litewin::WinpeBootstrapper::parse_created_boot_entry(sample1);
    assert(num1 == "0003");

    std::string sample2 =
        "BootOrder: 0005,0002\n"
        "Boot0002* USB Boot\n";
    std::string num2 = litewin::WinpeBootstrapper::parse_created_boot_entry(sample2);
    assert(num2 == "0005");

    std::cout << "[PASS] test_efibootmgr_stdout_parsing\n";
}

void test_dry_run_bootstrap() {
    litewin::WinpeBootstrapOptions opts;
    opts.dry_run = true;
    auto res = litewin::WinpeBootstrapper::bootstrap(opts);
    assert(res.success);
    assert(res.boot_entry_id == "0002");
    std::cout << "[PASS] test_dry_run_bootstrap\n";
}

void test_missing_windows_mount_error() {
    litewin::WinpeBootstrapOptions opts;
    opts.windows_mount_dir = "/non_existent/windows_mount_dir_12345";
    opts.dry_run = false;

    auto res = litewin::WinpeBootstrapper::bootstrap(opts);
    assert(!res.success);
    assert(res.error_message.find("Windows mount dir does not exist") != std::string::npos);
    std::cout << "[PASS] test_missing_windows_mount_error\n";
}

void test_mock_staging_files() {
    namespace fs = std::filesystem;
    const fs::path base = "/tmp/litewin_bootstrapper_mock";
    fs::remove_all(base);

    fs::path mock_win = base / "windows";
    fs::path mock_esp = base / "esp";
    fs::path mock_temp = base / "temp";

    // Setup mock windows recovery file
    fs::create_directories(mock_win / "Windows" / "System32" / "Recovery");
    {
        std::ofstream ofs(mock_win / "Windows" / "System32" / "Recovery" / "winre.wim");
        ofs << "mock winre content";
    }

    fs::create_directories(mock_win / "Windows" / "Boot" / "EFI");
    {
        std::ofstream ofs(mock_win / "Windows" / "Boot" / "EFI" / "bootmgfw.efi");
        ofs << "mock bootmgfw";
    }

    fs::create_directories(mock_esp);
    fs::create_directories(mock_temp);

    litewin::WinpeBootstrapOptions opts{
        .windows_mount_dir = mock_win.string(),
        .esp_mount_dir = mock_esp.string(),
        .temp_mount_dir = mock_temp.string(),
        .dry_run = false,
    };

    // Staging test: will attempt efibootmgr and fail if no root/UEFI, but we verify file staging happened first
    auto res = litewin::WinpeBootstrapper::bootstrap(opts);

    // Verify WinRE staged to Temp partition
    assert(fs::exists(mock_temp / "sources" / "boot.wim"));

    // Verify bootmgfw copied to ESP (both fallback and MS native path)
    assert(fs::exists(mock_esp / "EFI" / "BOOT" / "BOOTX64.EFI"));
    assert(fs::exists(mock_esp / "EFI" / "Microsoft" / "Boot" / "bootmgfw.efi"));

    // Verify scripts created
    assert(fs::exists(mock_esp / "assign_letters.txt"));
    assert(fs::exists(mock_esp / "startnet.cmd"));

    fs::remove_all(base);
    std::cout << "[PASS] test_mock_staging_files\n";
}

int main() {
    test_script_generation();
    test_efibootmgr_stdout_parsing();
    test_dry_run_bootstrap();
    test_missing_windows_mount_error();
    test_mock_staging_files();
    std::cout << "All winpe_bootstrapper tests passed!\n";
    return 0;
}

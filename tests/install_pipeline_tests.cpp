#include "platform/install_pipeline.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace litewin;

static void test_step_string_conversion() {
    assert(to_string(PipelineStep::None) == "None");
    assert(to_string(PipelineStep::PreCheck) == "PreCheck");
    assert(to_string(PipelineStep::PartitionDisk) == "PartitionDisk");
    assert(to_string(PipelineStep::MountVolumes) == "MountVolumes");
    assert(to_string(PipelineStep::AcquireImage) == "AcquireImage");
    assert(to_string(PipelineStep::DeployWindows) == "DeployWindows");
    assert(to_string(PipelineStep::InjectConfiguration) == "InjectConfiguration");
    assert(to_string(PipelineStep::BootstrapWinPE) == "BootstrapWinPE");
    assert(to_string(PipelineStep::UnmountVolumes) == "UnmountVolumes");
    assert(to_string(PipelineStep::Complete) == "Complete");
}

static void test_precheck_empty_disk_path() {
    PipelineOptions options;
    options.target_disk_path = "";
    auto result = InstallPipeline::execute(options);

    assert(!result.success);
    assert(result.failed_step == PipelineStep::PreCheck);
    assert(result.error_message == "Target disk path must not be empty");
}

static void test_dry_run_full_pipeline_success() {
    fs::path test_mount = fs::temp_directory_path() / "litewin_pipe_test";
    std::error_code ec;
    fs::remove_all(test_mount, ec);

    PipelineOptions options;
    options.target_disk_path = "/dev/nvme0n1";
    options.target_disk_size = 256ULL * 1024 * 1024 * 1024; // 256 GiB
    options.mount_root = test_mount.string();
    options.dry_run = true;

    std::vector<std::pair<PipelineStep, int>> progress_records;
    options.progress_callback = [&](PipelineStep step, int pct, const std::string& msg) {
        (void)msg;
        progress_records.emplace_back(step, pct);
    };

    auto result = InstallPipeline::execute(options);

    assert(result.success);
    assert(result.last_completed_step == PipelineStep::Complete);
    assert(result.failed_step == PipelineStep::None);
    assert(!result.step_logs.empty());

    // Verify progress callbacks fired across pipeline lifecycle
    assert(!progress_records.empty());
    assert(progress_records.front().second == 5);
    assert(progress_records.back().second == 100);

    // Verify injected files exist in p4
    fs::path p4_panther = test_mount / "p4" / "Windows" / "Panther";
    assert(fs::exists(p4_panther / "unattend.xml"));
    assert(fs::exists(p4_panther / "extend_c.txt"));

    // Verify unattend content
    std::ifstream xml_in(p4_panther / "unattend.xml");
    std::string xml_content((std::istreambuf_iterator<char>(xml_in)), std::istreambuf_iterator<char>());
    assert(xml_content.find("<unattend xmlns=") != std::string::npos);
    assert(xml_content.find("PreventDeviceEncryption") != std::string::npos);

    // Verify extend_c content
    std::ifstream extend_in(p4_panther / "extend_c.txt");
    std::string ext_content((std::istreambuf_iterator<char>(extend_in)), std::istreambuf_iterator<char>());
    assert(ext_content.find("select volume C") != std::string::npos);
    assert(ext_content.find("extend") != std::string::npos);

    // Verify staged bootloader files on ESP (P2)
    fs::path p2_efi = test_mount / "p2" / "EFI";
    assert(fs::exists(p2_efi / "Microsoft" / "Boot" / "bootmgfw.efi"));
    assert(fs::exists(p2_efi / "BOOT" / "BOOTX64.EFI"));
    assert(fs::exists(p2_efi / "Microsoft" / "Boot" / "BCD"));
    assert(fs::exists(test_mount / "p2" / "Boot" / "boot.sdi"));

    // Verify staged WinPE on P5 and directly on P2 (ESP 1024MB 통합안)
    assert(fs::exists(test_mount / "p5" / "sources" / "boot.wim"));
    assert(fs::exists(test_mount / "p2" / "sources" / "boot.wim"));
    assert(fs::exists(p2_efi / "Litewin" / "boot_stage.flag"));

    // Verify unattend XML cleans up boot_stage.flag and boot.wim from ESP
    assert(xml_content.find("boot_stage.flag") != std::string::npos);

    fs::remove_all(test_mount, ec);
}

static void test_custom_esd_path_override() {
    fs::path test_mount = fs::temp_directory_path() / "litewin_custom_esd_test";
    std::error_code ec;
    fs::remove_all(test_mount, ec);
    fs::create_directories(test_mount, ec);

    fs::path dummy_esd = test_mount / "local_image.esd";
    std::ofstream(dummy_esd) << "ESD_DATA";

    PipelineOptions options;
    options.target_disk_path = "/dev/sda";
    options.mount_root = test_mount.string();
    options.custom_esd_path = dummy_esd.string();
    options.dry_run = true;

    auto result = InstallPipeline::execute(options);
    assert(result.success);
    assert(result.last_completed_step == PipelineStep::Complete);

    bool found_custom_log = false;
    for (const auto& log : result.step_logs) {
        if (log.find("Using custom/cached ESD") != std::string::npos) {
            found_custom_log = true;
            break;
        }
    }
    assert(found_custom_log);

    fs::remove_all(test_mount, ec);
}

int main() {
    test_step_string_conversion();
    test_precheck_empty_disk_path();
    test_dry_run_full_pipeline_success();
    test_custom_esd_path_override();

    std::cout << "All install_pipeline tests passed successfully!\n";
    return 0;
}

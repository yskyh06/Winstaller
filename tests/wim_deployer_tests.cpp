#include "platform/wim_deployer.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

void test_parse_info_output() {
    std::string sample_info =
        "WIM Information:\n"
        "----------------\n"
        "Index: 1\n"
        "Name: Windows 11 Home\n"
        "Description: Windows 11 Home Edition\n"
        "Total Bytes: 18456123456\n"
        "\n"
        "Index: 2\n"
        "Name: Windows 11 Pro\n"
        "Description: Windows 11 Professional Edition\n"
        "Total Bytes: 19123456789\n";

    auto images = litewin::WimDeployer::parse_info_output(sample_info);
    assert(images.size() == 2);
    assert(images[0].index == 1);
    assert(images[0].name == "Windows 11 Home");
    assert(images[0].total_bytes == 18456123456ULL);

    assert(images[1].index == 2);
    assert(images[1].name == "Windows 11 Pro");
    assert(images[1].total_bytes == 19123456789ULL);
    std::cout << "[PASS] test_parse_info_output\n";
}

void test_missing_files_error_handling() {
    litewin::WimDeployOptions opts{
        .image_path = "/non_existent/path/image.esd",
        .target_dir = "/tmp",
    };

    auto res = litewin::WimDeployer::apply_image(opts);
    assert(!res.success);
    assert(res.error_message.find("Image file does not exist") != std::string::npos);

    // Now existing file, but non-existent target dir
    const std::string tmp_img = "/tmp/litewin_dummy.wim";
    {
        std::ofstream ofs(tmp_img);
        ofs << "dummy";
    }

    opts.image_path = tmp_img;
    opts.target_dir = "/non_existent/target_dir_12345";
    auto res2 = litewin::WimDeployer::apply_image(opts);
    assert(!res2.success);
    assert(res2.error_message.find("Target directory does not exist") != std::string::npos);

    std::filesystem::remove(tmp_img);
    std::cout << "[PASS] test_missing_files_error_handling\n";
}

void test_mock_wimlib_execution() {
    const std::string tmp_img = "/tmp/litewin_mock_test.esd";
    const std::string tmp_dir = "/tmp/litewin_mock_mount";
    const std::string mock_tool = "/tmp/mock_wimlib.sh";

    {
        std::ofstream ofs(tmp_img);
        ofs << "mock esd content";
    }
    std::filesystem::create_directories(tmp_dir);

    {
        std::ofstream ofs(mock_tool);
        ofs << "#!/bin/bash\n";
        ofs << "echo \"Mock wimlib called with: $@\"\n";
        ofs << "exit 0\n";
    }
    std::filesystem::permissions(mock_tool, std::filesystem::perms::owner_all);

    litewin::WimDeployOptions opts{
        .image_path = tmp_img,
        .image_index = 2,
        .target_dir = tmp_dir,
        .verify_integrity = true,
        .wimlib_tool = mock_tool,
    };

    auto res = litewin::WimDeployer::apply_image(opts);
    assert(res.success);
    assert(res.exit_code == 0);
    assert(res.stdout_output.find("apply /tmp/litewin_mock_test.esd 2 /tmp/litewin_mock_mount --check") != std::string::npos);

    std::filesystem::remove(tmp_img);
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::remove(mock_tool);
    std::cout << "[PASS] test_mock_wimlib_execution\n";
}

int main() {
    test_parse_info_output();
    test_missing_files_error_handling();
    test_mock_wimlib_execution();
    std::cout << "All wim_deployer tests passed!\n";
    return 0;
}

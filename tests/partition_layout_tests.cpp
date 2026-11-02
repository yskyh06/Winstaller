#include "core/partition_layout.hpp"
#include "platform/process_runner.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

void test_calculate_32gb_layout() {
    constexpr std::uint64_t disk_32gb = 32ULL * 1024 * 1024 * 1024;
    auto res = litewin::calculate_disk_layout(disk_32gb);
    assert(res.success);
    assert(res.layout.partitions.size() == 5);

    // Verify order and roles
    assert(res.layout.partitions[0].role == litewin::PartitionRole::recovery);
    assert(res.layout.partitions[0].filesystem == "ntfs");

    assert(res.layout.partitions[1].role == litewin::PartitionRole::efi_system);
    assert(res.layout.partitions[1].filesystem == "fat32");

    assert(res.layout.partitions[2].role == litewin::PartitionRole::msr);

    assert(res.layout.partitions[3].role == litewin::PartitionRole::windows_os);
    assert(res.layout.partitions[3].filesystem == "ntfs");

    // Temp partition must be at the very end (index 4)
    assert(res.layout.partitions[4].role == litewin::PartitionRole::litewin_temp);
    assert(res.layout.partitions[4].filesystem == "ntfs");
    assert(res.layout.partitions[4].name == "LitewinTemp");

    // Verify non-overlapping sequential sectors
    for (std::size_t i = 1; i < res.layout.partitions.size(); ++i) {
        const auto& prev = res.layout.partitions[i - 1];
        const auto& curr = res.layout.partitions[i];
        assert(prev.start_sector + prev.sector_count <= curr.start_sector);
    }
}

void test_too_small_disk() {
    constexpr std::uint64_t disk_10gb = 10ULL * 1024 * 1024 * 1024;
    auto res = litewin::calculate_disk_layout(disk_10gb);
    assert(!res.success);
    assert(!res.error_message.empty());
}

void test_generate_sfdisk_script() {
    constexpr std::uint64_t disk_60gb = 60ULL * 1024 * 1024 * 1024;
    auto res = litewin::calculate_disk_layout(disk_60gb);
    assert(res.success);

    std::string script = litewin::generate_sfdisk_script(res.layout);
    assert(script.find("label: gpt") != std::string::npos);
    assert(script.find("name=\"Recovery\"") != std::string::npos);
    assert(script.find("name=\"EFI system partition\"") != std::string::npos);
    assert(script.find("name=\"LitewinTemp\"") != std::string::npos);
}

void test_process_runner_echo() {
    auto res = litewin::ProcessRunner::run("echo", {"hello", "litewin"});
    assert(res.success());
    assert(res.stdout_output.find("hello litewin") != std::string::npos);
}

void test_process_runner_stdin() {
    std::string input = "sample piped input text";
    auto res = litewin::ProcessRunner::run("cat", {}, input);
    assert(res.success());
    assert(res.stdout_output == input);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: partition_layout_tests <scenario>\n";
        return 1;
    }

    const std::string_view scenario = argv[1];
    if (scenario == "calculate_32gb") {
        test_calculate_32gb_layout();
    } else if (scenario == "too_small_disk") {
        test_too_small_disk();
    } else if (scenario == "generate_sfdisk_script") {
        test_generate_sfdisk_script();
    } else if (scenario == "process_runner_echo") {
        test_process_runner_echo();
    } else if (scenario == "process_runner_stdin") {
        test_process_runner_stdin();
    } else {
        std::cerr << "Unknown scenario: " << scenario << "\n";
        return 1;
    }

    return 0;
}

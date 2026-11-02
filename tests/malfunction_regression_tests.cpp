#include "core/disk_target.hpp"
#include "core/partition_layout.hpp"
#include "platform/detail/disk_inspection.hpp"
#include "platform/network_probe.hpp"
#include "platform/process_runner.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
litewin::DiskInfo clean_disk() {
    litewin::DiskInfo disk{};
    disk.name = "sda"; disk.path = "/dev/sda"; disk.type = "disk";
    disk.size_bytes = 32ULL * 1024 * 1024 * 1024;
    disk.mountpoints = std::vector<std::string>{};
    disk.has_signatures = false;
    return disk;
}
std::string signature_output;
bool signature_command_success = true;
int signature_calls = 0;
litewin::ProcessResult fake_run(const std::string& executable,
                               const std::vector<std::string>& arguments,
                               const std::string& input) {
    require(input.empty(), "inspection must not write stdin");
    litewin::ProcessResult result;
    result.executed = true;
    if (executable == "lsblk") {
        result.stdout_output = R"({"blockdevices":[{"name":"sda","path":"/dev/sda","type":"disk","size":34359738368,"mountpoints":[null]}]})";
    } else {
        require(executable == "wipefs", "unexpected inspection tool");
        require(arguments == std::vector<std::string>{"--json", "--output", "TYPE", "--no-act", "--", "/dev/sda"},
                "signature inspection must be read-only and use the device path");
        ++signature_calls;
        result.stdout_output = signature_output;
        if (!signature_command_success) { result.executed = false; result.exit_code = 127; }
    }
    return result;
}
void signatures() {
    for (const auto* type : {"ext4", "linux_raid_member", "gpt", "LVM2_member"}) {
        signature_output = "{\"signatures\":[{\"type\":\"" + std::string(type) + "\"}]}";
        const auto inventory = litewin::detail::inspect_disks_with(fake_run);
        const auto& disk = std::get<litewin::DiskInventory>(inventory).at(0);
        require(disk.has_signatures == true, "existing signature must be preserved");
        const auto evaluation = litewin::evaluate_disk_target(disk);
        require(!evaluation.can_install() && evaluation.rejection_reason == litewin::DiskRejectionReason::existing_signatures,
                "whole-disk signatures must prevent installation");
    }
    signature_output = R"({"signatures":[]})";
    const auto inventory = litewin::detail::inspect_disks_with(fake_run);
    require(litewin::evaluate_disk_target(std::get<litewin::DiskInventory>(inventory).at(0)).can_install(),
            "verified clean disk should remain eligible");
    require(signature_calls == 5, "each disk needs signature inspection");
}
void unknown_signatures() {
    for (const auto* output : {"invalid JSON", "{}", R"({"signatures":null})", R"({"signatures":{}})"}) {
        signature_output = output;
        const auto inventory = litewin::detail::inspect_disks_with(fake_run);
        const auto& disk = std::get<litewin::DiskInventory>(inventory).at(0);
        require(!disk.has_signatures.has_value(), "invalid output must remain unknown");
        require(!litewin::evaluate_disk_target(disk).can_install(), "unknown signatures must prevent installation");
    }
    signature_command_success = false;
    signature_output = R"({"signatures":[]})";
    const auto inventory = litewin::detail::inspect_disks_with(fake_run);
    require(!litewin::evaluate_disk_target(std::get<litewin::DiskInventory>(inventory).at(0)).can_install(),
            "failed or missing wipefs must not prove a disk clean");
}
void unknown_mounts() {
    auto disk = clean_disk();
    disk.mountpoints.reset();
    const auto evaluation = litewin::evaluate_disk_target(disk);
    require(!evaluation.can_install() && evaluation.rejection_reason == litewin::DiskRejectionReason::incomplete_metadata,
            "unknown mount state must prevent installation");
    disk.mountpoints = std::vector<std::string>{};
    disk.has_signatures.reset();
    require(!litewin::evaluate_disk_target(disk).can_install(), "uninspected disk must be rejected");
    const auto parsed = litewin::parse_disk_inventory(R"({"blockdevices":[{"name":"sda","path":"/dev/sda","type":"disk","size":34359738368,"has_signatures":false}]})");
    require(!std::get<litewin::DiskInventory>(parsed).at(0).has_signatures.has_value(),
            "inventory JSON alone must not certify signature absence");
}
void layout_bounds() {
    for (auto sector_size : {512U, 4096U}) {
        for (auto bytes : {32ULL*1024*1024*1024, 60ULL*1024*1024*1024 + 12345}) {
            litewin::LayoutOptions options;
            options.sector_size = sector_size;
            options.recovery_size_bytes += 123;
            const auto result = litewin::calculate_disk_layout(bytes, options);
            require(result.success, "normal layout must fit");
            const auto reserved = 1 + (16384 + sector_size - 1) / sector_size;
            auto previous_end = static_cast<std::uint64_t>(reserved + 1);
            for (const auto& partition : result.layout.partitions) {
                require(partition.start_sector >= previous_end, "partitions overlap GPT or each other");
                require(partition.start_sector % options.alignment_sectors == 0, "unaligned partition");
                require(partition.sector_count > 0, "empty partition");
                previous_end = partition.start_sector + partition.sector_count;
                require(previous_end <= bytes / sector_size - reserved, "partition extends past usable end");
            }
            require(litewin::generate_sfdisk_script(result.layout).find("sector-size: " + std::to_string(sector_size)) != std::string::npos,
                    "script must preserve sector units");
        }
    }
}
void invalid_layout() {
    constexpr auto bytes = 32ULL*1024*1024*1024;
    litewin::LayoutOptions options;
    options.sector_size = 0;
    require(!litewin::calculate_disk_layout(bytes, options).success, "zero sector size");
    options.sector_size = 512; options.alignment_sectors = 0;
    require(!litewin::calculate_disk_layout(bytes, options).success, "zero alignment");
    options.alignment_sectors = std::numeric_limits<std::uint64_t>::max();
    require(!litewin::calculate_disk_layout(bytes, options).success, "alignment overflow");
    options = {}; options.temp_partition_size_bytes = std::numeric_limits<std::uint64_t>::max();
    require(!litewin::calculate_disk_layout(bytes, options).success, "oversized partition");
    options = {};
    const auto exactly_payload = options.recovery_size_bytes + options.efi_size_bytes + options.msr_size_bytes +
                                 options.temp_partition_size_bytes + 16ULL*1024*1024*1024;
    require(!litewin::calculate_disk_layout(exactly_payload, options).success, "GPT and alignment require additional space");
}
void closed_stdin() {
    struct sigaction before{}, after{};
    ::sigaction(SIGPIPE, nullptr, &before);
    const auto result = litewin::ProcessRunner::run("/bin/true", {}, std::string(16*1024*1024, 'x'));
    require(!result.success() && result.error_message.find("Broken pipe") != std::string::npos,
            "closed stdin must return EPIPE, not terminate the caller or fail setup");
    ::sigaction(SIGPIPE, nullptr, &after);
    require(before.sa_handler == after.sa_handler, "must not change global SIGPIPE handler");
    const auto missing = litewin::ProcessRunner::run("/nonexistent/litewin-review-tool", {}, std::string(16*1024*1024, 'x'));
    require(!missing.success() && missing.exit_code == 127, "failed exec must survive stdin writes");
}
void large_stdio(const std::string& self) {
    const std::string input(16*1024*1024, 'x');
    const auto result = litewin::ProcessRunner::run(self, {"child_copy"}, input);
    if (!result.success()) throw std::runtime_error("large simultaneous I/O failed: " + result.error_message +
        " exit=" + std::to_string(result.exit_code) + " stderr=" + result.stderr_output.substr(0, 200));
    require(result.stdout_output == input && result.stderr_output == input, "large simultaneous I/O lost data");
}
int child_copy() {
    char buffer[4096];
    ssize_t count;
    while ((count = ::read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        for (int fd : {STDOUT_FILENO, STDERR_FILENO}) {
            ssize_t written = 0;
            while (written < count) {
                const auto n = ::write(fd, buffer + written, count - written);
                if (n <= 0) return 1;
                written += n;
            }
        }
    }
    return count < 0 ? 1 : 0;
}
void numeric_only() {
    const auto start = std::chrono::steady_clock::now();
    for (const auto* host : {"localhost", "unresolvable.invalid", "999.999.999.999", ""}) {
        const auto result = litewin::probe_single_endpoint(host, 53, std::chrono::milliseconds(20));
        require(!result.success && result.error_message.find("numeric") != std::string::npos,
                "hostnames and invalid literals must be rejected without DNS");
    }
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "numeric validation must not wait on DNS");
    for (const auto* host : {"127.0.0.1", "::1"}) {
        const auto result = litewin::probe_single_endpoint(host, 0, std::chrono::milliseconds(20));
        require(result.error_message.find("numeric") == std::string::npos,
                "numeric IPv4 and IPv6 addresses must still be accepted");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "scenario required");
        const std::string_view scenario = argv[1];
        if (scenario == "child_copy") return child_copy();
        if (scenario == "signatures") signatures();
        else if (scenario == "unknown_signatures") unknown_signatures();
        else if (scenario == "unknown_mounts") unknown_mounts();
        else if (scenario == "layout_bounds") layout_bounds();
        else if (scenario == "invalid_layout") invalid_layout();
        else if (scenario == "closed_stdin") closed_stdin();
        else if (scenario == "large_stdio") large_stdio(argv[0]);
        else if (scenario == "numeric_only") numeric_only();
        else throw std::runtime_error("unknown scenario");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

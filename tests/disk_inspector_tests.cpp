#include "core/disk_target.hpp"
#include "platform/detail/disk_inspection.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
litewin::ProcessResult lsblk_result;
int signature_calls = 0;
litewin::ProcessResult fake_run(const std::string& command,
                               const std::vector<std::string>& arguments,
                               const std::string& input) {
    require(input.empty(), "inspection must not send stdin");
    if (command == "lsblk") return lsblk_result;
    require(command == "wipefs", "unexpected tool");
    require(arguments.size() == 6 && arguments[3] == "--no-act", "signature inspection must be read-only");
    ++signature_calls;
    litewin::ProcessResult result;
    result.executed = true;
    if (arguments.back() == "/dev/sda") result.stdout_output = R"({"signatures":[]})";
    else { result.exit_code = 1; result.stderr_output = "Permission denied"; }
    return result;
}
void query_failure(bool missing) {
    lsblk_result.executed = !missing;
    lsblk_result.exit_code = missing ? 127 : 32;
    lsblk_result.error_message = missing ? "Executable not found" : "";
    lsblk_result.stderr_output = "synthetic failure";
    const auto result = litewin::detail::inspect_disks_with(fake_run);
    const auto* error = std::get_if<litewin::DiskInspectionError>(&result);
    require(error && error->operation == "lsblk execution" && !error->message.empty(), "query failure must return an inspection error");
    require(signature_calls == 0, "must not scan after failed query");
}
void malformed_inventory() {
    lsblk_result.executed = true;
    for (const auto* output : {"invalid JSON", "{}", R"({"blockdevices":[{"name":"sda"}]})"}) {
        lsblk_result.stdout_output = output;
        const auto result = litewin::detail::inspect_disks_with(fake_run);
        const auto* error = std::get_if<litewin::DiskInspectionError>(&result);
        require(error && error->operation == "parse_disk_inventory", "malformed inventory must return a parse error");
    }
    require(signature_calls == 0, "must not scan incomplete inventory");
}
void empty_inventory() {
    lsblk_result.executed = true;
    lsblk_result.stdout_output = R"({"blockdevices":[]})";
    const auto result = litewin::detail::inspect_disks_with(fake_run);
    require(std::get<litewin::DiskInventory>(result).empty(), "no disks is a successful empty query");
    require(signature_calls == 0, "empty inventory must not trigger signature scanning");
}
void independent_disks() {
    lsblk_result.executed = true;
    lsblk_result.stdout_output = R"({"blockdevices":[
      {"name":"sda","path":"/dev/sda","type":"disk","size":34359738368,"mountpoints":[null]},
      {"name":"sdb","path":"/dev/sdb","type":"disk","size":34359738368,"mountpoints":[null]},
      {"name":"sr0","path":"/dev/sr0","type":"rom","size":1000,"mountpoints":[]}
    ]})";
    const auto result = litewin::detail::inspect_disks_with(fake_run);
    const auto& inventory = std::get<litewin::DiskInventory>(result);
    require(inventory.size() == 3 && signature_calls == 2, "scan disks only and preserve other device types");
    require(litewin::evaluate_disk_target(inventory[0]).can_install(), "successful scan should remain eligible");
    require(!litewin::evaluate_disk_target(inventory[1]).can_install() && !inventory[1].has_signatures.has_value(),
            "one failed scan must remain unknown without rejecting the whole inventory");
    require(!litewin::evaluate_disk_target(inventory[2]).can_install(), "ROM must not be an installation target");
}
void native_smoke() {
    const auto result = litewin::inspect_disks();
    if (const auto* error = std::get_if<litewin::DiskInspectionError>(&result)) {
        throw std::runtime_error(error->operation + ": " + error->message);
    }
    const auto& inventory = std::get<litewin::DiskInventory>(result);
    std::size_t disks = 0, unknown = 0, eligible = 0;
    for (const auto& device : inventory) {
        require(!device.name.empty() && !device.path.empty() && !device.type.empty(), "incomplete native device");
        if (device.type != "disk") continue;
        ++disks;
        if (!device.has_signatures.has_value()) {
            ++unknown;
            require(!litewin::evaluate_disk_target(device).can_install(), "unknown native signatures must be rejected");
        }
        eligible += litewin::evaluate_disk_target(device).can_install();
    }
    std::cout << "Read-only inventory: devices=" << inventory.size() << " disks=" << disks
              << " signatures_unknown=" << unknown << " eligible=" << eligible << '\n';
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "scenario required");
        const std::string_view scenario = argv[1];
        if (scenario == "native_smoke") native_smoke();
        else if (scenario == "missing_tool") query_failure(true);
        else if (scenario == "query_failure") query_failure(false);
        else if (scenario == "malformed_inventory") malformed_inventory();
        else if (scenario == "empty_inventory") empty_inventory();
        else if (scenario == "independent_disks") independent_disks();
        else throw std::runtime_error("unknown scenario");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

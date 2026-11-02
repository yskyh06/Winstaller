#include "core/disk_inventory.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view minimal_node =
    R"({"name":"sda","path":"/dev/sda","type":"disk","size":0})";

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string wrap(std::string_view nodes) {
    return "{\"blockdevices\":[" + std::string(nodes) + "]}";
}

std::string with_field(std::string_view field) {
    auto node = std::string(minimal_node);
    node.pop_back();
    return wrap(node + "," + std::string(field) + "}");
}

std::string fixture(const char* filename) {
    std::ifstream input(std::filesystem::path(FIXTURE_DIR) / filename);
    require(input.is_open(), std::string("cannot open fixture: ") + filename);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

litewin::DiskInventory success(std::string_view input) {
    auto result = litewin::parse_disk_inventory(input);
    if (const auto* error = std::get_if<litewin::ParseError>(&result)) {
        throw std::runtime_error(error->location + ": " + error->message);
    }
    return std::get<litewin::DiskInventory>(std::move(result));
}

void failure(std::string_view input, litewin::ParseErrorCode code,
             std::string_view location) {
    const auto result = litewin::parse_disk_inventory(input);
    const auto* error = std::get_if<litewin::ParseError>(&result);
    require(error != nullptr, "expected error for input: " + std::string(input));
    require(error->code == code, "wrong error code");
    require(error->location == location, "wrong error location: " + error->location);
    require(!error->message.empty(), "error must contain a reason");
}

void schema_failure(std::string_view input, std::string_view location) {
    failure(input, litewin::ParseErrorCode::invalid_schema, location);
}

void empty_inventory() {
    require(success(fixture("empty.json")).empty(), "empty array must succeed");
}

void mixed_tree() {
    const auto devices = success(fixture("mixed-tree.json"));
    require(devices.size() == 3, "must preserve all top-level device types");
    const auto& disk = devices[0];
    require(disk.name == "nvme0n1" && disk.path == "/dev/nvme0n1" && disk.type == "disk",
            "disk identity must survive parsing");
    require(disk.model == "Example NVMe" && disk.serial == "TEST-NVME-001"
                && disk.transport == "nvme", "metadata must survive parsing");
    require(disk.children.size() == 2 && disk.children[0].type == "part",
            "partitions must remain children");
    const auto& volume = disk.children[1].children.at(0);
    require(volume.type == "lvm" && volume.path == "/dev/mapper/system-root",
            "nonpartition descendants must remain in the tree");
    require(volume.mountpoints == std::vector<std::string>{"/", "/mnt/작업 공간"},
            "multiple mountpoints and UTF-8 paths must be preserved");
    require(devices[1].children[0].mountpoints == std::vector<std::string>{"[SWAP]"},
            "swap marker must be preserved");
    require(devices[2].type == "loop" && devices[2].children.empty(),
            "loop device must not be filtered out");
}

void optional_metadata() {
    const auto missing = success(wrap(minimal_node)).at(0);
    require(!missing.model && !missing.serial && !missing.transport,
            "missing metadata must be unknown");
    const auto nulls = success(with_field(R"("model":null,"serial":null,"tran":null)")).at(0);
    require(!nulls.model && !nulls.serial && !nulls.transport,
            "null metadata must be unknown");
    const auto empty = success(with_field(R"("model":"","serial":"","tran":"")")).at(0);
    require(empty.model && empty.model->empty() && empty.serial && empty.transport,
            "empty optional strings must be accepted without fabricated identifiers");
}

void mountpoint_states() {
    require(!success(wrap(minimal_node))[0].mountpoints, "missing mountpoints is unknown");
    require(!success(with_field(R"("mountpoints":null)"))[0].mountpoints,
            "null mountpoints is unknown");
    const auto empty = success(with_field(R"("mountpoints":[])"))[0].mountpoints;
    require(empty && empty->empty(), "supplied empty array is known");
    const auto null_entry = success(with_field(R"("mountpoints":[null])"))[0].mountpoints;
    require(null_entry && null_entry->empty(), "lsblk null array entries are not paths");
    const auto mixed = success(with_field(R"("mountpoints":[null,"/mnt/test",null,"[SWAP]"])"));
    require(mixed[0].mountpoints == std::vector<std::string>{"/mnt/test", "[SWAP]"},
            "only null entries may be omitted");
}

void large_size() {
    const auto devices = success(fixture("mixed-tree.json"));
    require(devices[0].size_bytes == 1099511627776ULL, "1 TiB must not be truncated");
    require(devices[1].size_bytes == 34359738368ULL, "32 GiB must not be truncated");
    require(success(wrap(minimal_node))[0].size_bytes == 0, "zero is valid inventory data");
}

void max_size() {
    const auto devices = success(wrap(
        R"({"name":"sda","path":"/dev/sda","type":"disk","size":18446744073709551615})"));
    require(devices[0].size_bytes == std::numeric_limits<std::uint64_t>::max(),
            "UINT64_MAX must survive parsing");
}

void unknown_fields() {
    const auto devices = success(with_field(R"("future_field":{"value":true})"));
    require(devices.size() == 1, "additional lsblk columns must be tolerated");
}

void malformed_json() {
    for (const auto input : {"", "{", "not json", "{\"blockdevices\":[]} trailing"}) {
        failure(input, litewin::ParseErrorCode::invalid_json, "$");
    }
}

void invalid_root() {
    for (const auto input : {"null", "[]", "42", "true", "\"text\""}) {
        schema_failure(input, "$");
    }
}

void missing_inventory() {
    schema_failure("{}", "$.blockdevices");
}

void invalid_inventory() {
    for (const auto input : {R"({"blockdevices":null})", R"({"blockdevices":{}})",
                            R"({"blockdevices":"disk"})"}) {
        schema_failure(input, "$.blockdevices");
    }
}

void missing_required_fields() {
    for (const auto* key : {"name", "path", "type", "size"}) {
        auto node = std::string(minimal_node);
        const auto start = node.find("\"" + std::string(key) + "\":");
        const auto comma = node.find(',', start);
        if (comma != std::string::npos) {
            node.erase(start, comma - start + 1);
        } else {
            node.erase(start - 1, node.size() - start);
        }
        schema_failure(wrap(node), "$.blockdevices[0]." + std::string(key));
    }
}

void invalid_required_strings() {
    for (const auto* key : {"name", "path", "type"}) {
        for (const auto* value : {"null", "12", "false", "[]", "{}", "\"\""}) {
            auto node = std::string(minimal_node);
            const auto start = node.find("\"" + std::string(key) + "\":")
                + std::string(key).size() + 3;
            const auto end = node.find(',', start);
            node.replace(start, end - start, value);
            schema_failure(wrap(node), "$.blockdevices[0]." + std::string(key));
        }
    }
}

void invalid_sizes() {
    for (const auto* value : {"-1", "null", "true", "\"4096\"", "\"4G\"", "1.5", "1.0",
                             "1e3", "18446744073709551616", "[]", "{}"}) {
        auto node = std::string(minimal_node);
        node.replace(node.find("\"size\":0") + 7, 1, value);
        schema_failure(wrap(node), "$.blockdevices[0].size");
    }
}

void invalid_optional_metadata() {
    for (const auto* key : {"model", "serial", "tran"}) {
        for (const auto* value : {"1", "false", "[]", "{}"}) {
            schema_failure(with_field("\"" + std::string(key) + "\":" + value),
                           "$.blockdevices[0]." + std::string(key));
        }
    }
}

void invalid_mountpoints() {
    for (const auto* value : {"false", "42", "{}", "\"/mnt/test\""}) {
        schema_failure(with_field("\"mountpoints\":" + std::string(value)),
                       "$.blockdevices[0].mountpoints");
    }
    for (const auto* value : {"false", "42", "{}", "[]"}) {
        schema_failure(with_field("\"mountpoints\":[\"/\"," + std::string(value) + "]"),
                       "$.blockdevices[0].mountpoints[1]");
    }
}

void invalid_children() {
    for (const auto* value : {"null", "{}", "true", "\"partition\""}) {
        schema_failure(with_field("\"children\":" + std::string(value)),
                       "$.blockdevices[0].children");
    }
    require(success(with_field(R"("children":[])"))[0].children.empty(),
            "explicit empty children must succeed");
}

void invalid_node() {
    for (const auto* value : {"null", "[]", "true", "42", "\"disk\""}) {
        schema_failure(wrap(value), "$.blockdevices[0]");
    }
}

void nested_error() {
    schema_failure(with_field(R"("children":[{"name":"sda1","path":"/dev/sda1","type":"part","size":-1}])"),
                   "$.blockdevices[0].children[0].size");
}

void no_partial_success() {
    schema_failure(wrap(std::string(minimal_node) + ",{}"), "$.blockdevices[1].name");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string_view, void (*)()>> scenarios = {
        {"empty_inventory", empty_inventory}, {"mixed_tree", mixed_tree},
        {"optional_metadata", optional_metadata}, {"mountpoint_states", mountpoint_states},
        {"large_size", large_size}, {"max_size", max_size},
        {"unknown_fields", unknown_fields}, {"malformed_json", malformed_json},
        {"invalid_root", invalid_root}, {"missing_inventory", missing_inventory},
        {"invalid_inventory", invalid_inventory}, {"missing_required_fields", missing_required_fields},
        {"invalid_required_strings", invalid_required_strings}, {"invalid_sizes", invalid_sizes},
        {"invalid_optional_metadata", invalid_optional_metadata}, {"invalid_mountpoints", invalid_mountpoints},
        {"invalid_children", invalid_children}, {"invalid_node", invalid_node},
        {"nested_error", nested_error}, {"no_partial_success", no_partial_success},
    };
    if (argc != 2) {
        std::cerr << "usage: disk_inventory_tests <scenario>\n";
        return 2;
    }
    for (const auto& [name, run] : scenarios) {
        if (name != argv[1]) {
            continue;
        }
        try {
            run();
            std::cout << "PASS " << name << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cerr << "unknown scenario: " << argv[1] << '\n';
    return 2;
}

#include "core/disk_inventory.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace litewin {
namespace {

using Json = nlohmann::json;

[[noreturn]] void schema_error(const std::string& location, std::string message) {
    throw ParseError{ParseErrorCode::invalid_schema, location, std::move(message)};
}

const Json& required_field(const Json& object, const char* key,
                           const std::string& location) {
    const auto field = object.find(key);
    if (field == object.end()) {
        schema_error(location + "." + key, "required field is missing");
    }
    return *field;
}

std::string required_string(const Json& object, const char* key,
                            const std::string& location) {
    const auto& field = required_field(object, key, location);
    if (!field.is_string() || field.get_ref<const std::string&>().empty()) {
        schema_error(location + "." + key, "expected a nonempty string");
    }
    return field.get<std::string>();
}

std::optional<std::string> optional_string(const Json& object, const char* key,
                                          const std::string& location) {
    const auto field = object.find(key);
    if (field == object.end() || field->is_null()) {
        return std::nullopt;
    }
    if (!field->is_string()) {
        schema_error(location + "." + key, "expected a string or null");
    }
    return field->get<std::string>();
}

std::uint64_t size_in_bytes(const Json& object, const std::string& location) {
    const auto& field = required_field(object, "size", location);
    // Test unsigned first: converting a uint64_t to int64_t could wrap.
    if (field.is_number_unsigned()) {
        return field.get<std::uint64_t>();
    }
    if (field.is_number_integer() && field.get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(field.get<std::int64_t>());
    }
    schema_error(location + ".size", "expected an integer between 0 and UINT64_MAX in bytes");
}

std::optional<std::vector<std::string>> parse_mountpoints(
    const Json& object, const std::string& location) {
    const auto field = object.find("mountpoints");
    if (field == object.end() || field->is_null()) {
        return std::nullopt;
    }
    if (!field->is_array()) {
        schema_error(location + ".mountpoints", "expected an array or null");
    }
    std::vector<std::string> result;
    for (std::size_t i = 0; i < field->size(); ++i) {
        const auto& entry = (*field)[i];
        if (entry.is_null()) {
            continue;
        }
        if (!entry.is_string()) {
            schema_error(location + ".mountpoints[" + std::to_string(i) + "]",
                         "expected a string or null");
        }
        result.push_back(entry.get<std::string>());
    }
    return result;
}

DiskInventory parse_nodes(const Json& nodes, const std::string& location) {
    if (!nodes.is_array()) {
        schema_error(location, "expected an array");
    }
    DiskInventory result;
    result.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto node_location = location + "[" + std::to_string(i) + "]";
        if (!node.is_object()) {
            schema_error(node_location, "expected a device object");
        }
        DiskInfo device{
            .name = required_string(node, "name", node_location),
            .path = required_string(node, "path", node_location),
            .type = required_string(node, "type", node_location),
            .size_bytes = size_in_bytes(node, node_location),
            .model = optional_string(node, "model", node_location),
            .serial = optional_string(node, "serial", node_location),
            .transport = optional_string(node, "tran", node_location),
            .mountpoints = parse_mountpoints(node, node_location),
            .children = {},
            .has_signatures = std::nullopt,
        };
        if (const auto children = node.find("children"); children != node.end()) {
            device.children = parse_nodes(*children, node_location + ".children");
        }
        result.push_back(std::move(device));
    }
    return result;
}

} // namespace

DiskInventoryResult parse_disk_inventory(std::string_view input) {
    try {
        const auto document = Json::parse(input.begin(), input.end());
        if (!document.is_object()) {
            schema_error("$", "expected an object");
        }
        return parse_nodes(required_field(document, "blockdevices", "$"), "$.blockdevices");
    } catch (const ParseError& error) {
        return error;
    } catch (const Json::exception& error) {
        return ParseError{ParseErrorCode::invalid_json, "$", error.what()};
    }
}

} // namespace litewin

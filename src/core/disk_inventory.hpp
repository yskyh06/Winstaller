#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace litewin {

// A node may be a disk, partition, loop device, LVM volume, etc.
// Preserve TYPE and children; installation eligibility is a separate concern.
struct DiskInfo {
    std::string name;
    std::string path;
    std::string type;
    std::uint64_t size_bytes;
    std::optional<std::string> model;
    std::optional<std::string> serial;
    std::optional<std::string> transport;

    // nullopt means the field was absent or null (unknown).
    // An empty vector means the supplied list contained no mount locations.
    // lsblk may emit null entries in that list; those entries are ignored.
    std::optional<std::vector<std::string>> mountpoints;
    std::vector<DiskInfo> children;

    // Set only by a successful read-only signature inspection; absent means unknown.
    std::optional<bool> has_signatures = std::nullopt;
};

using DiskInventory = std::vector<DiskInfo>;

enum class ParseErrorCode {
    invalid_json,
    invalid_schema,
};

struct ParseError {
    ParseErrorCode code;
    std::string location;
    std::string message;
};

using DiskInventoryResult = std::variant<DiskInventory, ParseError>;

// Parse lsblk JSON with NAME,PATH,TYPE,SIZE,MODEL,SERIAL,TRAN,MOUNTPOINTS.
// SIZE must be a nonnegative integer in bytes (lsblk --bytes).
// Input errors return ParseError; the function performs no I/O and never
// returns a partially parsed inventory. Resource exhaustion can still throw.
[[nodiscard]] DiskInventoryResult parse_disk_inventory(std::string_view input);

} // namespace litewin

#pragma once

#include <cstdint>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace litewin {

enum class NetworkInterfaceKind {
    unknown,
    loopback,
    ethernet, // Ethernet link type; virtual interfaces can have this type too.
    wifi,
    other,
};

enum class IpAddressFamily { ipv4, ipv6 };

struct InterfaceAddress {
    IpAddressFamily family;
    std::string address;
    std::uint32_t scope_id = 0; // IPv6 link-local scope; zero for IPv4.
    //C++20 이상 지원
    bool operator==(const InterfaceAddress&) const = default;
};

struct NetworkInterface {
    std::string name;
    NetworkInterfaceKind kind = NetworkInterfaceKind::unknown;
    bool is_up = false;       // IFF_UP: administratively enabled.
    bool is_running = false;  // IFF_RUNNING: kernel-reported resource state.
    bool is_loopback = false;
    std::vector<InterfaceAddress> addresses;
};

using NetworkInventory = std::vector<NetworkInterface>;

struct NetworkInspectionError {
    std::string operation;
    std::error_code code;
};

using NetworkInventoryResult = std::variant<NetworkInventory, NetworkInspectionError>;

[[nodiscard]] inline bool has_active_ethernet(const NetworkInventory& inventory) noexcept {
    for (const auto& iface : inventory) {
        if (iface.kind == NetworkInterfaceKind::ethernet && iface.is_up && iface.is_running) {
            for (const auto& addr : iface.addresses) {
                if (addr.family == IpAddressFamily::ipv4 && !addr.address.empty()) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace litewin

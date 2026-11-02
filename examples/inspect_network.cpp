#include "platform/network_inspector.hpp"

#include <iostream>

namespace {

const char* kind_name(litewin::NetworkInterfaceKind kind) {
    switch (kind) {
    case litewin::NetworkInterfaceKind::loopback: return "loopback";
    case litewin::NetworkInterfaceKind::ethernet: return "ethernet";
    case litewin::NetworkInterfaceKind::wifi: return "wifi";
    case litewin::NetworkInterfaceKind::other: return "other";
    case litewin::NetworkInterfaceKind::unknown: return "unknown";
    }
    return "unknown";
}

} // namespace

int main() {
    const auto result = litewin::inspect_network_interfaces();
    if (const auto* error = std::get_if<litewin::NetworkInspectionError>(&result)) {
        std::cerr << error->operation << ": " << error->code.message() << '\n';
        return 1;
    }
    const auto& interfaces = std::get<litewin::NetworkInventory>(result);
    if (interfaces.empty()) {
        std::cout << "No network interfaces found.\n";
    }
    for (const auto& interface : interfaces) {
        std::cout << interface.name << " kind=" << kind_name(interface.kind)
                  << " up=" << (interface.is_up ? "yes" : "no")
                  << " running=" << (interface.is_running ? "yes" : "no") << '\n';
        if (interface.addresses.empty()) {
            std::cout << "  No IP addresses assigned.\n";
        }
        for (const auto& address : interface.addresses) {
            std::cout << "  "
                      << (address.family == litewin::IpAddressFamily::ipv4 ? "IPv4 " : "IPv6 ")
                      << address.address;
            if (address.scope_id != 0) {
                std::cout << '%' << address.scope_id;
            }
            std::cout << '\n';
        }
    }
    std::cout << "Snapshot only; Internet connectivity has not been checked.\n";
}

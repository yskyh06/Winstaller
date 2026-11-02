#include "platform/network_inspector.hpp"
#include "platform/detail/network_inspection.hpp"

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>

#include <algorithm>
#include <cerrno>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

namespace litewin {
namespace {

NetworkInspectionError os_error(std::string operation, int error_number) {
    return {std::move(operation), std::error_code(error_number, std::generic_category())};
}

struct ObservedInterface {
    NetworkInterface info;
    std::optional<unsigned short> hardware_type;
};

NetworkInterfaceKind classify(const ObservedInterface& device,
                              const std::filesystem::path& sys_class_net) {
    if (device.info.is_loopback) {
        return NetworkInterfaceKind::loopback;
    }
    // Wi-Fi interfaces normally also report ARPHRD_ETHER, so query wireless
    // indicators first. Do not infer Wi-Fi from an interface-name prefix.
    const auto interface_path = sys_class_net / device.info.name;
    for (const auto* marker : {"wireless", "phy80211"}) {
        std::error_code error;
        if (std::filesystem::exists(interface_path / marker, error)) {
            return NetworkInterfaceKind::wifi;
        }
        if (error) {
            return NetworkInterfaceKind::unknown;
        }
    }
    std::error_code error;
    if (!std::filesystem::is_directory(interface_path, error) || error) {
        return NetworkInterfaceKind::unknown;
    }
    if (!device.hardware_type) {
        return NetworkInterfaceKind::unknown;
    }
    if (*device.hardware_type == ARPHRD_ETHER) {
        return NetworkInterfaceKind::ethernet;
    }
    return NetworkInterfaceKind::other;
}

NetworkInventoryResult collect(const ifaddrs* addresses,
                               const std::filesystem::path& sys_class_net) {
    std::map<std::string, ObservedInterface> observed;
    for (auto* entry = addresses; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_name == nullptr || entry->ifa_name[0] == '\0') {
            return os_error("getifaddrs returned an unnamed interface", EINVAL);
        }
        auto& device = observed[entry->ifa_name];
        device.info.name = entry->ifa_name;
        device.info.is_up = (entry->ifa_flags & IFF_UP) != 0;
        device.info.is_running = (entry->ifa_flags & IFF_RUNNING) != 0;
        device.info.is_loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
        if (entry->ifa_addr == nullptr) {
            continue; // A device can exist before it has an IP address.
        }

        const auto family = entry->ifa_addr->sa_family;
        if (family == AF_PACKET) {
            const auto* link = reinterpret_cast<const sockaddr_ll*>(entry->ifa_addr);
            device.hardware_type = link->sll_hatype;
            continue;
        }
        InterfaceAddress address{};
        const void* source = nullptr;
        if (family == AF_INET) {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
            address.family = IpAddressFamily::ipv4;
            source = &ipv4->sin_addr;
        } else if (family == AF_INET6) {
            const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(entry->ifa_addr);
            address.family = IpAddressFamily::ipv6;
            address.scope_id = ipv6->sin6_scope_id;
            source = &ipv6->sin6_addr;
        } else {
            continue;
        }
        char text[INET6_ADDRSTRLEN]{};
        if (inet_ntop(family, source, text, sizeof(text)) == nullptr) {
            return os_error("inet_ntop", errno);
        }
        address.address = text;
        device.info.addresses.push_back(std::move(address));
    }

    NetworkInventory inventory;
    inventory.reserve(observed.size());
    for (auto& [name, device] : observed) {
        device.info.kind = classify(device, sys_class_net);
        auto& ips = device.info.addresses;
        std::sort(ips.begin(), ips.end(), [](const auto& left, const auto& right) {
            return std::tie(left.family, left.address, left.scope_id)
                < std::tie(right.family, right.address, right.scope_id);
        });
        ips.erase(std::unique(ips.begin(), ips.end()), ips.end());
        inventory.push_back(std::move(device.info));
    }
    return inventory;
}

} // namespace

NetworkInventoryResult detail::inspect_network_interfaces_with(
    const NetworkInspectionBackend& backend) {
    ifaddrs* addresses = nullptr;
    if (backend.get_addresses(&addresses) != 0) {
        return os_error("getifaddrs", errno);
    }
    // Always release the list, including on early error and allocation failure.
    const std::unique_ptr<ifaddrs, void (*)(ifaddrs*)> owner(addresses, backend.free_addresses);
    return collect(owner.get(), backend.sys_class_net);
}

NetworkInventoryResult inspect_network_interfaces() {
    return detail::inspect_network_interfaces_with({getifaddrs, freeifaddrs, "/sys/class/net"});
}

} // namespace litewin

#pragma once

#include "core/network_interface.hpp"
#include "core/network_probe.hpp"

#include <string>

namespace litewin {

enum class NetworkActionKind {
    proceed,                  // Internet is reachable; proceed to next installation steps.
    require_network_settings, // Disconnected or Wi-Fi available; configuration required.
    no_adapters_found,        // No physical or wireless adapters detected (loopback only).
    endpoint_unreachable,     // Interface has IP, but internet probes failed.
};

struct NetworkDecision {
    NetworkActionKind action = NetworkActionKind::no_adapters_found;
    std::string reason;

    [[nodiscard]] bool can_proceed() const noexcept {
        return action == NetworkActionKind::proceed;
    }
};

[[nodiscard]] inline NetworkDecision decide_network_action(
    const NetworkInventory& inventory,
    const InternetReachabilityResult& reachability
) {
    if (reachability.is_online) {
        return NetworkDecision{
            .action = NetworkActionKind::proceed,
            .reason = "Internet connection confirmed reachable.",
        };
    }

    bool has_any_adapter = false;
    bool has_wifi = false;
    bool has_up_interface_with_ip = false;

    for (const auto& iface : inventory) {
        if (iface.is_loopback || iface.kind == NetworkInterfaceKind::loopback) {
            continue;
        }
        has_any_adapter = true;
        if (iface.kind == NetworkInterfaceKind::wifi) {
            has_wifi = true;
        }
        if (iface.is_up && iface.is_running && !iface.addresses.empty()) {
            has_up_interface_with_ip = true;
        }
    }

    if (!has_any_adapter) {
        return NetworkDecision{
            .action = NetworkActionKind::no_adapters_found,
            .reason = "No network adapters detected.",
        };
    }

    if (has_up_interface_with_ip) {
        return NetworkDecision{
            .action = NetworkActionKind::endpoint_unreachable,
            .reason = "Interface is active with IP, but internet endpoints are unreachable.",
        };
    }

    if (has_wifi) {
        return NetworkDecision{
            .action = NetworkActionKind::require_network_settings,
            .reason = "Wi-Fi adapter detected but not connected.",
        };
    }

    return NetworkDecision{
        .action = NetworkActionKind::require_network_settings,
        .reason = "Wired network is disconnected or unconfigured.",
    };
}

} // namespace litewin

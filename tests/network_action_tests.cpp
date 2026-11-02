#include "core/network_action.hpp"
#include "platform/network_configurator.hpp"
#include "platform/network_inspector.hpp"
#include "platform/network_probe.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

// Case 1: Wired Happy Path (Ethernet active with IP, probe successful)
void test_case1_wired_happy() {
    litewin::NetworkInventory inv;
    inv.push_back(litewin::NetworkInterface{
        .name = "eth0",
        .kind = litewin::NetworkInterfaceKind::ethernet,
        .is_up = true,
        .is_running = true,
        .is_loopback = false,
        .addresses = {
            litewin::InterfaceAddress{
                .family = litewin::IpAddressFamily::ipv4,
                .address = "192.168.1.10",
            },
        },
    });

    litewin::InternetReachabilityResult reachability;
    reachability.is_online = true;

    auto decision = litewin::decide_network_action(inv, reachability);
    assert(decision.can_proceed());
    assert(decision.action == litewin::NetworkActionKind::proceed);
}

// Case 2: Wi-Fi adapter detected, but disconnected / not configured
void test_case2_wifi_disconnected() {
    litewin::NetworkInventory inv;
    inv.push_back(litewin::NetworkInterface{
        .name = "wlan0",
        .kind = litewin::NetworkInterfaceKind::wifi,
        .is_up = false,
        .is_running = false,
        .is_loopback = false,
        .addresses = {},
    });

    litewin::InternetReachabilityResult reachability;
    reachability.is_online = false;

    auto decision = litewin::decide_network_action(inv, reachability);
    assert(!decision.can_proceed());
    assert(decision.action == litewin::NetworkActionKind::require_network_settings);
    assert(decision.reason.find("Wi-Fi") != std::string::npos);
}

// Case 3: Wired cable disconnected / DHCP unassigned
void test_case3_wired_disconnected() {
    litewin::NetworkInventory inv;
    inv.push_back(litewin::NetworkInterface{
        .name = "eth0",
        .kind = litewin::NetworkInterfaceKind::ethernet,
        .is_up = false,
        .is_running = false,
        .is_loopback = false,
        .addresses = {},
    });

    litewin::InternetReachabilityResult reachability;
    reachability.is_online = false;

    auto decision = litewin::decide_network_action(inv, reachability);
    assert(!decision.can_proceed());
    assert(decision.action == litewin::NetworkActionKind::require_network_settings);
}

// Case 4: No network adapters detected (loopback only)
void test_case4_no_adapters() {
    litewin::NetworkInventory inv;
    inv.push_back(litewin::NetworkInterface{
        .name = "lo",
        .kind = litewin::NetworkInterfaceKind::loopback,
        .is_up = true,
        .is_running = true,
        .is_loopback = true,
        .addresses = {},
    });

    litewin::InternetReachabilityResult reachability;
    reachability.is_online = false;

    auto decision = litewin::decide_network_action(inv, reachability);
    assert(!decision.can_proceed());
    assert(decision.action == litewin::NetworkActionKind::no_adapters_found);
}

// Case 5: Local IP assigned, but internet probes fail (captive portal / WAN down)
void test_case5_endpoint_unreachable() {
    litewin::NetworkInventory inv;
    inv.push_back(litewin::NetworkInterface{
        .name = "eth0",
        .kind = litewin::NetworkInterfaceKind::ethernet,
        .is_up = true,
        .is_running = true,
        .is_loopback = false,
        .addresses = {
            litewin::InterfaceAddress{
                .family = litewin::IpAddressFamily::ipv4,
                .address = "192.168.1.50",
            },
        },
    });

    litewin::InternetReachabilityResult reachability;
    reachability.is_online = false;

    auto decision = litewin::decide_network_action(inv, reachability);
    assert(!decision.can_proceed());
    assert(decision.action == litewin::NetworkActionKind::endpoint_unreachable);
}

// Case 6: Wi-Fi connected and online
void test_case6_wifi_happy() {
    litewin::NetworkInventory inv;
    inv.push_back(litewin::NetworkInterface{
        .name = "wlan0",
        .kind = litewin::NetworkInterfaceKind::wifi,
        .is_up = true,
        .is_running = true,
        .is_loopback = false,
        .addresses = {
            litewin::InterfaceAddress{
                .family = litewin::IpAddressFamily::ipv4,
                .address = "10.0.0.15",
            },
        },
    });

    litewin::InternetReachabilityResult reachability;
    reachability.is_online = true;

    auto decision = litewin::decide_network_action(inv, reachability);
    assert(decision.can_proceed());
    assert(decision.action == litewin::NetworkActionKind::proceed);
}

// Case 7: Live smoke test using exactly 1 single ping to avoid IP rate-limiting
void test_case7_native_smoke_single_ping() {
    auto inv_res = litewin::inspect_network_interfaces();
    if (std::holds_alternative<litewin::NetworkInspectionError>(inv_res)) {
        return; // Skip gracefully if sandbox blocks inspection
    }
    const auto& inv = std::get<litewin::NetworkInventory>(inv_res);

    // Exactly 1 probe to 1.1.1.1 (1 attempt only)
    std::vector<litewin::ProbeTarget> targets = {
        litewin::ProbeTarget{
            .host = "1.1.1.1",
            .port = 53,
            .timeout = std::chrono::milliseconds(1500),
            .attempts = 1,
            .interval = std::chrono::milliseconds(0),
        },
    };

    auto reachability = litewin::probe_internet(targets, /*early_exit=*/true);
    assert(reachability.summaries.size() == 1);
    assert(reachability.summaries[0].attempts_made == 1);

    auto decision = litewin::decide_network_action(inv, reachability);
    std::cout << "[Live Test] Decision Action: " << static_cast<int>(decision.action)
              << " (" << decision.reason << ")\n";

    if (reachability.is_online) {
        assert(decision.can_proceed());
        std::cout << "[Live Test] RTT: " << reachability.summaries[0].min_rtt.count() << " ms\n";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: network_action_tests <scenario>\n";
        return 1;
    }

    const std::string_view scenario = argv[1];
    if (scenario == "case1_wired_happy") {
        test_case1_wired_happy();
    } else if (scenario == "case2_wifi_disconnected") {
        test_case2_wifi_disconnected();
    } else if (scenario == "case3_wired_disconnected") {
        test_case3_wired_disconnected();
    } else if (scenario == "case4_no_adapters") {
        test_case4_no_adapters();
    } else if (scenario == "case5_endpoint_unreachable") {
        test_case5_endpoint_unreachable();
    } else if (scenario == "case6_wifi_happy") {
        test_case6_wifi_happy();
    } else if (scenario == "case7_native_smoke_single_ping") {
        test_case7_native_smoke_single_ping();
    } else {
        std::cerr << "Unknown scenario: " << scenario << "\n";
        return 1;
    }

    return 0;
}

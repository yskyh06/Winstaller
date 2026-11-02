#include "core/network_interface.hpp"
#include "core/network_probe.hpp"
#include "platform/network_probe.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

void test_has_active_ethernet() {
    litewin::NetworkInventory inv;
    assert(!litewin::has_active_ethernet(inv));

    // Ethernet down
    inv.push_back(litewin::NetworkInterface{
        .name = "eth0",
        .kind = litewin::NetworkInterfaceKind::ethernet,
        .is_up = false,
        .is_running = false,
        .is_loopback = false,
        .addresses = {},
    });
    assert(!litewin::has_active_ethernet(inv));

    // Ethernet up but no IP
    inv[0].is_up = true;
    inv[0].is_running = true;
    assert(!litewin::has_active_ethernet(inv));

    // Ethernet up with IPv4
    inv[0].addresses.push_back(litewin::InterfaceAddress{
        .family = litewin::IpAddressFamily::ipv4,
        .address = "192.168.1.100",
    });
    assert(litewin::has_active_ethernet(inv));

    // Wi-Fi only (should return false for has_active_ethernet)
    litewin::NetworkInventory wifi_inv;
    wifi_inv.push_back(litewin::NetworkInterface{
        .name = "wlan0",
        .kind = litewin::NetworkInterfaceKind::wifi,
        .is_up = true,
        .is_running = true,
        .addresses = {litewin::InterfaceAddress{.family = litewin::IpAddressFamily::ipv4, .address = "192.168.1.200"}},
    });
    assert(!litewin::has_active_ethernet(wifi_inv));
}

void test_invalid_address() {
    auto res = litewin::probe_single_endpoint("999.999.999.999", 53, std::chrono::milliseconds(50));
    assert(!res.success);
    assert(!res.error_message.empty());
}

void test_unreachable_timeout() {
    // 192.0.2.1 is RFC5737 TEST-NET-1 (documentation only, unroutable)
    litewin::ProbeTarget target{
        .host = "192.0.2.1",
        .port = 80,
        .timeout = std::chrono::milliseconds(50),
        .attempts = 3,
        .interval = std::chrono::milliseconds(10),
    };

    auto summary = litewin::probe_endpoint(target);
    assert(summary.attempts_made == 3);
    assert(summary.failure_count == 3);
    assert(summary.success_count == 0);
    assert(!summary.is_reachable());
    assert(summary.attempts.size() == 3);
    for (const auto& att : summary.attempts) {
        assert(!att.success);
    }
}

void test_native_smoke() {
    auto result = litewin::probe_internet(litewin::default_internet_probe_targets(), /*early_exit=*/true);
    // In environments with internet, result.is_online will be true.
    // If running in an offline sandbox, it may be false, but should not crash or hang.
    assert(result.summaries.size() == 2);
    for (const auto& s : result.summaries) {
        assert(s.attempts_made >= 1);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: network_probe_tests <scenario>\n";
        return 1;
    }

    const std::string_view scenario = argv[1];
    if (scenario == "has_active_ethernet") {
        test_has_active_ethernet();
    } else if (scenario == "invalid_address") {
        test_invalid_address();
    } else if (scenario == "unreachable_timeout") {
        test_unreachable_timeout();
    } else if (scenario == "native_smoke") {
        test_native_smoke();
    } else {
        std::cerr << "Unknown scenario: " << scenario << "\n";
        return 1;
    }

    return 0;
}

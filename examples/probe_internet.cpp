#include "core/network_action.hpp"
#include "platform/network_configurator.hpp"
#include "platform/network_inspector.hpp"
#include "platform/network_probe.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    bool interactive = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--interactive" || std::string_view(argv[i]) == "-i") {
            interactive = true;
        }
    }

    while (true) {
        std::cout << "[Litewin] Inspecting network interfaces...\n";
        auto inventory_result = litewin::inspect_network_interfaces();

        if (const auto* err = std::get_if<litewin::NetworkInspectionError>(&inventory_result)) {
            std::cerr << "Failed to inspect network interfaces: " << err->operation
                      << " (" << err->code.message() << ")\n";
            return 1;
        }

        const auto& inventory = std::get<litewin::NetworkInventory>(inventory_result);
        bool has_ethernet = litewin::has_active_ethernet(inventory);

        std::cout << "Active Ethernet detected: " << (has_ethernet ? "Yes" : "No") << "\n";
        for (const auto& iface : inventory) {
            std::cout << "  - " << iface.name << " (kind=" << static_cast<int>(iface.kind)
                      << ", up=" << iface.is_up << ", running=" << iface.is_running << ")\n";
            for (const auto& addr : iface.addresses) {
                std::cout << "      ip: " << addr.address << "\n";
            }
        }

        std::cout << "\n[Litewin] Testing internet reachability (3 attempts each to 1.1.1.1 and 8.8.8.8)...\n";
        auto probe_result = litewin::probe_internet(litewin::default_internet_probe_targets(), /*early_exit=*/false);

        for (const auto& summary : probe_result.summaries) {
            std::cout << "\nTarget: " << summary.target.host << ":" << summary.target.port << "\n";
            std::cout << "  Attempts: " << summary.attempts_made
                      << " | Success: " << summary.success_count
                      << " | Failure: " << summary.failure_count << "\n";

            for (const auto& att : summary.attempts) {
                std::cout << "    [" << att.attempt_index << "] "
                          << (att.success ? "SUCCESS" : "FAILED")
                          << " (RTT: " << att.rtt.count() << " ms)";
                if (!att.success && !att.error_message.empty()) {
                    std::cout << " - " << att.error_message;
                }
                std::cout << "\n";
            }

            if (summary.is_reachable()) {
                std::cout << "  RTT Min/Avg/Max: " << summary.min_rtt.count() << " / "
                          << summary.avg_rtt.count() << " / "
                          << summary.max_rtt.count() << " ms\n";
            }
        }

        auto decision = litewin::decide_network_action(inventory, probe_result);
        std::cout << "\n[Decision] Action: " << static_cast<int>(decision.action)
                  << " | Reason: " << decision.reason << "\n";

        if (decision.can_proceed()) {
            std::cout << "\n[Success] Network is ready for Windows installation workflow.\n";
            return 0;
        }

        if (!interactive) {
            std::cout << "\n[Notice] Pass --interactive (-i) to open network configuration tool.\n";
            return 2;
        }

        std::cout << "\nOpening network configuration tool...\n";
        auto config_res = litewin::open_network_settings();
        if (!config_res.executed) {
            std::cerr << "Configuration tool error: " << config_res.error_message << "\n";
            return 3;
        }

        std::cout << "Configuration tool finished. Re-checking network...\n\n";
    }
}

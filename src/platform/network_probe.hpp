#pragma once

#include "core/network_probe.hpp"

#include <vector>

namespace litewin {

[[nodiscard]] std::vector<ProbeTarget> default_internet_probe_targets();

// Numeric IPv4/IPv6 addresses only. Hostnames are rejected without DNS queries.
[[nodiscard]] ProbeAttemptResult probe_single_endpoint(
    const std::string& host,
    std::uint16_t port,
    std::chrono::milliseconds timeout
);

[[nodiscard]] ProbeSummary probe_endpoint(
    const ProbeTarget& target,
    bool early_exit = false
);

[[nodiscard]] InternetReachabilityResult probe_internet(
    const std::vector<ProbeTarget>& targets = default_internet_probe_targets(),
    bool early_exit = false
);

} // namespace litewin

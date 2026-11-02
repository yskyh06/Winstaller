#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace litewin {

struct ProbeTarget {
    // Numeric IPv4/IPv6 address; hostnames are unsupported.
    std::string host = "1.1.1.1";
    std::uint16_t port = 53;
    std::chrono::milliseconds timeout{1000};
    int attempts = 3;
    std::chrono::milliseconds interval{200};
};

struct ProbeAttemptResult {
    int attempt_index = 0;
    bool success = false;
    std::chrono::milliseconds rtt{0};
    std::string error_message;
};

struct ProbeSummary {
    ProbeTarget target;
    int attempts_made = 0;
    int success_count = 0;
    int failure_count = 0;
    std::chrono::milliseconds min_rtt{0};
    std::chrono::milliseconds max_rtt{0};
    std::chrono::milliseconds avg_rtt{0};
    std::vector<ProbeAttemptResult> attempts;

    [[nodiscard]] bool is_reachable() const noexcept {
        return success_count > 0;
    }
};

struct InternetReachabilityResult {
    bool is_online = false;
    std::vector<ProbeSummary> summaries;
};

} // namespace litewin

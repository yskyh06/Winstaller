#include "platform/network_probe.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <climits>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace litewin {

namespace {

class ScopedSocket {
public:
    explicit ScopedSocket(int fd) noexcept : fd_(fd) {}
    ~ScopedSocket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;
    ScopedSocket(ScopedSocket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    ScopedSocket& operator=(ScopedSocket&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

} // namespace

std::vector<ProbeTarget> default_internet_probe_targets() {
    return {
        ProbeTarget{
            .host = "1.1.1.1",
            .port = 53,
            .timeout = std::chrono::milliseconds(1000),
            .attempts = 3,
            .interval = std::chrono::milliseconds(200),
        },
        ProbeTarget{
            .host = "8.8.8.8",
            .port = 53,
            .timeout = std::chrono::milliseconds(1000),
            .attempts = 3,
            .interval = std::chrono::milliseconds(200),
        },
    };
}

ProbeAttemptResult probe_single_endpoint(
    const std::string& host,
    std::uint16_t port,
    std::chrono::milliseconds timeout
) {
    ProbeAttemptResult result;
    const auto start_time = std::chrono::steady_clock::now();

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST; // Avoid blocking DNS resolution during IP probe

    struct addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        result.error_message = "Expected a numeric IPv4 or IPv6 address: " + std::string(gai_strerror(rc));
        result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }

    struct FreeAddrInfoGuard {
        struct addrinfo* ptr;
        ~FreeAddrInfoGuard() {
            if (ptr) {
                ::freeaddrinfo(ptr);
            }
        }
    } guard{res};

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        result.success = false;
        result.error_message = std::strerror(errno);
        result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }
    ScopedSocket sock(fd);

    int flags = ::fcntl(sock.get(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(sock.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
        result.success = false;
        result.error_message = std::strerror(errno);
        result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }

    rc = ::connect(sock.get(), res->ai_addr, res->ai_addrlen);
    if (rc == 0) {
        // Immediate connection
        result.success = true;
        result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }

    if (errno != EINPROGRESS) {
        result.success = false;
        result.error_message = std::strerror(errno);
        result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }

    struct pollfd pfd {};
    pfd.fd = sock.get();
    pfd.events = POLLOUT;

    const int timeout_ms = static_cast<int>(std::clamp<std::int64_t>(timeout.count(), 0, INT_MAX));
    const int poll_res = ::poll(&pfd, 1, timeout_ms);

    if (poll_res == 0) {
        result.success = false;
        result.error_message = "Connection timed out";
        result.rtt = timeout;
        return result;
    }

    if (poll_res < 0) {
        result.success = false;
        result.error_message = std::strerror(errno);
        result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }

    int socket_error = 0;
    socklen_t len = sizeof(socket_error);
    if (::getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &socket_error, &len) < 0) {
        result.success = false;
        result.error_message = std::strerror(errno);
    } else if (socket_error != 0) {
        result.success = false;
        result.error_message = std::strerror(socket_error);
    } else {
        result.success = true;
    }

    result.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);
    return result;
}

ProbeSummary probe_endpoint(const ProbeTarget& target, bool early_exit) {
    ProbeSummary summary;
    summary.target = target;

    std::chrono::milliseconds total_rtt{0};
    bool first_success = true;

    for (int i = 0; i < target.attempts; ++i) {
        if (i > 0 && target.interval.count() > 0) {
            std::this_thread::sleep_for(target.interval);
        }

        ProbeAttemptResult attempt = probe_single_endpoint(target.host, target.port, target.timeout);
        attempt.attempt_index = i + 1;
        summary.attempts_made++;

        if (attempt.success) {
            summary.success_count++;
            total_rtt += attempt.rtt;
            if (first_success || attempt.rtt < summary.min_rtt) {
                summary.min_rtt = attempt.rtt;
            }
            if (first_success || attempt.rtt > summary.max_rtt) {
                summary.max_rtt = attempt.rtt;
            }
            first_success = false;
        } else {
            summary.failure_count++;
        }

        summary.attempts.push_back(std::move(attempt));

        if (early_exit && summary.success_count > 0) {
            break;
        }
    }

    if (summary.success_count > 0) {
        summary.avg_rtt = total_rtt / summary.success_count;
    }

    return summary;
}

InternetReachabilityResult probe_internet(
    const std::vector<ProbeTarget>& targets,
    bool early_exit
) {
    InternetReachabilityResult result;
    result.is_online = false;

    const auto default_targets = default_internet_probe_targets();
    const auto& active_targets = targets.empty() ? default_targets : targets;

    for (const auto& target : active_targets) {
        ProbeSummary summary = probe_endpoint(target, early_exit);
        if (summary.is_reachable()) {
            result.is_online = true;
        }
        result.summaries.push_back(std::move(summary));
    }

    return result;
}

} // namespace litewin

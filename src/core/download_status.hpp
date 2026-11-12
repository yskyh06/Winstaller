#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace litewin {

enum class DownloadPhase {
    Connecting,
    Downloading,
    Verifying,
    Complete,
    Failed,
    Cached
};

enum class DownloadOrigin {
    Official,
    Mirror,
    Unknown
};

enum class DownloadTransport {
    Pending,
    HttpsVerified,
    Http,
    Local,
    Failed
};

enum class DownloadHash {
    NotProvided,
    Pending,
    Verifying,
    Matched,
    Mismatch
};

struct DownloadStatus {
    DownloadPhase phase = DownloadPhase::Connecting;
    DownloadOrigin origin = DownloadOrigin::Unknown;
    DownloadTransport transport = DownloadTransport::Pending;
    DownloadHash hash = DownloadHash::Pending;
    uint64_t downloaded_bytes = 0;
    uint64_t total_bytes = 0;
    uint64_t resumed_bytes = 0;
    double bytes_per_second = 0.0;
    std::optional<double> eta_seconds = std::nullopt;
    std::string host = "";
};

inline constexpr uint64_t safe_add(uint64_t a, uint64_t b) noexcept {
    if (std::numeric_limits<uint64_t>::max() - a < b) {
        return std::numeric_limits<uint64_t>::max();
    }
    return a + b;
}

inline bool is_http_scheme(std::string_view url) noexcept {
    return url.starts_with("http://") || url.starts_with("https://");
}

inline std::string sanitize_download_host(std::string_view url) {
    for (char c : url) {
        auto uc = static_cast<unsigned char>(c);
        if (uc <= 32 || uc == 127) {
            return "";
        }
    }

    std::string_view s = url;
    auto scheme_pos = s.find("://");
    if (scheme_pos != std::string_view::npos) {
        std::string_view scheme = s.substr(0, scheme_pos);
        if (scheme.empty()) return "";
        for (char c : scheme) {
            auto uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '+' && c != '-' && c != '.') {
                return "";
            }
        }
        s = s.substr(scheme_pos + 3);
    }

    auto auth_end = s.find_first_of("/?#");
    if (auth_end != std::string_view::npos) {
        s = s.substr(0, auth_end);
    }
    if (s.empty()) return "";

    auto at_pos = s.rfind('@');
    if (at_pos != std::string_view::npos) {
        s = s.substr(at_pos + 1);
    }
    if (s.empty()) return "";

    std::string_view host_part;
    if (s.front() == '[') {
        auto close_bracket = s.find(']');
        if (close_bracket == std::string_view::npos) {
            return "";
        }
        host_part = s.substr(1, close_bracket - 1);
        std::string_view rest = s.substr(close_bracket + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return "";
            std::string_view port = rest.substr(1);
            if (port.empty()) return "";
            for (char c : port) {
                if (c < '0' || c > '9') return "";
            }
        }
    } else {
        auto colon_pos = s.find(':');
        if (colon_pos != std::string_view::npos) {
            if (s.rfind(':') != colon_pos) {
                return "";
            }
            host_part = s.substr(0, colon_pos);
            std::string_view port = s.substr(colon_pos + 1);
            if (port.empty()) return "";
            for (char c : port) {
                if (c < '0' || c > '9') return "";
            }
        } else {
            host_part = s;
        }
    }

    if (host_part.empty()) return "";

    std::string host;
    host.reserve(host_part.size());
    for (char c : host_part) {
        auto uc = static_cast<unsigned char>(c);
        char lower = static_cast<char>(std::tolower(uc));
        bool valid = (lower >= 'a' && lower <= 'z') ||
                     (lower >= '0' && lower <= '9') ||
                     lower == '.' || lower == '-' || lower == '_' || lower == ':';
        if (!valid) {
            return "";
        }
        host.push_back(lower);
    }

    if (host.find("..") != std::string::npos) return "";
    if (host.front() == '.' || host.back() == '.') return "";

    return host;
}

inline DownloadOrigin classify_download_origin(std::string_view host) {
    if (host.empty()) {
        return DownloadOrigin::Unknown;
    }

    if (host == "releases.ubuntu.com" ||
        host == "cdimage.ubuntu.com" ||
        host == "archive.ubuntu.com" ||
        host == "security.ubuntu.com" ||
        host == "download.windowsupdate.com" ||
        host == "dl.delivery.mp.microsoft.com") {
        return DownloadOrigin::Official;
    }

    constexpr std::string_view winup = ".download.windowsupdate.com";
    if (host.size() > winup.size() && host.ends_with(winup)) {
        return DownloadOrigin::Official;
    }

    return DownloadOrigin::Unknown;
}

class DownloadMeter {
public:
    explicit DownloadMeter(uint64_t resume = 0) {
        reset(resume);
    }

    void reset(uint64_t resume = 0) {
        resumed_bytes_ = resume;
        samples_.clear();
        has_started_ = false;
        start_time_ = {};
        last_sample_time_ = {};
        last_session_bytes_ = 0;
        last_progress_time_ = {};
        last_progress_bytes_ = 0;
    }

    DownloadStatus sample(uint64_t session_bytes, uint64_t session_total,
                          std::chrono::steady_clock::time_point now) {
        if (!has_started_ || session_bytes < last_session_bytes_ || (has_started_ && now < last_sample_time_)) {
            samples_.clear();
            has_started_ = true;
            start_time_ = now;
            last_progress_time_ = now;
            last_progress_bytes_ = session_bytes;
        }

        if (session_bytes > last_progress_bytes_) {
            last_progress_bytes_ = session_bytes;
            last_progress_time_ = now;
        }

        samples_.push_back({now, session_bytes});

        constexpr auto window = std::chrono::seconds(5);
        while (samples_.size() > 1 && (now - samples_[1].time) > window) {
            samples_.pop_front();
        }
        while (samples_.size() > 64) {
            samples_.pop_front();
        }

        last_sample_time_ = now;
        last_session_bytes_ = session_bytes;

        DownloadStatus status;
        status.resumed_bytes = resumed_bytes_;
        status.downloaded_bytes = safe_add(resumed_bytes_, session_bytes);
        status.total_bytes = (session_total > 0) ? safe_add(resumed_bytes_, session_total) : 0;

        if (session_total > 0 && session_bytes >= session_total) {
            status.phase = DownloadPhase::Complete;
        } else if (status.downloaded_bytes > 0) {
            status.phase = DownloadPhase::Downloading;
        } else {
            status.phase = DownloadPhase::Connecting;
        }

        bool is_stalled = (now - last_progress_time_) >= std::chrono::seconds(3);

        double rate = 0.0;
        if (!is_stalled && samples_.size() >= 2) {
            auto dt = std::chrono::duration<double>(now - samples_.front().time).count();
            if (dt > 0.001) {
                uint64_t dbytes = (session_bytes >= samples_.front().bytes)
                                      ? (session_bytes - samples_.front().bytes)
                                      : 0;
                rate = static_cast<double>(dbytes) / dt;
            }
        }
        status.bytes_per_second = rate;

        auto total_elapsed = std::chrono::duration<double>(now - start_time_).count();
        if (session_total == 0) {
            status.eta_seconds = std::nullopt;
        } else if (session_bytes >= session_total) {
            status.eta_seconds = 0.0;
        } else if (is_stalled || total_elapsed < 1.0 || rate <= 0.0) {
            status.eta_seconds = std::nullopt;
        } else {
            uint64_t remaining_bytes = session_total - session_bytes;
            status.eta_seconds = static_cast<double>(remaining_bytes) / rate;
        }

        return status;
    }

private:
    struct SamplePoint {
        std::chrono::steady_clock::time_point time;
        uint64_t bytes = 0;
    };

    uint64_t resumed_bytes_ = 0;
    std::deque<SamplePoint> samples_;
    bool has_started_ = false;
    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point last_sample_time_{};
    uint64_t last_session_bytes_ = 0;
    std::chrono::steady_clock::time_point last_progress_time_{};
    uint64_t last_progress_bytes_ = 0;
};

} // namespace litewin

#pragma once
#include "core/download_status.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace litewin {

enum class DownloadErrorKind {
    none,
    http_error,               // 404, 500, etc.
    forbidden_or_expired,     // 403 Forbidden (Link expired or IP mismatch)
    rate_limited,             // 429 Too Many Requests
    network_failure,          // DNS or socket timeout
    insufficient_disk_space,  // ENOSPC or pre-check failure
    hash_mismatch,            // Checksum verification failed
    aborted,                  // User canceled
    invalid_content_type,     // Captive portal HTML detected instead of binary
    etag_mismatch_reset,      // Server file changed; partial reset required
};

struct DownloadSessionCache {
    std::string url;
    std::string etag;
    std::string last_modified;
    std::uint64_t content_length = 0;
    std::int64_t created_at_epoch_sec = 0;
    std::int64_t expires_at_epoch_sec = 0; // Microsoft token expires in ~24h
};

// Validates whether the cached download link is still safe to use (default: 4-hour safety buffer)
[[nodiscard]] inline bool is_download_session_valid(
    const DownloadSessionCache& cache,
    std::int64_t current_epoch_sec,
    int safety_buffer_seconds = 4 * 3600
) noexcept {
    if (cache.url.empty() || cache.expires_at_epoch_sec <= 0) {
        return false;
    }
    // Expired or within the safety buffer window
    if (current_epoch_sec + safety_buffer_seconds >= cache.expires_at_epoch_sec) {
        return false;
    }
    return true;
}

struct DownloadOptions {
    bool strict_official_source=false;
    std::uint64_t expected_size=0;
    std::string url;
    std::string destination_path;
    std::string expected_sha256;          // Optional; if non-empty, triggers hash verification
    std::uint64_t minimum_expected_bytes = 100ULL * 1024 * 1024; // 100 MB floor (Captive portal defense)
    std::uint32_t connect_timeout_seconds = 15;
    std::uint32_t low_speed_limit_bytes = 10240; // 10 KiB/s
    std::uint32_t low_speed_time_seconds = 30;
    std::string expected_etag;            // If set and changes, aborts/resets partial file
    std::function<bool(std::uint64_t downloaded, std::uint64_t total)> progress_callback;
    std::function<void(const DownloadStatus&)> status_callback;
};

struct DownloadResult {
    DownloadStatus status;
    bool success = false;
    DownloadErrorKind error_kind = DownloadErrorKind::none;
    long http_status_code = 0;
    std::uint64_t bytes_downloaded = 0;
    std::uint64_t total_expected_bytes = 0;
    std::string etag_received;
    std::string computed_sha256;
    std::string error_message;
};

} // namespace litewin

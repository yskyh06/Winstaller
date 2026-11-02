#pragma once

#include "core/download_types.hpp"

#include <optional>
#include <string>

namespace litewin {

struct MidoResolverOptions {
    std::string product_name = "win11"; // "win11", "win10"
    std::string language = "Korean";    // "Korean", "English", etc.
    std::string architecture = "x64";
    std::string cache_filepath = "/tmp/litewin_session.json";
    std::string mido_script_path = "scripts/fetch_win_link.sh";
    int safe_buffer_seconds = 4 * 3600; // 4 hours safety window
};

struct MidoResolveResult {
    bool success = false;
    bool from_cache = false;
    std::string url;
    std::string filename;
    std::string expected_sha256;
    std::int64_t expires_at_epoch_sec = 0;
    std::string error_message;
};

// Saves session cache to JSON file on disk
bool save_download_session_cache(
    const std::string& filepath,
    const DownloadSessionCache& cache
);

// Loads session cache from JSON file on disk
[[nodiscard]] std::optional<DownloadSessionCache> load_download_session_cache(
    const std::string& filepath
);

class MidoResolver {
public:
    // Resolves official download link.
    // ALWAYS checks local cache first; only invokes Mido.sh if cache is missing or expired.
    [[nodiscard]] static MidoResolveResult resolve(
        const MidoResolverOptions& options,
        std::int64_t current_epoch_sec = 0 // 0 means use system clock
    );
};

} // namespace litewin

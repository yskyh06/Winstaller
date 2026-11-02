#include "platform/mido_resolver.hpp"
#include "platform/process_runner.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace litewin {

bool save_download_session_cache(
    const std::string& filepath,
    const DownloadSessionCache& cache
) {
    try {
        nlohmann::json j;
        j["url"] = cache.url;
        j["etag"] = cache.etag;
        j["last_modified"] = cache.last_modified;
        j["content_length"] = cache.content_length;
        j["created_at_epoch_sec"] = cache.created_at_epoch_sec;
        j["expires_at_epoch_sec"] = cache.expires_at_epoch_sec;

        std::filesystem::path p(filepath);
        if (!p.parent_path().empty()) {
            std::filesystem::create_directories(p.parent_path());
        }

        std::ofstream ofs(filepath);
        if (!ofs.is_open()) {
            return false;
        }
        ofs << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<DownloadSessionCache> load_download_session_cache(
    const std::string& filepath
) {
    try {
        if (!std::filesystem::exists(filepath)) {
            return std::nullopt;
        }

        std::ifstream ifs(filepath);
        if (!ifs.is_open()) {
            return std::nullopt;
        }

        auto j = nlohmann::json::parse(ifs);
        DownloadSessionCache cache;
        cache.url = j.value("url", "");
        cache.etag = j.value("etag", "");
        cache.last_modified = j.value("last_modified", "");
        cache.content_length = j.value("content_length", 0ULL);
        cache.created_at_epoch_sec = j.value("created_at_epoch_sec", 0LL);
        cache.expires_at_epoch_sec = j.value("expires_at_epoch_sec", 0LL);

        if (cache.url.empty()) {
            return std::nullopt;
        }
        return cache;
    } catch (...) {
        return std::nullopt;
    }
}

MidoResolveResult MidoResolver::resolve(
    const MidoResolverOptions& options,
    std::int64_t current_epoch_sec
) {
    MidoResolveResult result;

    if (current_epoch_sec == 0) {
        current_epoch_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    // 1. Check local session cache first (Zero network call defense)
    auto cached = load_download_session_cache(options.cache_filepath);
    if (cached.has_value() && is_download_session_valid(*cached, current_epoch_sec, options.safe_buffer_seconds)) {
        result.success = true;
        result.from_cache = true;
        result.url = cached->url;
        result.expires_at_epoch_sec = cached->expires_at_epoch_sec;
        return result;
    }

    // 2. Invoke Mido script to generate a fresh link
    if (!std::filesystem::exists(options.mido_script_path)) {
        result.success = false;
        result.error_message = "Mido script not found at: " + options.mido_script_path;
        return result;
    }

    std::vector<std::string> args;
    if (options.mido_script_path.find("fetch_win_link.sh") != std::string::npos) {
        std::string edition_id = (options.product_name == "win10") ? "19045" : "3813";
        std::string locale = (options.language == "Korean") ? "ko-kr" : "en-us";
        args = {options.mido_script_path, edition_id, options.language, locale};
    } else {
        args = {options.mido_script_path, options.product_name, options.language, options.architecture, "--url-only"};
    }

    auto proc = ProcessRunner::run("bash", args);

    if (!proc.success()) {
        result.success = false;
        result.error_message = proc.error_message.empty() ? proc.stderr_output : proc.error_message;
        if (result.error_message.empty()) {
            result.error_message = "Mido script failed with exit code " + std::to_string(proc.exit_code);
        }
        return result;
    }

    // 3. Parse generated URL from stdout
    std::istringstream iss(proc.stdout_output);
    std::string line;
    std::string final_url;

    while (std::getline(iss, line)) {
        // Strip carriage returns or whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.rfind("https://", 0) == 0 || line.rfind("http://", 0) == 0) {
            final_url = line;
            break;
        }
    }

    if (final_url.empty()) {
        result.success = false;
        result.error_message = "Failed to parse download URL from Mido script output:\n" + proc.stdout_output;
        return result;
    }

    // 4. Save to cache with 24-hour expiration timestamp
    DownloadSessionCache new_cache{
        .url = final_url,
        .etag = "",
        .last_modified = "",
        .content_length = 0,
        .created_at_epoch_sec = current_epoch_sec,
        .expires_at_epoch_sec = current_epoch_sec + (24 * 3600),
    };
    save_download_session_cache(options.cache_filepath, new_cache);

    result.success = true;
    result.from_cache = false;
    result.url = final_url;
    result.expires_at_epoch_sec = new_cache.expires_at_epoch_sec;
    return result;
}

} // namespace litewin

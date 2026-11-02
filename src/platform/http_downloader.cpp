#include "platform/http_downloader.hpp"
#include "core/hash_verifier.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <curl/curl.h>
#include <filesystem>
#include <memory>
#include <string_view>
#include <unistd.h>

namespace litewin {

namespace {

struct ScopedCurl {
    CURL* handle = nullptr;
    ScopedCurl() : handle(curl_easy_init()) {}
    ~ScopedCurl() {
        if (handle) {
            curl_easy_cleanup(handle);
        }
    }
    ScopedCurl(const ScopedCurl&) = delete;
    ScopedCurl& operator=(const ScopedCurl&) = delete;
    [[nodiscard]] CURL* get() const noexcept { return handle; }
};

struct ScopedFile {
    std::FILE* fp = nullptr;
    explicit ScopedFile(std::FILE* f) noexcept : fp(f) {}
    ~ScopedFile() {
        if (fp) {
            std::fclose(fp);
        }
    }
    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;
    [[nodiscard]] std::FILE* get() const noexcept { return fp; }
    void close() noexcept {
        if (fp) {
            std::fclose(fp);
            fp = nullptr;
        }
    }
};

struct DownloadContext {
    const DownloadOptions* options = nullptr;
    DownloadResult* result = nullptr;
    std::FILE* file = nullptr;
    std::string part_filepath;
    bool response_headers_parsed = false;
    bool is_html_detected = false;
    bool range_reset_needed = false;
    long http_status = 0;
};

std::size_t header_callback(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    const std::size_t total = size * nitems;
    auto* ctx = static_cast<DownloadContext*>(userdata);
    std::string_view line(buffer, total);

    // Parse ETag
    constexpr std::string_view kEtagPrefix = "ETag:";
    constexpr std::string_view kEtagPrefixLower = "etag:";
    if (line.size() >= kEtagPrefix.size()) {
        std::string prefix(line.substr(0, kEtagPrefix.size()));
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return std::tolower(c); });
        if (prefix == kEtagPrefixLower) {
            std::string etag(line.substr(kEtagPrefix.size()));
            // Trim whitespace and quotes
            while (!etag.empty() && (std::isspace(static_cast<unsigned char>(etag.front())) || etag.front() == '"')) {
                etag.erase(0, 1);
            }
            while (!etag.empty() && (std::isspace(static_cast<unsigned char>(etag.back())) || etag.back() == '"' || etag.back() == '\r' || etag.back() == '\n')) {
                etag.pop_back();
            }
            ctx->result->etag_received = etag;
        }
    }

    // Parse Content-Type to catch captive portals
    constexpr std::string_view kContentTypePrefix = "content-type:";
    if (line.size() >= kContentTypePrefix.size()) {
        std::string prefix(line.substr(0, kContentTypePrefix.size()));
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return std::tolower(c); });
        if (prefix == kContentTypePrefix) {
            std::string ct(line.substr(kContentTypePrefix.size()));
            std::transform(ct.begin(), ct.end(), ct.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ct.find("text/html") != std::string::npos) {
                ctx->is_html_detected = true;
            }
        }
    }

    return total;
}

std::size_t write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* ctx = static_cast<DownloadContext*>(userdata);

    if (ctx->is_html_detected) {
        // Abort on captive portal HTML page
        return 0;
    }

    if (!ctx->file) {
        return 0;
    }

    std::size_t written = std::fwrite(ptr, size, nmemb, ctx->file);
    if (written < nmemb) {
        // Disk I/O error or ENOSPC
        ctx->result->error_kind = DownloadErrorKind::insufficient_disk_space;
        ctx->result->error_message = "Disk write failed or space exhausted (ENOSPC)";
        return 0;
    }

    ctx->result->bytes_downloaded += (written * size);
    return written * size;
}

int xferinfo_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<DownloadContext*>(clientp);
    if (ctx->options->progress_callback) {
        bool continue_dl = ctx->options->progress_callback(static_cast<std::uint64_t>(dlnow), static_cast<std::uint64_t>(dltotal));
        if (!continue_dl) {
            return 1; // Abort transfer
        }
    }
    return 0;
}

} // namespace

DownloadResult HttpDownloader::download(const DownloadOptions& options) {
    DownloadResult result;

    if (options.url.empty() || options.destination_path.empty()) {
        result.success = false;
        result.error_kind = DownloadErrorKind::http_error;
        result.error_message = "Empty URL or destination path specified.";
        return result;
    }

    const std::filesystem::path dest_path(options.destination_path);
    const std::filesystem::path part_path = options.destination_path + ".part";

    // 1. Pre-check disk space if destination directory exists
    std::error_code ec;
    auto parent_dir = dest_path.parent_path();
    if (!parent_dir.empty()) {
        std::filesystem::create_directories(parent_dir, ec);
        auto space_info = std::filesystem::space(parent_dir, ec);
        if (!ec && options.minimum_expected_bytes > 0 && space_info.available < options.minimum_expected_bytes) {
            result.success = false;
            result.error_kind = DownloadErrorKind::insufficient_disk_space;
            result.error_message = "Available disk space is less than minimum required bytes.";
            return result;
        }
    }

    // 2. Check existing .part file size for Range resume
    std::uint64_t existing_bytes = 0;
    if (std::filesystem::exists(part_path, ec)) {
        existing_bytes = std::filesystem::file_size(part_path, ec);
        if (ec) {
            existing_bytes = 0;
        }
    }

    ScopedCurl curl;
    if (!curl.get()) {
        result.success = false;
        result.error_kind = DownloadErrorKind::network_failure;
        result.error_message = "Failed to initialize libcurl.";
        return result;
    }

    // Open file in append or create mode
    const char* open_mode = (existing_bytes > 0) ? "ab" : "wb";
    std::FILE* raw_file = std::fopen(part_path.c_str(), open_mode);
    if (!raw_file) {
        result.success = false;
        result.error_kind = DownloadErrorKind::insufficient_disk_space;
        result.error_message = "Failed to open temporary .part file for writing.";
        return result;
    }
    ScopedFile file_guard(raw_file);

    DownloadContext ctx;
    ctx.options = &options;
    ctx.result = &result;
    ctx.file = file_guard.get();
    ctx.part_filepath = part_path.string();

    curl_easy_setopt(curl.get(), CURLOPT_URL, options.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, static_cast<long>(options.connect_timeout_seconds));
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, static_cast<long>(options.low_speed_limit_bytes));
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, static_cast<long>(options.low_speed_time_seconds));

    // Range resume header
    if (existing_bytes > 0) {
        curl_easy_setopt(curl.get(), CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(existing_bytes));
    }

    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ctx);

    if (options.progress_callback) {
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &ctx);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    }

    CURLcode res = curl_easy_perform(curl.get());

    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result.http_status_code);
    ctx.http_status = result.http_status_code;

    // Flush dirty pages to physical disk
    if (file_guard.get()) {
        std::fflush(file_guard.get());
        int fd = fileno(file_guard.get());
        if (fd >= 0) {
            ::fsync(fd);
        }
        file_guard.close();
    }

    // Handle Captive portal detection
    if (ctx.is_html_detected) {
        result.success = false;
        result.error_kind = DownloadErrorKind::invalid_content_type;
        result.error_message = "Received HTML response instead of binary ISO. Possible captive portal or web login required.";
        return result;
    }

    // Handle HTTP Status classification
    if (result.http_status_code == 403) {
        result.success = false;
        result.error_kind = DownloadErrorKind::forbidden_or_expired;
        result.error_message = "HTTP 403 Forbidden: Download link expired or IP address mismatched.";
        return result;
    }
    if (result.http_status_code == 429) {
        result.success = false;
        result.error_kind = DownloadErrorKind::rate_limited;
        result.error_message = "HTTP 429 Too Many Requests: Rate limited by Microsoft server.";
        return result;
    }

    if (res != CURLE_OK) {
        result.success = false;
        if (result.error_kind == DownloadErrorKind::none) {
            if (res == CURLE_ABORTED_BY_CALLBACK) {
                result.error_kind = DownloadErrorKind::aborted;
                result.error_message = "Download aborted by callback.";
            } else {
                result.error_kind = DownloadErrorKind::network_failure;
                result.error_message = curl_easy_strerror(res);
            }
        }
        return result;
    }

    if (result.http_status_code >= 400) {
        result.success = false;
        result.error_kind = DownloadErrorKind::http_error;
        result.error_message = "HTTP server error status: " + std::to_string(result.http_status_code);
        return result;
    }

    // 3. ETag validation (Chimera defense)
    if (!options.expected_etag.empty() && !result.etag_received.empty() && options.expected_etag != result.etag_received) {
        result.success = false;
        result.error_kind = DownloadErrorKind::etag_mismatch_reset;
        result.error_message = "Server ETag changed during download. Partial file reset required.";
        std::filesystem::remove(part_path, ec);
        return result;
    }

    // 4. SHA-256 hash verification (Streaming)
    if (!options.expected_sha256.empty()) {
        result.computed_sha256 = compute_file_sha256(part_path.string());
        if (!verify_file_sha256(part_path.string(), options.expected_sha256)) {
            result.success = false;
            result.error_kind = DownloadErrorKind::hash_mismatch;
            result.error_message = "SHA-256 checksum mismatch! Expected: " + options.expected_sha256 +
                                   ", Got: " + result.computed_sha256;
            return result;
        }
    }

    // 5. Atomic promotion: Rename .part to destination
    std::filesystem::rename(part_path, dest_path, ec);
    if (ec) {
        result.success = false;
        result.error_kind = DownloadErrorKind::insufficient_disk_space;
        result.error_message = "Failed to promote .part file to final destination: " + ec.message();
        return result;
    }

    result.success = true;
    result.error_kind = DownloadErrorKind::none;
    return result;
}

} // namespace litewin

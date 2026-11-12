#include "platform/http_downloader.hpp"
#include "core/hash_verifier.hpp"
#include "platform/update_transport.hpp"
#include <fcntl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <curl/curl.h>
#include <filesystem>
#include <memory>
#include <openssl/evp.h>
#include <string_view>
#include <thread>
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
    EVP_MD_CTX* md_ctx = nullptr;
    std::string part_filepath;
    std::uint64_t resume_offset = 0;
    bool response_headers_parsed = false;
    std::string response_url;
    bool is_html_detected = false;
    bool range_reset_needed = false;
    long http_status = 0;
    CURL* curl = nullptr;
    DownloadMeter meter;
    std::chrono::steady_clock::time_point last_report{};
    ScopedFile* file_guard = nullptr;
};

// Callbacks are advisory; never unwind C libcurl callbacks through user code.
void publish_status(const DownloadOptions& options, const DownloadStatus& status) noexcept {
    if (options.status_callback) { try { options.status_callback(status); } catch (...) {} }
}

void update_source(DownloadContext& ctx) {
    char* effective = nullptr;
    curl_easy_getinfo(ctx.curl, CURLINFO_EFFECTIVE_URL, &effective);
    std::string url = effective ? effective : "";
    auto& status = ctx.result->status;
    status.host = sanitize_download_host(url);
    status.origin = classify_download_origin(status.host);
    status.transport = DownloadTransport::Pending;
    if (url.starts_with("file://")) { status.transport = DownloadTransport::Local; status.host.clear(); }
    else if (url.starts_with("http://") && ctx.response_headers_parsed) status.transport = DownloadTransport::Http;
    else if (url.starts_with("https://") && ctx.response_headers_parsed && url == ctx.response_url) {
        long response = 0;
        curl_easy_getinfo(ctx.curl, CURLINFO_RESPONSE_CODE, &response);
        long verified = -1;
        curl_easy_getinfo(ctx.curl, CURLINFO_SSL_VERIFYRESULT, &verified);
        if (verified == 0 && response >= 200) status.transport = DownloadTransport::HttpsVerified;
    }
}

std::size_t header_callback(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    const std::size_t total = size * nitems;
    auto* ctx = static_cast<DownloadContext*>(userdata);
    std::string_view line(buffer, total);

    // Reset per-hop state on new HTTP status line (e.g. after 301/302 redirect)
    if (line.rfind("HTTP/", 0) == 0) {
        ctx->is_html_detected = false;
        ctx->response_headers_parsed = false;
        char* response_url = nullptr;
        curl_easy_getinfo(ctx->curl, CURLINFO_EFFECTIVE_URL, &response_url);
        ctx->response_url = response_url ? response_url : "";
        ctx->range_reset_needed = false;
        ctx->result->etag_received.clear();
        if (ctx->resume_offset > 0 &&
            (line.find(" 200 ") != std::string_view::npos || line.find(" 200\r") != std::string_view::npos)) {
            ctx->range_reset_needed = true;
        }
    }

    if (line == "\r\n" || line == "\n") ctx->response_headers_parsed = true;

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

    if (ctx->range_reset_needed) {
        ctx->range_reset_needed = false;
        if (ctx->file) {
            // freopen may close the old FILE even on failure. Keep the owner synchronized.
            ctx->file = std::freopen(ctx->part_filepath.c_str(), "wb", ctx->file);
            ctx->file_guard->fp = ctx->file;
            ctx->resume_offset = 0;
            ctx->meter.reset(0);
            ctx->result->bytes_downloaded = 0;
        }
        if (ctx->md_ctx) {
            EVP_DigestInit_ex(ctx->md_ctx, EVP_sha256(), nullptr);
        }
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

    if (ctx->md_ctx && written > 0) {
        EVP_DigestUpdate(ctx->md_ctx, ptr, written * size);
    }

    ctx->result->bytes_downloaded += (written * size);
    return written * size;
}

int xferinfo_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<DownloadContext*>(clientp);
    try {
        const auto now = std::chrono::steady_clock::now();
        // Sample once per second: curl may call this hundreds of times per second.
        if (ctx->last_report == std::chrono::steady_clock::time_point{} || now - ctx->last_report >= std::chrono::seconds(1)) {
            auto measured = ctx->meter.sample(static_cast<uint64_t>(std::max<curl_off_t>(0,dlnow)),
                static_cast<uint64_t>(std::max<curl_off_t>(0,dltotal)), now);
            auto& status = ctx->result->status;
            status.downloaded_bytes = measured.downloaded_bytes;
            status.total_bytes = measured.total_bytes;
            status.resumed_bytes = measured.resumed_bytes;
            status.bytes_per_second = measured.bytes_per_second;
            status.eta_seconds = measured.eta_seconds;
            status.phase = dlnow > 0 ? DownloadPhase::Downloading : DownloadPhase::Connecting;
            update_source(*ctx);
            publish_status(*ctx->options, status);
            ctx->last_report = now;
        }
        if (ctx->options->progress_callback && !ctx->options->progress_callback(
            safe_add(ctx->resume_offset, static_cast<uint64_t>(std::max<curl_off_t>(0,dlnow))),
            dltotal > 0 ? safe_add(ctx->resume_offset, static_cast<uint64_t>(dltotal)) : 0)) return 1;
    } catch (...) { return 1; }
    return 0;
}

} // namespace

DownloadResult HttpDownloader::download(const DownloadOptions& options) {
    DownloadResult result;
    result.status.hash = options.expected_sha256.empty() ? DownloadHash::NotProvided : DownloadHash::Pending;
    // Explicit finish helper avoids destructor/NRVO dependence for the returned status.
    auto finish = [&]() -> DownloadResult {
        result.status.phase = result.success ? DownloadPhase::Complete : DownloadPhase::Failed;
        result.status.bytes_per_second = 0;
        result.status.eta_seconds = result.success ? std::optional<double>(0) : std::nullopt;
        if (!result.success && result.error_kind == DownloadErrorKind::network_failure && result.status.transport == DownloadTransport::Pending)
            result.status.transport = DownloadTransport::Failed;
        publish_status(options, result.status);
        return result;
    };
    publish_status(options, result.status);

    if (options.url.empty() || options.destination_path.empty()) {
        result.success = false;
        result.error_kind = DownloadErrorKind::http_error;
        result.error_message = "Empty URL or destination path specified.";
        return finish();
    }

    if(options.strict_official_source) {
        int fd=-1;
        try {
            if(!valid_update_url(options.url,true)||options.expected_sha256.empty()||!options.expected_size)
                throw std::runtime_error("Incomplete signed download target");
            const std::filesystem::path dest(options.destination_path),part=options.destination_path+".part";
            if(!dest.parent_path().empty())std::filesystem::create_directories(dest.parent_path());
            fd=open(part.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
            if(fd<0)throw std::runtime_error("Could not open partial image");
            const auto offset=lseek(fd,0,SEEK_END);
            if(offset<0 || static_cast<std::uint64_t>(offset)>options.expected_size)throw std::runtime_error("Invalid partial image size");
            if(std::filesystem::space(dest.parent_path()).available<options.expected_size-static_cast<std::uint64_t>(offset))
                throw std::runtime_error("Insufficient space for the signed image");
            result.status.resumed_bytes=static_cast<std::uint64_t>(offset);
            result.status.host=sanitize_download_host(options.url);result.status.origin=DownloadOrigin::Official;
            DownloadMeter meter;meter.reset(result.status.resumed_bytes);
            auto last=std::chrono::steady_clock::time_point{};
            std::stop_source stop;
            fetch_official_image({"image",options.url,options.expected_sha256,options.expected_size},fd,stop.get_token(),[&](auto bytes,auto total){
                if(options.progress_callback&&!options.progress_callback(bytes,total)){result.error_kind=DownloadErrorKind::aborted;stop.request_stop();}
                if(bytes<result.status.resumed_bytes){result.status.resumed_bytes=0;meter.reset(0);}
                auto now=std::chrono::steady_clock::now();
                if(now-last<std::chrono::milliseconds(500))return;
                last=now;
                auto sample=meter.sample(bytes>=result.status.resumed_bytes?bytes-result.status.resumed_bytes:0,total>=result.status.resumed_bytes?total-result.status.resumed_bytes:0,now);
                result.status.downloaded_bytes=bytes;result.status.total_bytes=total;
                result.status.bytes_per_second=sample.bytes_per_second;result.status.eta_seconds=sample.eta_seconds;
                result.status.phase=DownloadPhase::Downloading;result.status.transport=DownloadTransport::HttpsVerified;
                publish_status(options,result.status);
            });
            const int closed=close(fd);fd=-1;if(closed!=0)throw std::runtime_error("Could not close the downloaded image safely");
            result.status.transport=static_cast<std::uint64_t>(offset)==options.expected_size?DownloadTransport::Local:DownloadTransport::HttpsVerified;
            result.status.phase=DownloadPhase::Verifying;result.status.hash=DownloadHash::Verifying;publish_status(options,result.status);
            if(!verify_file_sha256(part.string(),options.expected_sha256)){result.error_kind=DownloadErrorKind::hash_mismatch;result.status.hash=DownloadHash::Mismatch;std::filesystem::remove(part);throw std::runtime_error("Signed image checksum mismatch");}
            std::filesystem::rename(part,dest);result.success=true;result.bytes_downloaded=options.expected_size;result.total_expected_bytes=options.expected_size;
            result.status.downloaded_bytes=options.expected_size;result.status.total_bytes=options.expected_size;result.status.hash=DownloadHash::Matched;
        }catch(const std::exception& e){if(fd>=0)close(fd);result.error_message=e.what();if(result.error_kind==DownloadErrorKind::none)result.error_kind=DownloadErrorKind::network_failure;}
        return finish();
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
            return finish();
        }
    }

    // 2. Perform transfer with up to 3 attempts (exponential backoff + Range resume)
    CURLcode res = CURLE_OK;
    bool html_detected = false;
    std::string streamed_sha256;
    constexpr int kMaxAttempts = 3;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1 << (attempt - 1)));
        }

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
            return finish();
        }

        const char* open_mode = (existing_bytes > 0) ? "ab" : "wb";
        std::FILE* raw_file = std::fopen(part_path.c_str(), open_mode);
        if (!raw_file) {
            result.success = false;
            result.error_kind = DownloadErrorKind::insufficient_disk_space;
            result.error_message = "Failed to open temporary .part file for writing.";
            return finish();
        }
        ScopedFile file_guard(raw_file);

        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md_ctx(nullptr, EVP_MD_CTX_free);
        if (existing_bytes == 0 && !options.expected_sha256.empty()) {
            md_ctx.reset(EVP_MD_CTX_new());
            if (md_ctx) {
                EVP_DigestInit_ex(md_ctx.get(), EVP_sha256(), nullptr);
            }
        }

        DownloadContext ctx;
        ctx.options = &options;
        ctx.result = &result;
        ctx.file = file_guard.get();
        ctx.md_ctx = md_ctx.get();
        ctx.part_filepath = part_path.string();
        ctx.resume_offset = existing_bytes;
        ctx.curl = curl.get();
        ctx.file_guard = &file_guard;
        ctx.meter.reset(existing_bytes);
        result.bytes_downloaded = existing_bytes;
        result.error_kind = DownloadErrorKind::none;
        result.etag_received.clear();
        result.status.phase = DownloadPhase::Connecting;
        result.status.transport = DownloadTransport::Pending;
        result.status.hash = options.expected_sha256.empty() ? DownloadHash::NotProvided : DownloadHash::Pending;
        result.status.downloaded_bytes = existing_bytes;
        result.status.total_bytes = 0;
        result.status.resumed_bytes = existing_bytes;
        result.status.bytes_per_second = 0;
        result.status.eta_seconds.reset();
        result.status.host.clear();
        result.status.origin = DownloadOrigin::Unknown;
        publish_status(options, result.status);

        curl_easy_setopt(curl.get(), CURLOPT_URL, options.url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https,file");
        curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https,file");
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, static_cast<long>(options.connect_timeout_seconds));
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, static_cast<long>(options.low_speed_limit_bytes));
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, static_cast<long>(options.low_speed_time_seconds));

        if (existing_bytes > 0) {
            curl_easy_setopt(curl.get(), CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(existing_bytes));
        }

        curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &ctx);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ctx);

        if (options.progress_callback || options.status_callback) {
            curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
            curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &ctx);
            curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        }

        res = curl_easy_perform(curl.get());
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result.http_status_code);
        ctx.http_status = result.http_status_code;
        update_source(ctx);
        result.status.downloaded_bytes = result.bytes_downloaded;
        result.status.resumed_bytes = ctx.resume_offset;
        curl_off_t content_length = -1;
        curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
        result.status.total_bytes = content_length >= 0 ? safe_add(ctx.resume_offset,static_cast<uint64_t>(content_length)) : 0;
        result.total_expected_bytes = result.status.total_bytes;
        if (res == CURLE_PEER_FAILED_VERIFICATION || res == CURLE_SSL_CONNECT_ERROR)
            result.status.transport = DownloadTransport::Failed;
        html_detected = ctx.is_html_detected;

        if (res == CURLE_OK && md_ctx) {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int digest_len = 0;
            if (EVP_DigestFinal_ex(md_ctx.get(), digest, &digest_len) == 1) {
                static const char hex_chars[] = "0123456789abcdef";
                streamed_sha256.clear();
                streamed_sha256.reserve(digest_len * 2);
                for (unsigned int i = 0; i < digest_len; ++i) {
                    streamed_sha256.push_back(hex_chars[(digest[i] >> 4) & 0x0F]);
                    streamed_sha256.push_back(hex_chars[digest[i] & 0x0F]);
                }
            }
        }

        if (file_guard.get()) {
            if (std::fflush(file_guard.get()) != 0) {
                result.error_kind = DownloadErrorKind::insufficient_disk_space;
                result.error_message = "Failed to flush downloaded file.";
                res = CURLE_WRITE_ERROR;
            }
            int fd = fileno(file_guard.get());
            if (fd >= 0) {
                if (::fsync(fd) != 0) {
                    result.error_kind = DownloadErrorKind::insufficient_disk_space;
                    result.error_message = "Failed to persist downloaded file.";
                    res = CURLE_WRITE_ERROR;
                }
            }
            file_guard.close();
        }

        if (res == CURLE_OK || res == CURLE_ABORTED_BY_CALLBACK || html_detected ||
            result.error_kind == DownloadErrorKind::insufficient_disk_space ||
            result.http_status_code == 403 || result.http_status_code == 429) {
            break;
        }
    }

    // Handle Captive portal detection
    if (html_detected) {
        result.success = false;
        result.error_kind = DownloadErrorKind::invalid_content_type;
        result.error_message = "Received HTML response instead of binary ISO. Possible captive portal or web login required.";
        return finish();
    }

    // Handle HTTP Status classification
    if (result.http_status_code == 403) {
        result.success = false;
        result.error_kind = DownloadErrorKind::forbidden_or_expired;
        result.error_message = "HTTP 403 Forbidden: Download link expired or IP address mismatched.";
        return finish();
    }
    if (result.http_status_code == 429) {
        result.success = false;
        result.error_kind = DownloadErrorKind::rate_limited;
        result.error_message = "HTTP 429 Too Many Requests: Rate limited by Microsoft server.";
        return finish();
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
        return finish();
    }

    if (result.http_status_code >= 400) {
        result.success = false;
        result.error_kind = DownloadErrorKind::http_error;
        result.error_message = "HTTP server error status: " + std::to_string(result.http_status_code);
        return finish();
    }

    // 3. ETag validation (Chimera defense)
    if (!options.expected_etag.empty() && !result.etag_received.empty() && options.expected_etag != result.etag_received) {
        result.success = false;
        result.error_kind = DownloadErrorKind::etag_mismatch_reset;
        result.error_message = "Server ETag changed during download. Partial file reset required.";
        std::filesystem::remove(part_path, ec);
        return finish();
    }

    // 4. SHA-256 hash verification (Zero-reread in-flight streaming or single-pass fallback)
    if (!options.expected_sha256.empty()) {
        result.status.phase = DownloadPhase::Verifying;
        result.status.hash = DownloadHash::Verifying;
        result.status.bytes_per_second = 0;
        result.status.eta_seconds.reset();
        publish_status(options, result.status);
        result.computed_sha256 = !streamed_sha256.empty()
            ? streamed_sha256
            : compute_file_sha256(part_path.string());
        std::string expected_lower = options.expected_sha256;
        std::transform(expected_lower.begin(), expected_lower.end(), expected_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (result.computed_sha256.empty() || result.computed_sha256 != expected_lower) {
            result.success = false;
            result.error_kind = DownloadErrorKind::hash_mismatch;
            result.status.hash = DownloadHash::Mismatch;
            result.error_message = "SHA-256 checksum mismatch! Expected: " + options.expected_sha256 +
                                   ", Got: " + result.computed_sha256;
            std::filesystem::remove(part_path, ec);
            return finish();
        }
    }

    if (!options.expected_sha256.empty()) result.status.hash = DownloadHash::Matched;

    // 5. Atomic promotion: Rename .part to destination
    std::filesystem::rename(part_path, dest_path, ec);
    if (ec) {
        result.success = false;
        result.error_kind = DownloadErrorKind::insufficient_disk_space;
        result.error_message = "Failed to promote .part file to final destination: " + ec.message();
        return finish();
    }

    result.success = true;
    result.error_kind = DownloadErrorKind::none;
    return finish();
}

} // namespace litewin

#pragma once

#include "core/download_types.hpp"

namespace litewin {

class HttpDownloader {
public:
    // Downloads a file using libcurl with resumable Range support,
    // ETag validation, disk pre-check, and atomic promotion upon hash verification.
    [[nodiscard]] static DownloadResult download(const DownloadOptions& options);
};

} // namespace litewin

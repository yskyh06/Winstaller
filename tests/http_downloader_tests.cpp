#include "core/download_types.hpp"
#include "core/hash_verifier.hpp"
#include "platform/http_downloader.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

void test_sha256_streaming_known() {
    const std::string tmp_file = "/tmp/litewin_sha256_test.txt";
    const std::string content = "The quick brown fox jumps over the lazy dog";
    const std::string expected_sha256 = "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592";

    {
        std::ofstream ofs(tmp_file, std::ios::binary);
        ofs << content;
    }

    std::string actual = litewin::compute_file_sha256(tmp_file);
    assert(actual == expected_sha256);
    assert(litewin::verify_file_sha256(tmp_file, expected_sha256));

    std::filesystem::remove(tmp_file);
}

void test_sha256_corrupted_byte() {
    const std::string tmp_file = "/tmp/litewin_sha256_corrupt.txt";
    const std::string content = "The quick brown fox jumps over the lazy dog";
    const std::string expected_sha256 = "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592";

    {
        std::ofstream ofs(tmp_file, std::ios::binary);
        ofs << "The quick brown fox jumps over the lazy cog"; // 1 byte corrupted ('d' -> 'c')
    }

    assert(!litewin::verify_file_sha256(tmp_file, expected_sha256));
    std::filesystem::remove(tmp_file);
}

void test_session_cache_validity() {
    litewin::DownloadSessionCache cache{
        .url = "https://software.download.prss.microsoft.com/dbazure/Win11_24H2.iso?t=token",
        .etag = "abc123etag",
        .last_modified = "Sun, 01 Nov 2026 00:00:00 GMT",
        .content_length = 5ULL * 1024 * 1024 * 1024,
        .created_at_epoch_sec = 1000000,
        .expires_at_epoch_sec = 1000000 + (24 * 3600), // 24 hours later
    };

    // Right after creation (0 hours elapsed) -> Valid
    assert(litewin::is_download_session_valid(cache, 1000000));

    // 18 hours elapsed -> Valid (within 20-hour safe window)
    assert(litewin::is_download_session_valid(cache, 1000000 + (18 * 3600)));

    // 21 hours elapsed (less than 4 hours left before 24h expiration) -> Invalid (safety buffer rejects)
    assert(!litewin::is_download_session_valid(cache, 1000000 + (21 * 3600)));

    // 25 hours elapsed (already expired) -> Invalid
    assert(!litewin::is_download_session_valid(cache, 1000000 + (25 * 3600)));
}

void test_download_precheck_disk_space() {
    litewin::DownloadOptions opts{
        .url = "file:///tmp/dummy.iso",
        .destination_path = "/tmp/litewin_dummy.iso",
        .expected_sha256 = "",
        .minimum_expected_bytes = 10000ULL * 1024 * 1024 * 1024 * 1024, // 10,000 TB (Impossible space)
        .connect_timeout_seconds = 15,
        .low_speed_limit_bytes = 10240,
        .low_speed_time_seconds = 30,
        .expected_etag = "",
        .progress_callback = nullptr,
    };

    auto res = litewin::HttpDownloader::download(opts);
    assert(!res.success);
    assert(res.error_kind == litewin::DownloadErrorKind::insufficient_disk_space);
}

void test_download_local_file_with_hash() {
    const std::string src_file = "/tmp/litewin_test_source.bin";
    const std::string dst_file = "/tmp/litewin_test_dest.bin";
    const std::string part_file = dst_file + ".part";
    const std::string content = "Testing libcurl atomic download and sha256 verification.";

    {
        std::ofstream ofs(src_file, std::ios::binary);
        ofs << content;
    }

    std::string expected_hash = litewin::compute_file_sha256(src_file);
    assert(!expected_hash.empty());

    litewin::DownloadOptions opts{
        .url = "file://" + src_file,
        .destination_path = dst_file,
        .expected_sha256 = expected_hash,
        .minimum_expected_bytes = 10, // Small floor for local test
        .connect_timeout_seconds = 15,
        .low_speed_limit_bytes = 10240,
        .low_speed_time_seconds = 30,
        .expected_etag = "",
        .progress_callback = nullptr,
    };

    auto res = litewin::HttpDownloader::download(opts);
    assert(res.success);
    assert(res.error_kind == litewin::DownloadErrorKind::none);

    // .part must be promoted to destination file
    assert(!std::filesystem::exists(part_file));
    assert(std::filesystem::exists(dst_file));
    assert(std::filesystem::file_size(dst_file) == content.size());
    assert(litewin::verify_file_sha256(dst_file, expected_hash));

    std::filesystem::remove(src_file);
    std::filesystem::remove(dst_file);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: http_downloader_tests <scenario>\n";
        return 1;
    }

    const std::string_view scenario = argv[1];
    if (scenario == "sha256_streaming_known") {
        test_sha256_streaming_known();
    } else if (scenario == "sha256_corrupted_byte") {
        test_sha256_corrupted_byte();
    } else if (scenario == "session_cache_validity") {
        test_session_cache_validity();
    } else if (scenario == "download_precheck_disk_space") {
        test_download_precheck_disk_space();
    } else if (scenario == "download_local_file_with_hash") {
        test_download_local_file_with_hash();
    } else {
        std::cerr << "Unknown scenario: " << scenario << "\n";
        return 1;
    }

    return 0;
}

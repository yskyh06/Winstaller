#include "platform/mido_resolver.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

void test_cache_save_and_load() {
    const std::string tmp_cache = "/tmp/litewin_test_cache.json";
    litewin::DownloadSessionCache cache{
        .url = "https://software.download.prss.microsoft.com/test/win11.iso",
        .etag = "etag-12345",
        .last_modified = "Sun, 01 Nov 2026 00:00:00 GMT",
        .content_length = 6000000000ULL,
        .created_at_epoch_sec = 1700000000,
        .expires_at_epoch_sec = 1700086400,
    };

    assert(litewin::save_download_session_cache(tmp_cache, cache));
    auto loaded = litewin::load_download_session_cache(tmp_cache);
    assert(loaded.has_value());
    assert(loaded->url == cache.url);
    assert(loaded->etag == cache.etag);
    assert(loaded->content_length == cache.content_length);
    assert(loaded->expires_at_epoch_sec == cache.expires_at_epoch_sec);

    std::filesystem::remove(tmp_cache);
}

void test_cache_hit_prevents_script_execution() {
    const std::string tmp_cache = "/tmp/litewin_test_hit_cache.json";
    std::int64_t now = 1700000000;
    litewin::DownloadSessionCache cache{
        .url = "https://software.download.prss.microsoft.com/cached/win11.iso",
        .etag = "",
        .last_modified = "",
        .content_length = 0,
        .created_at_epoch_sec = now,
        .expires_at_epoch_sec = now + (24 * 3600),
    };
    litewin::save_download_session_cache(tmp_cache, cache);

    // Provide a non-existent fake script path to prove the script is NEVER called on cache hit
    litewin::MidoResolverOptions opts{
        .cache_filepath = tmp_cache,
        .mido_script_path = "/non_existent_path/fake_mido.sh",
        .safe_buffer_seconds = 4 * 3600,
    };

    auto res = litewin::MidoResolver::resolve(opts, now + (2 * 3600)); // 2 hours elapsed
    assert(res.success);
    assert(res.from_cache);
    assert(res.url == cache.url);

    std::filesystem::remove(tmp_cache);
}

void test_cache_expired_triggers_script() {
    const std::string tmp_cache = "/tmp/litewin_test_exp_cache.json";
    std::int64_t now = 1700000000;
    litewin::DownloadSessionCache cache{
        .url = "https://software.download.prss.microsoft.com/expired/win11.iso",
        .etag = "",
        .last_modified = "",
        .content_length = 0,
        .created_at_epoch_sec = now,
        .expires_at_epoch_sec = now + (24 * 3600),
    };
    litewin::save_download_session_cache(tmp_cache, cache);

    // Provide non-existent script; because cache is expired (22 hours elapsed, inside 4h buffer),
    // it will attempt to execute script and fail with "script not found" instead of returning stale cache.
    litewin::MidoResolverOptions opts{
        .cache_filepath = tmp_cache,
        .mido_script_path = "/non_existent_path/fake_mido.sh",
        .safe_buffer_seconds = 4 * 3600,
    };

    auto res = litewin::MidoResolver::resolve(opts, now + (22 * 3600));
    assert(!res.success);
    assert(!res.from_cache);
    assert(res.error_message.find("not found") != std::string::npos);

    std::filesystem::remove(tmp_cache);
}

void test_mock_script_resolution() {
    const std::string tmp_cache = "/tmp/litewin_test_mock_cache.json";
    const std::string mock_script = "/tmp/mock_mido.sh";

    {
        std::ofstream ofs(mock_script);
        ofs << "#!/bin/bash\n";
        ofs << "echo '[Mido] Fetching SKU...'\n";
        ofs << "echo 'https://software.download.prss.microsoft.com/dbazure/Win11_24H2_Korean_x64.iso?t=token123'\n";
    }
    std::filesystem::permissions(mock_script, std::filesystem::perms::owner_all);

    litewin::MidoResolverOptions opts{
        .cache_filepath = tmp_cache,
        .mido_script_path = mock_script,
    };

    std::filesystem::remove(tmp_cache); // Ensure fresh
    auto res = litewin::MidoResolver::resolve(opts, 1700000000);
    assert(res.success);
    assert(!res.from_cache);
    assert(res.url.find("Win11_24H2_Korean_x64.iso") != std::string::npos);

    // Verify cache file was automatically created
    assert(std::filesystem::exists(tmp_cache));
    auto cached = litewin::load_download_session_cache(tmp_cache);
    assert(cached.has_value());
    assert(cached->url == res.url);

    std::filesystem::remove(tmp_cache);
    std::filesystem::remove(mock_script);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: mido_resolver_tests <scenario>\n";
        return 1;
    }

    const std::string_view scenario = argv[1];
    if (scenario == "cache_save_and_load") {
        test_cache_save_and_load();
    } else if (scenario == "cache_hit_prevents_script_execution") {
        test_cache_hit_prevents_script_execution();
    } else if (scenario == "cache_expired_triggers_script") {
        test_cache_expired_triggers_script();
    } else if (scenario == "mock_script_resolution") {
        test_mock_script_resolution();
    } else {
        std::cerr << "Unknown scenario: " << scenario << "\n";
        return 1;
    }

    return 0;
}

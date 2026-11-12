#if __has_include("../src/core/download_status.hpp")
#include "../src/core/download_status.hpp"
#elif __has_include("core/download_status.hpp")
#include "core/download_status.hpp"
#else
#include "download_status.hpp"
#endif

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#define REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            throw std::runtime_error(std::string("Assertion failed: ") + #expr + \
                                     " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (false)

void test_known_eta() {
    litewin::DownloadMeter meter;
    auto t0 = std::chrono::steady_clock::time_point{};
    meter.sample(0, 10000, t0);

    auto t1 = t0 + std::chrono::seconds(2);
    auto status = meter.sample(4000, 10000, t1);

    REQUIRE(status.downloaded_bytes == 4000);
    REQUIRE(status.total_bytes == 10000);
    REQUIRE(std::abs(status.bytes_per_second - 2000.0) < 1.0);
    REQUIRE(status.eta_seconds.has_value());
    REQUIRE(std::abs(*status.eta_seconds - 3.0) < 0.1);
}

void test_resume_no_speedspike() {
    litewin::DownloadMeter meter(100'000'000);
    auto t0 = std::chrono::steady_clock::time_point{};
    meter.sample(0, 50'000, t0);

    auto t1 = t0 + std::chrono::seconds(1);
    auto status = meter.sample(10'000, 50'000, t1);

    REQUIRE(status.resumed_bytes == 100'000'000);
    REQUIRE(status.downloaded_bytes == 100'010'000);
    REQUIRE(status.total_bytes == 100'050'000);
    REQUIRE(std::abs(status.bytes_per_second - 10000.0) < 1.0);
    REQUIRE(status.eta_seconds.has_value());
    REQUIRE(std::abs(*status.eta_seconds - 4.0) < 0.1);
}

void test_unknown_size() {
    litewin::DownloadMeter meter;
    auto t0 = std::chrono::steady_clock::time_point{};
    meter.sample(0, 0, t0);

    auto t1 = t0 + std::chrono::seconds(2);
    auto status = meter.sample(4000, 0, t1);

    REQUIRE(status.total_bytes == 0);
    REQUIRE(status.bytes_per_second > 0.0);
    REQUIRE(!status.eta_seconds.has_value());
}

void test_reset_decreased_counter() {
    litewin::DownloadMeter meter;
    auto t0 = std::chrono::steady_clock::time_point{};
    meter.sample(0, 10000, t0);

    auto t1 = t0 + std::chrono::seconds(2);
    meter.sample(4000, 10000, t1);

    auto t2 = t1 + std::chrono::seconds(1);
    auto status_reset = meter.sample(0, 10000, t2);
    REQUIRE(status_reset.downloaded_bytes == 0);
    REQUIRE(!status_reset.eta_seconds.has_value());

    auto t3 = t2 + std::chrono::seconds(2);
    auto status_resumed = meter.sample(2000, 10000, t3);
    REQUIRE(std::abs(status_resumed.bytes_per_second - 1000.0) < 1.0);
}

void test_stall() {
    litewin::DownloadMeter meter;
    auto t0 = std::chrono::steady_clock::time_point{};
    meter.sample(0, 10000, t0);

    auto t1 = t0 + std::chrono::seconds(1);
    auto status1 = meter.sample(2000, 10000, t1);
    REQUIRE(status1.bytes_per_second > 0.0);

    auto t2 = t1 + std::chrono::seconds(3);
    auto status2 = meter.sample(2000, 10000, t2);
    REQUIRE(status2.bytes_per_second == 0.0);
    REQUIRE(!status2.eta_seconds.has_value());
}

void test_credentials_query_redaction() {
    std::string url = "https://user:secret_token@releases.ubuntu.com:8443/24.04/iso?token=xyz#hash";
    std::string host = litewin::sanitize_download_host(url);
    REQUIRE(host == "releases.ubuntu.com");
    REQUIRE(host.find("secret_token") == std::string::npos);
    REQUIRE(host.find("8443") == std::string::npos);
    REQUIRE(host.find("xyz") == std::string::npos);
}

void test_spoofhost_rejection() {
    using litewin::DownloadOrigin;
    using litewin::classify_download_origin;
    using litewin::sanitize_download_host;

    REQUIRE(classify_download_origin("releases.ubuntu.com") == DownloadOrigin::Official);
    REQUIRE(classify_download_origin("cdimage.ubuntu.com") == DownloadOrigin::Official);
    REQUIRE(classify_download_origin("archive.ubuntu.com") == DownloadOrigin::Official);
    REQUIRE(classify_download_origin("security.ubuntu.com") == DownloadOrigin::Official);
    REQUIRE(classify_download_origin("download.windowsupdate.com") == DownloadOrigin::Official);
    REQUIRE(classify_download_origin("sub.download.windowsupdate.com") == DownloadOrigin::Official);
    REQUIRE(classify_download_origin("a.b.download.windowsupdate.com") == DownloadOrigin::Official);
    REQUIRE(classify_download_origin("dl.delivery.mp.microsoft.com") == DownloadOrigin::Official);

    REQUIRE(classify_download_origin("releases.ubuntu.com.attacker.com") == DownloadOrigin::Unknown);
    REQUIRE(classify_download_origin("fake-download.windowsupdate.com") == DownloadOrigin::Unknown);
    REQUIRE(classify_download_origin("notdownload.windowsupdate.com") == DownloadOrigin::Unknown);
    REQUIRE(classify_download_origin("download.windowsupdate.com.attacker.org") == DownloadOrigin::Unknown);
    REQUIRE(classify_download_origin("mirror.math.princeton.edu") == DownloadOrigin::Unknown);
    REQUIRE(classify_download_origin("") == DownloadOrigin::Unknown);

    REQUIRE(sanitize_download_host("http://releases.ubuntu.com\r\ninjection.com") == "");
    REQUIRE(sanitize_download_host("http://user@releases.ubuntu.com@evil.com") == "evil.com");
}

void test_overflow_safety() {
    litewin::DownloadMeter meter(std::numeric_limits<uint64_t>::max() - 10);
    auto t0 = std::chrono::steady_clock::time_point{};
    auto status = meter.sample(20, 100, t0);
    REQUIRE(status.downloaded_bytes == std::numeric_limits<uint64_t>::max());
}

int main() {
    test_known_eta();
    test_resume_no_speedspike();
    test_unknown_size();
    test_reset_decreased_counter();
    test_stall();
    test_credentials_query_redaction();
    test_spoofhost_rejection();
    test_overflow_safety();
    std::cout << "All download_status tests passed successfully.\n";
    return 0;
}

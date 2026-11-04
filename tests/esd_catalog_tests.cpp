#include "core/esd_catalog.hpp"

#include <cassert>
#include <iostream>

void test_default_target_is_26h2_korean_x64() {
    auto target = litewin::get_default_esd_target();
    assert(target.version == litewin::WindowsVersion::Win11_26H2);
    assert(target.language_code == "ko-kr");
    assert(target.architecture == "x64");
    assert(!target.download_url.empty());
    assert(target.size_bytes > 4000000000ULL);
    assert(!target.encrypted);
    std::cout << "[PASS] test_default_target_is_26h2_korean_x64\n";
}

void test_version_parsing_and_to_string() {
    assert(litewin::to_string(litewin::WindowsVersion::Win11_26H2) == "Windows 11 26H2 (Recommended)");
    assert(litewin::parse_windows_version("26h2") == litewin::WindowsVersion::Win11_26H2);
    assert(litewin::parse_windows_version("win11_24h2") == litewin::WindowsVersion::Win11_24H2);
    assert(litewin::parse_windows_version("win10_22h2") == litewin::WindowsVersion::Win10_22H2);
    assert(!litewin::parse_windows_version("invalid_os").has_value());
    std::cout << "[PASS] test_version_parsing_and_to_string\n";
}

void test_find_targets() {
    // Korean 24H2
    auto k24 = litewin::find_esd_target(litewin::WindowsVersion::Win11_24H2, "ko-kr", "x64");
    assert(k24.has_value());
    assert(k24->file_name.find("26100") != std::string::npos);
    assert(k24->download_url.find("dl.delivery.mp.microsoft.com") != std::string::npos);

    // Korean Win10 22H2
    auto k10 = litewin::find_esd_target(litewin::WindowsVersion::Win10_22H2, "ko-kr", "x64");
    assert(k10.has_value());
    assert(k10->file_name.find("19045") != std::string::npos);

    // Case-insensitive TUI language lookup (ko-KR / en-US)
    auto en26 = litewin::find_esd_target(litewin::WindowsVersion::Win11_26H2, "en-US", "X64");
    assert(en26.has_value());
    assert(en26->language_code == "en-us");
    assert(en26->download_url.rfind("http://", 0) == 0);

    // Non-existent target
    auto missing = litewin::find_esd_target(litewin::WindowsVersion::Win11_26H2, "fr-fr", "x86");
    assert(!missing.has_value());
    std::cout << "[PASS] test_find_targets\n";
}

void test_architectures_per_version() {
    auto w11_archs = litewin::get_supported_architectures(litewin::WindowsVersion::Win11_26H2);
    assert(w11_archs.size() == 2);
    assert(w11_archs[0] == "x64");
    assert(w11_archs[1] == "arm64");

    auto w10_archs = litewin::get_supported_architectures(litewin::WindowsVersion::Win10_22H2);
    assert(w10_archs.size() == 3);
    assert(w10_archs[2] == "x86");
    std::cout << "[PASS] test_architectures_per_version\n";
}

int main() {
    test_default_target_is_26h2_korean_x64();
    test_version_parsing_and_to_string();
    test_find_targets();
    test_architectures_per_version();
    std::cout << "All esd_catalog tests passed!\n";
    return 0;
}

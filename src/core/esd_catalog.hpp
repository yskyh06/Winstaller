#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litewin {

enum class WindowsVersion {
    Win11_26H2, // Default / Recommended
    Win11_25H2,
    Win11_24H2,
    Win10_22H2,
};

[[nodiscard]] std::string_view to_string(WindowsVersion ver);
[[nodiscard]] std::optional<WindowsVersion> parse_windows_version(std::string_view str);

struct EsdTarget {
    WindowsVersion version = WindowsVersion::Win11_26H2;
    std::string version_name;
    std::string language_code;
    std::string language_name;
    std::string architecture;
    std::string file_name;
    std::string download_url;
    std::uint64_t size_bytes = 0;
    std::string sha1_hash;
    std::string sha256_hash;
    bool encrypted = false;
};

// Returns default target: Win11 26H2, Korean (ko-kr), x64
[[nodiscard]] EsdTarget get_default_esd_target();

// Look up target by version, language code, and architecture
[[nodiscard]] std::optional<EsdTarget> find_esd_target(
    WindowsVersion version,
    std::string_view language_code,
    std::string_view architecture
);

// Returns supported versions in priority order (26H2 first)
[[nodiscard]] std::vector<WindowsVersion> get_supported_versions();

// Returns supported architectures for a version (Win11: x64, arm64; Win10: x64, arm64, x86)
[[nodiscard]] std::vector<std::string> get_supported_architectures(WindowsVersion version);

// Returns supported languages for a version & architecture
[[nodiscard]] std::vector<std::string> get_supported_languages(
    WindowsVersion version,
    std::string_view architecture
);

} // namespace litewin

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litewin {

enum class OsTabCategory {
    Windows,
    Ubuntu,
    Debian,
    RhelFedora,
    Arch
};

enum class LinuxDistroFamily {
    Debian,
    RedHat,
    Arch
};

enum class LinuxExtractorMode {
    SingleSquashFS,
    LayeredSquashFS,
    Erofs
};

enum class LinuxEnvironment {
    DesktopGUI,
    ServerCLI
};

struct LinuxTarget {
    std::string id;
    std::string display_name;
    OsTabCategory tab_category = OsTabCategory::Ubuntu;
    LinuxDistroFamily family = LinuxDistroFamily::Debian;
    LinuxExtractorMode extractor_mode = LinuxExtractorMode::SingleSquashFS;
    LinuxEnvironment environment = LinuxEnvironment::DesktopGUI;
    std::string version;
    bool is_expired = false;
    std::vector<std::string> image_layers;
    std::string download_url;
    std::string sha256_hash;
    std::uint64_t size_bytes = 0;
    std::string kernel_path;
};

[[nodiscard]] const std::vector<LinuxTarget>& get_all_linux_targets();
[[nodiscard]] std::vector<LinuxTarget> get_linux_targets_by_tab(OsTabCategory tab);
[[nodiscard]] std::optional<LinuxTarget> find_linux_target_by_id(std::string_view id);

[[nodiscard]] std::string_view to_string(OsTabCategory tab);
[[nodiscard]] std::string_view to_string(LinuxDistroFamily family);
[[nodiscard]] std::string_view to_string(LinuxExtractorMode mode);
[[nodiscard]] std::string_view to_string(LinuxEnvironment env);

} // namespace litewin

#pragma once

#include "platform/process_runner.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace litewin {

struct WimImageInfo {
    int index = 1;
    std::string name;
    std::string description;
    std::string architecture;
    std::string installation_type;
    std::uint64_t total_bytes = 0;
};

struct WimDeployOptions {
    std::string image_path;
    int image_index = 1;
    std::string target_dir;
    bool verify_integrity = true;
    std::string wimlib_tool = "wimlib-imagex";
    std::function<void(int percent, const std::string& status)> progress_callback = nullptr;
};

struct WimDeployResult {
    bool success = false;
    int exit_code = -1;
    std::string error_message;
    std::string stdout_output;
    std::string stderr_output;
};

class WimDeployer {
public:
    // Parses image index list from wimlib-imagex info stdout
    [[nodiscard]] static std::vector<WimImageInfo> parse_info_output(const std::string& info_stdout);

    // Signed catalog selections never fall back to a different edition or architecture.
    [[nodiscard]] static std::optional<WimImageInfo> resolve_exact_image(const std::vector<WimImageInfo>& images,const std::string& name);

    // Queries image info from an ESD/WIM file
    [[nodiscard]] static std::vector<WimImageInfo> get_image_info(
        const std::string& image_path,
        const std::string& wimlib_tool = "wimlib-imagex"
    );

    // Applies the ESD/WIM image to target directory
    [[nodiscard]] static WimDeployResult apply_image(
        const WimDeployOptions& options
    );
};

} // namespace litewin

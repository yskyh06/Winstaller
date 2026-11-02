#pragma once

#include <string>

namespace litewin {

enum class NetworkConfigUiMode {
    auto_detect,
    tui_only,
    gui_only,
};

struct NetworkConfiguratorResult {
    bool executed = false;
    int exit_code = 0;
    std::string tool_used;
    std::string error_message;
};

[[nodiscard]] NetworkConfiguratorResult open_network_settings(
    NetworkConfigUiMode mode = NetworkConfigUiMode::auto_detect
);

} // namespace litewin

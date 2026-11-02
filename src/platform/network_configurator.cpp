#include "platform/network_configurator.hpp"

#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace litewin {

namespace {

bool command_exists(const char* cmd) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        path_env = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";
    }

    std::string path_str = path_env;
    std::size_t start = 0;
    while (start < path_str.size()) {
        std::size_t end = path_str.find(':', start);
        if (end == std::string::npos) {
            end = path_str.size();
        }
        std::string dir = path_str.substr(start, end - start);
        if (!dir.empty()) {
            std::string full_path = dir + "/" + cmd;
            if (::access(full_path.c_str(), X_OK) == 0) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

NetworkConfiguratorResult execute_tool(const char* tool_path, char* const argv[]) {
    NetworkConfiguratorResult result;
    result.tool_used = tool_path;

    pid_t pid = ::fork();
    if (pid < 0) {
        result.executed = false;
        result.error_message = std::strerror(errno);
        return result;
    }

    if (pid == 0) {
        // Child process
        ::setenv("TERM", "linux", 0);
        ::execvp(tool_path, argv);
        ::_exit(127);
    }

    // Parent process: wait for completion
    int status = 0;
    pid_t waited = ::waitpid(pid, &status, 0);
    if (waited < 0) {
        result.executed = false;
        result.error_message = std::strerror(errno);
        return result;
    }

    result.executed = true;
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        if (result.exit_code == 127) {
            result.executed = false;
            result.error_message = "Failed to execute " + std::string(tool_path);
        }
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
        result.error_message = "Process terminated by signal";
    }

    return result;
}

} // namespace

NetworkConfiguratorResult open_network_settings(NetworkConfigUiMode mode) {
    NetworkConfiguratorResult result;

    bool has_gui_session = (std::getenv("DISPLAY") != nullptr) || (std::getenv("WAYLAND_DISPLAY") != nullptr);

    bool try_gui = false;
    if (mode == NetworkConfigUiMode::gui_only) {
        try_gui = true;
    } else if (mode == NetworkConfigUiMode::auto_detect && has_gui_session) {
        try_gui = true;
    }

    if (try_gui && command_exists("nm-connection-editor")) {
        char cmd[] = "nm-connection-editor";
        char* const argv[] = {cmd, nullptr};
        return execute_tool("nm-connection-editor", argv);
    }

    if (mode == NetworkConfigUiMode::gui_only) {
        result.executed = false;
        result.error_message = "GUI mode requested, but nm-connection-editor not found or no GUI session active.";
        return result;
    }

    // Fallback or explicit TUI
    if (command_exists("nmtui")) {
        char cmd[] = "nmtui";
        char* const argv[] = {cmd, nullptr};
        return execute_tool("nmtui", argv);
    }

    if (command_exists("iwctl")) {
        char cmd[] = "iwctl";
        char* const argv[] = {cmd, nullptr};
        return execute_tool("iwctl", argv);
    }

    result.executed = false;
    result.error_message = "Network configuration tool not found (nmtui/iwctl not installed).";
    return result;
}

} // namespace litewin

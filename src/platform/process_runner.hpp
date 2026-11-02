#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace litewin {

struct ProcessResult {
    bool executed = false;
    int exit_code = 0;
    std::string stdout_output;
    std::string stderr_output;
    std::string error_message;

    [[nodiscard]] bool success() const noexcept {
        return executed && exit_code == 0;
    }
};

class ProcessRunner {
public:
    // Execute a command with arguments and optional stdin string.
    // stdin is a local stream socket; output is drained concurrently with input.
    // An early stdin close returns an error without changing SIGPIPE handlers.
    [[nodiscard]] static ProcessResult run(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        const std::string& stdin_input = ""
    );

    // Execute a command and stream stdout/stderr chunks in real time via callback.
    [[nodiscard]] static ProcessResult run_streaming(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        const std::function<void(std::string_view)>& on_output_chunk,
        const std::string& stdin_input = ""
    );
};

} // namespace litewin

#include "platform/wim_deployer.hpp"

#include <filesystem>
#include <regex>
#include <sstream>

namespace litewin {

std::vector<WimImageInfo> WimDeployer::parse_info_output(const std::string& info_stdout) {
    std::vector<WimImageInfo> results;
    std::istringstream iss(info_stdout);
    std::string line;

    WimImageInfo current;
    bool in_image = false;

    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
            s.pop_back();
        }
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
            ++start;
        }
        if (start > 0) {
            s = s.substr(start);
        }
    };

    while (std::getline(iss, line)) {
        trim(line);
        if (line.rfind("Index:", 0) == 0) {
            if (in_image) {
                results.push_back(current);
                current = WimImageInfo{};
            }
            in_image = true;
            try {
                std::string idx_str = line.substr(6);
                trim(idx_str);
                current.index = std::stoi(idx_str);
            } catch (...) {
                current.index = 1;
            }
        } else if (in_image) {
            if (line.rfind("Name:", 0) == 0) {
                current.name = line.substr(5);
                trim(current.name);
            } else if (line.rfind("Description:", 0) == 0) {
                current.description = line.substr(12);
                trim(current.description);
            } else if (line.rfind("Total Bytes:", 0) == 0) {
                std::string bytes_str = line.substr(12);
                trim(bytes_str);
                try {
                    current.total_bytes = std::stoull(bytes_str);
                } catch (...) {
                    current.total_bytes = 0;
                }
            }
        }
    }

    if (in_image) {
        results.push_back(current);
    }

    return results;
}

std::vector<WimImageInfo> WimDeployer::get_image_info(
    const std::string& image_path,
    const std::string& wimlib_tool
) {
    if (!std::filesystem::exists(image_path)) {
        return {};
    }

    auto proc = ProcessRunner::run(wimlib_tool, {"info", image_path});
    if (!proc.success()) {
        return {};
    }

    return parse_info_output(proc.stdout_output);
}

WimDeployResult WimDeployer::apply_image(const WimDeployOptions& options) {
    WimDeployResult result;

    if (!std::filesystem::exists(options.image_path)) {
        result.success = false;
        result.error_message = "Image file does not exist: " + options.image_path;
        return result;
    }

    if (!std::filesystem::exists(options.target_dir)) {
        result.success = false;
        result.error_message = "Target directory does not exist: " + options.target_dir;
        return result;
    }

    std::vector<std::string> args = {
        "apply",
        options.image_path,
        std::to_string(options.image_index),
        options.target_dir
    };

    if (options.verify_integrity) {
        args.push_back("--check");
    }

    std::string line_buf;
    std::regex pct_re(R"(\((\d+)%\))");

    auto proc = ProcessRunner::run_streaming(
        options.wimlib_tool,
        args,
        [&](std::string_view chunk) {
            if (!options.progress_callback) return;
            for (char c : chunk) {
                if (c == '\r' || c == '\n') {
                    if (!line_buf.empty()) {
                        std::smatch m;
                        if (std::regex_search(line_buf, m, pct_re)) {
                            try {
                                int pct = std::stoi(m[1].str());
                                options.progress_callback(pct, line_buf);
                            } catch (...) {}
                        }
                        line_buf.clear();
                    }
                } else {
                    line_buf.push_back(c);
                }
            }
        }
    );
    result.exit_code = proc.exit_code;
    result.stdout_output = proc.stdout_output;
    result.stderr_output = proc.stderr_output;

    if (proc.success()) {
        result.success = true;
    } else {
        result.success = false;
        result.error_message = proc.error_message.empty() ? proc.stderr_output : proc.error_message;
        if (result.error_message.empty()) {
            result.error_message = "wimlib-imagex failed with exit code " + std::to_string(proc.exit_code);
        }
    }

    return result;
}

} // namespace litewin

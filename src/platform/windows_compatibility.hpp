#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace litewin {

enum class HardwareSupport {
    Supported,
    Unsupported,
    Unknown
};

inline std::string to_string(HardwareSupport support) {
    switch (support) {
        case HardwareSupport::Supported:   return "Supported";
        case HardwareSupport::Unsupported: return "Unsupported";
        case HardwareSupport::Unknown:
        default:                           return "Unknown";
    }
}

struct WindowsCompatibility {
    HardwareSupport tpm = HardwareSupport::Unknown;
    HardwareSupport secure_boot = HardwareSupport::Unknown;
    std::optional<bool> secure_boot_enabled;
    bool uefi = false;

    std::string summary() const {
        std::ostringstream oss;
        oss << "UEFI: " << (uefi ? "Supported" : "Unsupported (Legacy BIOS)") << "\n";
        oss << "TPM: " << to_string(tpm)
            << (bypass_tpm() ? " [Bypass Candidate]" : "") << "\n";
        oss << "Secure Boot: " << to_string(secure_boot);
        if (secure_boot_enabled.has_value()) {
            oss << " (" << (*secure_boot_enabled ? "Enabled" : "Disabled") << ")";
        }
        if (bypass_secure_boot()) {
            oss << " [Bypass Candidate]";
        }
        oss << "\n";
        oss << "Notice: Assessment is diagnostic only and gives no guarantee of Windows installation success.";
        return oss.str();
    }

    bool bypass_tpm() const {
        return tpm == HardwareSupport::Unsupported;
    }

    bool bypass_secure_boot() const {
        return secure_boot == HardwareSupport::Unsupported && uefi;
    }
};

namespace detail {
inline std::filesystem::path resolve_sysfs(const std::filesystem::path& root, const std::filesystem::path& relative_path) {
    if (root.empty() || root == "/") {
        return "/" / relative_path;
    }
    std::string rel = relative_path.generic_string();
    while (!rel.empty() && rel.front() == '/') {
        rel.erase(rel.begin());
    }
    return root / rel;
}
} // namespace detail

inline WindowsCompatibility inspect_windows_compatibility(std::filesystem::path root = "/") {
    WindowsCompatibility result;

    // 1. TPM inspection via sys/class/tpm entries tpm*/tpm_version_major
    std::error_code ec;
    auto tpm_class_dir = detail::resolve_sysfs(root, "sys/class/tpm");

    size_t devices_found = 0;
    bool has_supported = false;
    bool has_unsupported = false;
    bool has_unknown = false;

    if (std::filesystem::is_directory(tpm_class_dir, ec)) {
        for (std::filesystem::directory_iterator it(tpm_class_dir, ec), end; it != end; it.increment(ec)) {
            const auto& entry = *it;
            if (ec) {
                has_unknown = true;
                break;
            }
            const auto fname = entry.path().filename().string();
            if (fname.rfind("tpm", 0) != 0) {
                continue;
            }
            devices_found++;

            auto ver_path = entry.path() / "tpm_version_major";
            std::ifstream ver_file(ver_path);
            if (!ver_file.is_open()) {
                has_unknown = true;
                continue;
            }

            std::string line;
            if (!std::getline(ver_file, line)) {
                has_unknown = true;
                continue;
            }

            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                line.erase(line.begin());
            }

            if (line == "2") {
                has_supported = true;
            } else if (line == "1") {
                has_unsupported = true;
            } else {
                has_unknown = true;
            }
        }
    }

    if (ec) has_unknown = true;
    if (devices_found == 0) {
        result.tpm = HardwareSupport::Unknown;
    } else if (has_supported) {
        result.tpm = HardwareSupport::Supported;
    } else if (has_unsupported && !has_unknown) {
        result.tpm = HardwareSupport::Unsupported;
    } else {
        result.tpm = HardwareSupport::Unknown;
    }

    // 2. UEFI and Secure Boot inspection
    auto efi_dir = detail::resolve_sysfs(root, "sys/firmware/efi");
    if (!std::filesystem::is_directory(efi_dir, ec)) {
        result.uefi = false;
        result.secure_boot = HardwareSupport::Unknown;
        result.secure_boot_enabled = std::nullopt;
        return result;
    }

    result.uefi = true;

    auto secboot_path = detail::resolve_sysfs(root, "sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c");
    std::ifstream secboot_file(secboot_path, std::ios::binary);
    if (!secboot_file.is_open()) {
        result.secure_boot = HardwareSupport::Unknown;
        result.secure_boot_enabled = std::nullopt;
        return result;
    }

    std::vector<char> var_bytes(6);
    secboot_file.read(var_bytes.data(), 6);
    var_bytes.resize(static_cast<size_t>(secboot_file.gcount()));
    if (var_bytes.size() == 5) {
        unsigned char val = static_cast<unsigned char>(var_bytes[4]);
        if (val == 1) {
            result.secure_boot = HardwareSupport::Supported;
            result.secure_boot_enabled = true;
        } else if (val == 0) {
            result.secure_boot = HardwareSupport::Supported;
            result.secure_boot_enabled = false;
        } else {
            result.secure_boot = HardwareSupport::Unknown;
            result.secure_boot_enabled = std::nullopt;
        }
    } else {
        result.secure_boot = HardwareSupport::Unknown;
        result.secure_boot_enabled = std::nullopt;
    }

    return result;
}

} // namespace litewin

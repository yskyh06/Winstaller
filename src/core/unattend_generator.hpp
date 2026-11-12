#pragma once

#include <string>

namespace litewin {

struct UnattendOptions {
    std::string username = "User";
    std::string password = "";
    std::string computer_name = ""; // If empty, generated e.g. DESKTOP-XXXXXX
    std::string timezone = "Korea Standard Time";
    std::string language = "ko-KR";

    // Privacy & Anti-tracking (Auto-disabled by default)
    bool disable_telemetry = true;
    bool disable_advertising_id = true;
    bool disable_tailored_experiences = true;

    // Bypass & Security protection
    bool create_local_account = true;
    bool bypass_nro = true;            // Skip Microsoft Online Account
    bool bypass_tpm_secureboot = false; // Bypass TPM/SecureBoot checks
    bool prevent_bitlocker = true;     // Prevent automatic BitLocker encryption
    bool disable_oobe_update = true;   // Skip Windows Update check in OOBE (DisableZAU)

    // Explorer & Usability tweaks
    bool show_file_extensions = true;  // HideFileExt = 0
    bool disable_edge_first_run = true;// Hide First Run Experience

    // Independently selected from read-only hardware assessment.
    bool bypass_tpm = false;
    bool bypass_secure_boot = false;

    // Post-install partition cleanup
    bool auto_extend_c_drive = true;   // Deletes 5th partition and extends C:
};

class UnattendGenerator {
public:
    // Generates complete valid unattended XML string
    [[nodiscard]] static std::string generate_xml(const UnattendOptions& options);

    // Generates the diskpart script for post-install partition cleanup
    [[nodiscard]] static std::string generate_extend_script();
};

} // namespace litewin

#include "core/unattend_generator.hpp"

#include <random>
#include <sstream>

namespace litewin {

static std::string escape_xml(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '\"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c; break;
        }
    }
    return out;
}

static std::string generate_random_computer_name() {
    static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937 rng(rd());
    std::string name = "DESKTOP-";
    for (int i = 0; i < 7; ++i) {
        name += charset[rng() % (sizeof(charset) - 1)];
    }
    return name;
}

std::string UnattendGenerator::generate_extend_script() {
    return "select volume C\n"
           "select partition 5\n"
           "remove all dismount noerr\n"
           "delete partition override\n"
           "select volume C\n"
           "extend\n"
           "exit\n";
}

std::string UnattendGenerator::generate_xml(const UnattendOptions& options) {
    std::ostringstream oss;

    std::string comp_name = options.computer_name.empty()
        ? generate_random_computer_name()
        : options.computer_name;

    oss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    oss << "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n";
    oss << "          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\"\n";
    oss << "          xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n";

    // 1. specialize pass
    oss << "  <settings pass=\"specialize\">\n";
    oss << "    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"amd64\" "
           "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n";
    oss << "      <ComputerName>" << escape_xml(comp_name) << "</ComputerName>\n";
    oss << "      <TimeZone>" << escape_xml(options.timezone) << "</TimeZone>\n";
    oss << "    </component>\n";

    // RunSynchronous commands in specialize pass
    oss << "    <component name=\"Microsoft-Windows-Deployment\" processorArchitecture=\"amd64\" "
           "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n";
    oss << "      <RunSynchronous>\n";

    int sync_order = 1;
    auto add_sync_cmd = [&](const std::string& cmd, const std::string& desc) {
        oss << "        <RunSynchronousCommand wcm:action=\"add\">\n";
        oss << "          <Order>" << sync_order++ << "</Order>\n";
        oss << "          <Path>" << escape_xml("cmd.exe /c \"" + cmd + " & exit /b 0\"") << "</Path>\n";
        oss << "          <Description>" << escape_xml(desc) << "</Description>\n";
        oss << "        </RunSynchronousCommand>\n";
    };

    if (options.bypass_tpm_secureboot) {
        add_sync_cmd("reg.exe add \"HKLM\\SYSTEM\\Setup\\LabConfig\" /v \"BypassTPMCheck\" /t REG_DWORD /d 1 /f", "Bypass TPM Check");
        add_sync_cmd("reg.exe add \"HKLM\\SYSTEM\\Setup\\LabConfig\" /v \"BypassSecureBootCheck\" /t REG_DWORD /d 1 /f", "Bypass SecureBoot Check");
        add_sync_cmd("reg.exe add \"HKLM\\SYSTEM\\Setup\\LabConfig\" /v \"BypassRAMCheck\" /t REG_DWORD /d 1 /f", "Bypass RAM Check");
        add_sync_cmd("reg.exe add \"HKLM\\SYSTEM\\Setup\\LabConfig\" /v \"BypassStorageCheck\" /t REG_DWORD /d 1 /f", "Bypass Storage Check");
        add_sync_cmd("reg.exe add \"HKLM\\SYSTEM\\Setup\\LabConfig\" /v \"BypassCPUCheck\" /t REG_DWORD /d 1 /f", "Bypass CPU Check");
        add_sync_cmd("reg.exe add \"HKLM\\SYSTEM\\Setup\\MoSetup\" /v \"AllowUpgradesWithUnsupportedTPMOrCPU\" /t REG_DWORD /d 1 /f", "Allow upgrades on unsupported CPU/TPM");
    }

    if (options.bypass_nro) {
        add_sync_cmd("reg.exe add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v \"BypassNRO\" /t REG_DWORD /d 1 /f", "Bypass NRO requirement");
    }

    if (options.disable_oobe_update) {
        add_sync_cmd("reg.exe add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v \"DisableZAU\" /t REG_DWORD /d 1 /f", "Disable OOBE Zero Day Auto Update");
    }

    if (options.prevent_bitlocker) {
        add_sync_cmd("reg.exe add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\BitLocker\" /v \"PreventDeviceEncryption\" /t REG_DWORD /d 1 /f", "Prevent BitLocker auto-encryption");
    }

    if (options.disable_telemetry) {
        add_sync_cmd("reg.exe add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection\" /v \"AllowTelemetry\" /t REG_DWORD /d 0 /f", "Disable Telemetry");
        add_sync_cmd("reg.exe add \"HKLM\\SYSTEM\\CurrentControlSet\\Services\\DiagTrack\" /v \"Start\" /t REG_DWORD /d 4 /f", "Disable Diagnostics Tracking Service");
    }

    if (options.disable_advertising_id) {
        add_sync_cmd("reg.exe add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo\" /v \"DisabledByGroupPolicy\" /t REG_DWORD /d 1 /f", "Disable Advertising ID");
    }

    if (options.disable_edge_first_run) {
        add_sync_cmd("reg.exe add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge\" /v \"HideFirstRunExperience\" /t REG_DWORD /d 1 /f", "Hide Edge first run experience");
    }

    oss << "      </RunSynchronous>\n";
    oss << "    </component>\n";
    oss << "  </settings>\n";

    // 2. oobeSystem pass
    oss << "  <settings pass=\"oobeSystem\">\n";
    oss << "    <component name=\"Microsoft-Windows-International-Core\" processorArchitecture=\"amd64\" "
           "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n";
    oss << "      <InputLocale>" << escape_xml(options.language) << "</InputLocale>\n";
    oss << "      <SystemLocale>" << escape_xml(options.language) << "</SystemLocale>\n";
    oss << "      <UILanguage>" << escape_xml(options.language) << "</UILanguage>\n";
    oss << "      <UserLocale>" << escape_xml(options.language) << "</UserLocale>\n";
    oss << "    </component>\n";

    oss << "    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"amd64\" "
           "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n";
    oss << "      <OOBE>\n";
    oss << "        <HideEULAPage>true</HideEULAPage>\n";
    oss << "        <HideLocalAccountScreen>true</HideLocalAccountScreen>\n";
    oss << "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n";
    oss << "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n";
    oss << "        <ProtectYourPC>3</ProtectYourPC>\n";
    oss << "      </OOBE>\n";

    oss << "      <UserAccounts>\n";
    oss << "        <LocalAccounts>\n";
    oss << "          <LocalAccount wcm:action=\"add\">\n";
    oss << "            <Name>" << escape_xml(options.username) << "</Name>\n";
    oss << "            <Group>Administrators</Group>\n";
    oss << "            <Password>\n";
    oss << "              <Value>" << escape_xml(options.password) << "</Value>\n";
    oss << "              <PlainText>true</PlainText>\n";
    oss << "            </Password>\n";
    oss << "          </LocalAccount>\n";
    oss << "        </LocalAccounts>\n";
    oss << "      </UserAccounts>\n";

    if (options.password.empty()) {
        oss << "      <AutoLogon>\n";
        oss << "        <Password>\n";
        oss << "          <Value></Value>\n";
        oss << "          <PlainText>true</PlainText>\n";
        oss << "        </Password>\n";
        oss << "        <Enabled>true</Enabled>\n";
        oss << "        <LogonCount>1</LogonCount>\n";
        oss << "        <Username>" << escape_xml(options.username) << "</Username>\n";
        oss << "      </AutoLogon>\n";
    }

    // FirstLogonCommands
    oss << "      <FirstLogonCommands>\n";
    int logon_order = 1;
    auto add_logon_cmd = [&](const std::string& cmd, const std::string& desc) {
        oss << "        <SynchronousCommand wcm:action=\"add\">\n";
        oss << "          <Order>" << logon_order++ << "</Order>\n";
        oss << "          <CommandLine>" << escape_xml(cmd) << "</CommandLine>\n";
        oss << "          <Description>" << escape_xml(desc) << "</Description>\n";
        oss << "        </SynchronousCommand>\n";
    };

    if (options.bypass_tpm_secureboot) {
        add_logon_cmd("reg.exe add \"HKCU\\Control Panel\\UnsupportedHardwareNotificationCache\" /v \"SV1\" /t REG_DWORD /d 0 /f", "Hide unsupported hardware watermark SV1");
        add_logon_cmd("reg.exe add \"HKCU\\Control Panel\\UnsupportedHardwareNotificationCache\" /v \"SV2\" /t REG_DWORD /d 0 /f", "Hide unsupported hardware watermark SV2");
    }

    if (options.show_file_extensions) {
        add_logon_cmd("reg.exe add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v \"HideFileExt\" /t REG_DWORD /d 0 /f", "Show file extensions");
    }

    if (options.disable_tailored_experiences) {
        add_logon_cmd("reg.exe add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Privacy\" /v \"TailoredExperiencesWithDiagnosticDataEnabled\" /t REG_DWORD /d 0 /f", "Disable Tailored Experiences");
    }

    if (options.auto_extend_c_drive) {
        add_logon_cmd("cmd.exe /c \"diskpart.exe /s C:\\Windows\\Panther\\extend_c.txt > C:\\Windows\\Panther\\extend.log 2>&1\"", "Delete Temp Partition and Extend C Drive");
        add_logon_cmd("cmd.exe /c \"mountvol S: /s & if exist S:\\EFI\\Litewin\\boot_stage.flag del /f /q S:\\EFI\\Litewin\\boot_stage.flag & if exist S:\\sources\\boot.wim del /f /q S:\\sources\\boot.wim & mountvol S: /d\"", "Clean Litewin Boot Flag and WinPE from ESP");
    }

    add_logon_cmd("cmd.exe /c \"if exist C:\\Windows\\Panther\\unattend.xml del /f /q C:\\Windows\\Panther\\unattend.xml\"", "Remove unattend.xml containing user credentials");

    oss << "      </FirstLogonCommands>\n";
    oss << "    </component>\n";
    oss << "  </settings>\n";
    oss << "</unattend>\n";

    return oss.str();
}

} // namespace litewin

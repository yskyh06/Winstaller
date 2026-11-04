#include "core/unattend_generator.hpp"

#include <cassert>
#include <iostream>

void test_default_unattend_xml() {
    litewin::UnattendOptions opts;
    std::string xml = litewin::UnattendGenerator::generate_xml(opts);

    // Schema and encoding
    assert(xml.find("<?xml version=\"1.0\" encoding=\"utf-8\"?>") != std::string::npos);
    assert(xml.find("urn:schemas-microsoft-com:unattend") != std::string::npos);

    // Language and timezone
    assert(xml.find("<TimeZone>Korea Standard Time</TimeZone>") != std::string::npos);
    assert(xml.find("<InputLocale>ko-KR</InputLocale>") != std::string::npos);
    assert(xml.find("<UILanguage>ko-KR</UILanguage>") != std::string::npos);

    // User account & auto-logon
    assert(xml.find("<Name>User</Name>") != std::string::npos);
    assert(xml.find("<AutoLogon>") != std::string::npos);

    // Privacy & anti-tracking (Disabled by default)
    assert(xml.find("<ProtectYourPC>3</ProtectYourPC>") != std::string::npos);
    assert(xml.find("AllowTelemetry") != std::string::npos);
    assert(xml.find("DisabledByGroupPolicy") != std::string::npos); // Advertising ID
    assert(xml.find("TailoredExperiencesWithDiagnosticDataEnabled") != std::string::npos);

    // Bypasses & security
    assert(xml.find("BypassTPMCheck") != std::string::npos);
    assert(xml.find("AllowUpgradesWithUnsupportedTPMOrCPU") != std::string::npos);
    assert(xml.find("UnsupportedHardwareNotificationCache") != std::string::npos);
    assert(xml.find("BypassNRO") != std::string::npos);
    assert(xml.find("DisableZAU") != std::string::npos);
    assert(xml.find("PreventDeviceEncryption") != std::string::npos); // BitLocker
    assert(xml.find("HideFileExt") != std::string::npos);             // File extensions
    assert(xml.find("HideFirstRunExperience") != std::string::npos);  // Edge

    // Post-install extend command
    assert(xml.find("extend_c.txt") != std::string::npos);

    std::cout << "[PASS] test_default_unattend_xml\n";
}

void test_custom_user_with_password() {
    litewin::UnattendOptions opts{
        .username = "AdminBob",
        .password = "SecretP@ss123",
        .computer_name = "WORKSTATION-01",
        .timezone = "Tokyo Standard Time",
        .language = "ja-JP",
        .disable_telemetry = false,
        .bypass_tpm_secureboot = false,
    };

    std::string xml = litewin::UnattendGenerator::generate_xml(opts);

    assert(xml.find("<ComputerName>WORKSTATION-01</ComputerName>") != std::string::npos);
    assert(xml.find("<TimeZone>Tokyo Standard Time</TimeZone>") != std::string::npos);
    assert(xml.find("<Name>AdminBob</Name>") != std::string::npos);
    assert(xml.find("<Value>SecretP@ss123</Value>") != std::string::npos);

    // Because password is provided, AutoLogon should not be enabled
    assert(xml.find("<AutoLogon>") == std::string::npos);

    // TPM bypass disabled
    assert(xml.find("BypassTPMCheck") == std::string::npos);

    std::cout << "[PASS] test_custom_user_with_password\n";
}

void test_xml_escaping() {
    litewin::UnattendOptions opts{
        .username = "Alice & Bob <Special>",
        .password = "\"P'ass&123>\"",
        .computer_name = "PC<1>",
    };

    std::string xml = litewin::UnattendGenerator::generate_xml(opts);

    assert(xml.find("Alice &amp; Bob &lt;Special&gt;") != std::string::npos);
    assert(xml.find("&quot;P&apos;ass&amp;123&gt;&quot;") != std::string::npos);
    assert(xml.find("PC&lt;1&gt;") != std::string::npos);

    std::cout << "[PASS] test_xml_escaping\n";
}

void test_extend_script_content() {
    std::string script = litewin::UnattendGenerator::generate_extend_script();
    assert(script.find("select volume C") != std::string::npos);
    assert(script.find("select partition 5") != std::string::npos);
    assert(script.find("remove all dismount noerr") != std::string::npos);
    assert(script.find("delete partition override") != std::string::npos);
    assert(script.find("extend") != std::string::npos);
    std::cout << "[PASS] test_extend_script_content\n";
}

int main() {
    test_default_unattend_xml();
    test_custom_user_with_password();
    test_xml_escaping();
    test_extend_script_content();
    std::cout << "All unattend_generator tests passed!\n";
    return 0;
}

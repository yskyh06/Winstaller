#if __has_include("core/windows_settings.hpp")
#include "core/windows_settings.hpp"
#elif __has_include("../src/core/windows_settings.hpp")
#include "../src/core/windows_settings.hpp"
#else
#include "windows_settings.hpp"
#endif

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#define REQUIRE(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error(std::string("Requirement failed: ") + #cond + " at line " + std::to_string(__LINE__)); \
        } \
    } while (false)

using namespace litewin;

void test_valid_save_cancel_defaultrestore() {
    WindowsSettingsSession session;
    REQUIRE(!session.is_open());
    session.open();
    REQUIRE(session.is_open());

    session.draft().options.username = "Tester";
    session.draft().options.password = "Secret123";
    session.draft().password_confirm = "Secret123";
    session.draft().options.computer_name = "TEST-PC";
    session.draft().options.disable_telemetry = false;
    REQUIRE(session.dirty());

    REQUIRE(session.save());
    REQUIRE(!session.is_open());
    REQUIRE(!session.dirty());
    REQUIRE(session.effective().username == "Tester");
    REQUIRE(session.effective().password == "Secret123");
    REQUIRE(session.effective().computer_name == "TEST-PC");
    REQUIRE(session.effective().disable_telemetry == false);
    REQUIRE(session.decision() == SettingsDecision::Pending);

    session.open();
    session.draft().options.username = "Changed";
    session.draft().options.password = "OtherPwd";
    session.draft().password_confirm = "OtherPwd";
    REQUIRE(session.dirty());
    session.cancel();
    REQUIRE(!session.is_open());
    REQUIRE(!session.dirty());
    REQUIRE(session.effective().username == "Tester");

    session.open();
    session.draft().options.username = "DraftOnly";
    session.restore_defaults();
    REQUIRE(session.draft().options.username == "User");
    REQUIRE(session.effective().username == "Tester");
}

void test_invalid_password() {
    WindowsSettingsSession session;
    session.open();

    session.draft().options.password = "Pass1";
    session.draft().password_confirm = "Pass2";
    REQUIRE(!session.save());
    REQUIRE(!session.error().empty());
    REQUIRE(session.error().find("Pass1") == std::string::npos);
    REQUIRE(session.error().find("Pass2") == std::string::npos);

    session.draft().options.password = std::string(65, 'a');
    session.draft().password_confirm = std::string(65, 'a');
    REQUIRE(!session.save());
    REQUIRE(session.error().find('a') == std::string::npos || session.error().find("between") != std::string::npos);

    session.draft().options.password = std::string("bad\x01pwd");
    session.draft().password_confirm = std::string("bad\x01pwd");
    REQUIRE(!session.save());
}

void test_prior_save_preserved_on_timeout() {
    WindowsSettingsSession session;
    session.open();
    session.draft().options.username = "ValidUser";
    REQUIRE(session.save());
    REQUIRE(session.effective().username == "ValidUser");

    auto t0 = WindowsSettingsSession::Clock::now();
    session.begin_completion(t0, 10);
    REQUIRE(session.completing());

    session.open();
    session.draft().options.username = "Invalid/Name";
    REQUIRE(session.dirty());

    auto expired_time = t0 + std::chrono::seconds(11);
    auto dec = session.poll_completion(expired_time);
    REQUIRE(dec == SettingsDecision::Discarded);
    REQUIRE(session.effective().username == "ValidUser");
    REQUIRE(!session.is_open());
}

void test_never_saved_defaults_on_timeout() {
    UnattendOptions def;
    def.username = "OriginalDefault";
    WindowsSettingsSession session(def);

    auto t0 = WindowsSettingsSession::Clock::now();
    session.open();
    session.draft().options.username = "Unfinished";
    session.begin_completion(t0, 5);

    auto expired_time = t0 + std::chrono::seconds(5);
    REQUIRE(session.poll_completion(expired_time) == SettingsDecision::Discarded);
    REQUIRE(session.effective().username == "OriginalDefault");
}

void test_deadline_repeated_calls_not_extended() {
    WindowsSettingsSession session;
    auto t0 = WindowsSettingsSession::Clock::now();
    session.begin_completion(t0, 10);
    session.begin_completion(t0 + std::chrono::seconds(5), 10);

    REQUIRE(session.remaining_seconds(t0 + std::chrono::seconds(4)) == 6);
    REQUIRE(session.remaining_seconds(t0 + std::chrono::seconds(9)) == 1);
    REQUIRE(session.remaining_seconds(t0 + std::chrono::seconds(10)) == 0);

    REQUIRE(session.poll_completion(t0 + std::chrono::seconds(10)) == SettingsDecision::Discarded);
}

void test_boundary_late_save_rejected() {
    {
        WindowsSettingsSession session;
        auto t0 = WindowsSettingsSession::Clock::now();
        session.begin_completion(t0, 10);
        REQUIRE(!session.save(t0 + std::chrono::seconds(10)));
        REQUIRE(session.decision() == SettingsDecision::Discarded);
    }
    {
        WindowsSettingsSession session;
        auto t0 = WindowsSettingsSession::Clock::now();
        session.begin_completion(t0, 10);
        session.draft().options.username = "OnTimeUser";
        REQUIRE(session.save(t0 + std::chrono::seconds(9)));
        REQUIRE(session.decision() == SettingsDecision::Saved);
        REQUIRE(session.effective().username == "OnTimeUser");
    }
}

void test_post_decision_edits_save_cannot_alter_effective() {
    WindowsSettingsSession session;
    auto t0 = WindowsSettingsSession::Clock::now();
    session.draft().options.username = "Committed";
    session.begin_completion(t0, 10);
    REQUIRE(session.save(t0 + std::chrono::seconds(1)));
    REQUIRE(session.decision() == SettingsDecision::Saved);

    session.draft().options.username = "MaliciousOverride";
    REQUIRE(!session.save(t0 + std::chrono::seconds(2)));
    REQUIRE(session.effective().username == "Committed");
    REQUIRE(session.decision() == SettingsDecision::Saved);

    session.cancel();
    REQUIRE(session.decision() == SettingsDecision::Saved);
    REQUIRE(session.effective().username == "Committed");

    session.reset_for_retry();
    REQUIRE(session.decision() == SettingsDecision::Saved);
    REQUIRE(session.effective().username == "Committed");
}

void test_draft_security_flags_not_copied() {
    WindowsSettingsSession session;
    session.open();
    REQUIRE(session.effective().bypass_nro == true);
    REQUIRE(session.effective().bypass_tpm_secureboot == false);
    REQUIRE(session.effective().language == "ko-KR");

    session.draft().options.bypass_nro = false;
    session.draft().options.bypass_tpm_secureboot = true;
    session.draft().options.language = "en-US";
    REQUIRE(!session.dirty());

    session.draft().options.username = "ValidUser";
    REQUIRE(session.dirty());
    REQUIRE(session.save());

    REQUIRE(session.effective().username == "ValidUser");
    REQUIRE(session.effective().bypass_nro == true);
    REQUIRE(session.effective().bypass_tpm_secureboot == false);
    REQUIRE(session.effective().language == "ko-KR");
}

void test_username_and_computer_name_validation() {
    WindowsSettingsSession session;

    session.draft().options.username = "";
    REQUIRE(!session.save());
    session.draft().options.username = std::string(21, 'a');
    REQUIRE(!session.save());
    session.draft().options.username = "bad:user";
    REQUIRE(!session.save());
    session.draft().options.username = " leading";
    REQUIRE(!session.save());
    session.draft().options.username = "trailing ";
    REQUIRE(!session.save());
    session.draft().options.username = ".dotlead";
    REQUIRE(!session.save());
    session.draft().options.username = "dottrail.";
    REQUIRE(!session.save());
    session.draft().options.username = "Administrator";
    REQUIRE(!session.save());
    session.draft().options.username = "guest";
    REQUIRE(!session.save());
    session.draft().options.username = "DefaultAccount";
    REQUIRE(!session.save());
    session.draft().options.username = "wdagutilityaccount";
    REQUIRE(!session.save());

    session.draft().options.username = "GoodUser";
    session.draft().options.computer_name = "-LeadingHyphen";
    REQUIRE(!session.save());
    session.draft().options.computer_name = "TrailingHyphen-";
    REQUIRE(!session.save());
    session.draft().options.computer_name = "12345678";
    REQUIRE(!session.save());
    session.draft().options.computer_name = std::string(16, 'A');
    REQUIRE(!session.save());
    session.draft().options.computer_name = "Invalid_Under";
    REQUIRE(!session.save());

    session.draft().options.computer_name = "VALID-PC-01";
    REQUIRE(session.save());
    REQUIRE(session.effective().computer_name == "VALID-PC-01");

    session.draft().options.computer_name = "";
    REQUIRE(session.save());
    REQUIRE(session.effective().computer_name.empty());
}

void test_dirty_and_remaining_seconds() {
    WindowsSettingsSession session;
    session.open();
    REQUIRE(session.is_open());
    session.begin_completion(WindowsSettingsSession::Clock::now(), 10);
    REQUIRE(!session.is_open());

    session.open();
    session.draft().password_confirm = "mismatch_only";
    REQUIRE(session.dirty());
}

int main() {
    try {
        test_valid_save_cancel_defaultrestore();
        test_invalid_password();
        test_prior_save_preserved_on_timeout();
        test_never_saved_defaults_on_timeout();
        test_deadline_repeated_calls_not_extended();
        test_boundary_late_save_rejected();
        test_post_decision_edits_save_cannot_alter_effective();
        test_draft_security_flags_not_copied();
        test_username_and_computer_name_validation();
        test_dirty_and_remaining_seconds();
        std::cout << "All tests passed successfully.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << "\n";
        return 1;
    }
}

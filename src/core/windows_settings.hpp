#pragma once

#include "core/unattend_generator.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <cctype>

namespace litewin {

struct WindowsSettingsDraft {
    UnattendOptions options;
    std::string password_confirm;
};

enum class SettingsDecision {
    Pending,
    Saved,
    Discarded
};

class WindowsSettingsSession {
public:
    using Clock = std::chrono::steady_clock;

    explicit WindowsSettingsSession(UnattendOptions defaults = {})
        : initial_defaults_(defaults),
          saved_options_(defaults),
          draft_{defaults, defaults.password},
          editor_open_(false),
          completing_(false),
          deadline_{},
          decision_(SettingsDecision::Pending),
          last_error_{}
    {}

    void open() noexcept {
        if (decision_ != SettingsDecision::Pending) {
            return;
        }
        editor_open_ = true;
    }

    [[nodiscard]] bool is_open() const noexcept {
        return editor_open_;
    }

    [[nodiscard]] WindowsSettingsDraft& draft() noexcept {
        return draft_;
    }

    [[nodiscard]] const WindowsSettingsDraft& draft() const noexcept {
        return draft_;
    }

    [[nodiscard]] UnattendOptions effective() const {
        return saved_options_;
    }

    [[nodiscard]] SettingsDecision decision() const noexcept {
        return decision_;
    }

    [[nodiscard]] bool dirty() const noexcept {
        return draft_.options.create_local_account != saved_options_.create_local_account ||
               (draft_.options.create_local_account &&
                (draft_.options.username != saved_options_.username ||
                 draft_.options.password != saved_options_.password ||
                 draft_.password_confirm != saved_options_.password)) ||
               draft_.options.computer_name != saved_options_.computer_name ||
               draft_.options.disable_telemetry != saved_options_.disable_telemetry ||
               draft_.options.disable_advertising_id != saved_options_.disable_advertising_id ||
               draft_.options.disable_tailored_experiences != saved_options_.disable_tailored_experiences;
    }

    bool save(Clock::time_point now = Clock::now()) {
        if (decision_ != SettingsDecision::Pending) {
            last_error_ = "Session already completed";
            return false;
        }

        if (completing_ && now >= deadline_) {
            decision_ = SettingsDecision::Discarded;
            editor_open_ = false;
            draft_.options = saved_options_;
            draft_.password_confirm = saved_options_.password;
            last_error_ = "Deadline expired";
            return false;
        }

        std::string err;
        if (!validate_draft(draft_, err)) {
            last_error_ = err;
            return false;
        }

        saved_options_.create_local_account = draft_.options.create_local_account;
        if (saved_options_.create_local_account) {
            saved_options_.username = draft_.options.username;
            saved_options_.password = draft_.options.password;
        } else {
            saved_options_.username = initial_defaults_.username;
            saved_options_.password.clear();
        }
        saved_options_.computer_name = draft_.options.computer_name;
        saved_options_.disable_telemetry = draft_.options.disable_telemetry;
        saved_options_.disable_advertising_id = draft_.options.disable_advertising_id;
        saved_options_.disable_tailored_experiences = draft_.options.disable_tailored_experiences;

        draft_.options = saved_options_;
        draft_.password_confirm = saved_options_.password;

        editor_open_ = false;
        if (completing_) {
            decision_ = SettingsDecision::Saved;
        }
        last_error_.clear();
        return true;
    }

    void cancel() noexcept {
        if (decision_ != SettingsDecision::Pending) {
            return;
        }
        draft_.options = saved_options_;
        draft_.password_confirm = saved_options_.password;
        editor_open_ = false;
        if (completing_) {
            decision_ = SettingsDecision::Discarded;
        }
        last_error_.clear();
    }

    void restore_defaults() noexcept {
        draft_.options.create_local_account = initial_defaults_.create_local_account;
        draft_.options.username = initial_defaults_.username;
        draft_.options.password = initial_defaults_.password;
        draft_.password_confirm = initial_defaults_.password;
        draft_.options.computer_name = initial_defaults_.computer_name;
        draft_.options.disable_telemetry = initial_defaults_.disable_telemetry;
        draft_.options.disable_advertising_id = initial_defaults_.disable_advertising_id;
        draft_.options.disable_tailored_experiences = initial_defaults_.disable_tailored_experiences;
        last_error_.clear();
    }

    [[nodiscard]] std::string error() const {
        return last_error_;
    }

    void begin_completion(Clock::time_point now, int seconds = 10) noexcept {
        if (decision_ != SettingsDecision::Pending) {
            return;
        }
        if (!completing_) {
            completing_ = true;
            deadline_ = now + std::chrono::seconds(seconds);
        }
        if (!dirty()) {
            editor_open_ = false;
        }
    }

    void begin_completion(int seconds = 10) noexcept {
        begin_completion(Clock::now(), seconds);
    }

    [[nodiscard]] bool completing() const noexcept {
        return completing_;
    }

    [[nodiscard]] int remaining_seconds(Clock::time_point now = Clock::now()) const noexcept {
        if (!completing_ || now >= deadline_) {
            return 0;
        }
        auto diff_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline_ - now).count();
        if (diff_ns <= 0) {
            return 0;
        }
        return static_cast<int>((diff_ns + 999'999'999LL) / 1'000'000'000LL);
    }

    SettingsDecision poll_completion(Clock::time_point now = Clock::now()) {
        return poll_completion_impl(now);
    }

    SettingsDecision poll_completion(Clock::time_point now = Clock::now()) const {
        return poll_completion_impl(now);
    }

    void reset_for_retry() noexcept {
        if (decision_ != SettingsDecision::Pending) {
            return;
        }
        completing_ = false;
        deadline_ = Clock::time_point{};
        draft_.options = saved_options_;
        draft_.password_confirm = saved_options_.password;
        editor_open_ = false;
        last_error_.clear();
    }

private:
    SettingsDecision poll_completion_impl(Clock::time_point now) const {
        if (decision_ != SettingsDecision::Pending) {
            return decision_;
        }
        if (completing_ && now >= deadline_) {
            decision_ = SettingsDecision::Discarded;
            editor_open_ = false;
            draft_.options = saved_options_;
            draft_.password_confirm = saved_options_.password;
        }
        return decision_;
    }

    static bool validate_draft(const WindowsSettingsDraft& d, std::string& err) {
        if (d.options.create_local_account) {
            const auto& user = d.options.username;
            if (user.empty() || user.length() > 20) {
                err = "Username must be between 1 and 20 characters";
                return false;
            }
            for (char ch : user) {
                auto uc = static_cast<unsigned char>(ch);
                if (uc < 32 || uc > 126) {
                    err = "Username must contain only printable ASCII characters";
                    return false;
                }
            }
            constexpr std::string_view forbidden = "/\\[]:;|=,+*?<>\"";
            for (char ch : user) {
                if (forbidden.find(ch) != std::string_view::npos) {
                    err = "Username contains forbidden characters";
                    return false;
                }
            }
            if (user.front() == ' ' || user.front() == '.' || user.back() == ' ' || user.back() == '.') {
                err = "Username cannot start or end with a space or dot";
                return false;
            }

            std::string lower_user;
            lower_user.reserve(user.length());
            for (char ch : user) {
                lower_user.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (lower_user == "administrator" || lower_user == "guest" ||
                lower_user == "defaultaccount" || lower_user == "wdagutilityaccount") {
                err = "Username is a reserved account name";
                return false;
            }

            const auto& pwd = d.options.password;
            if (pwd.length() > 64) {
                err = "Password length must be between 0 and 64 characters";
                return false;
            }
            for (char ch : pwd) {
                auto uc = static_cast<unsigned char>(ch);
                if (uc < 32 || uc > 126) {
                    err = "Password must contain only printable ASCII characters";
                    return false;
                }
            }
            if (pwd != d.password_confirm) {
                err = "Password and confirmation do not match";
                return false;
            }

        }

        const auto& comp = d.options.computer_name;
        if (!comp.empty()) {
            if (comp.length() > 15) {
                err = "Computer name length must be between 1 and 15 characters";
                return false;
            }
            bool has_non_digit = false;
            for (char ch : comp) {
                bool is_alnum = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
                if (!is_alnum && ch != '-') {
                    err = "Computer name must contain only alphanumeric characters or hyphens";
                    return false;
                }
                if (ch < '0' || ch > '9') {
                    has_non_digit = true;
                }
            }
            if (comp.front() == '-' || comp.back() == '-') {
                err = "Computer name cannot start or end with a hyphen";
                return false;
            }
            if (!has_non_digit) {
                err = "Computer name cannot be all numeric";
                return false;
            }
        }

        return true;
    }

    UnattendOptions initial_defaults_;
    UnattendOptions saved_options_;
    mutable WindowsSettingsDraft draft_;
    mutable bool editor_open_;
    mutable bool completing_;
    mutable Clock::time_point deadline_;
    mutable SettingsDecision decision_;
    mutable std::string last_error_;
};

} // namespace litewin

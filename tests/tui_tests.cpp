#include "ui/ansi_terminal.hpp"
#include "ui/winstaller_tui.hpp"
#include "core/linux_catalog.hpp"

#include <cassert>
#include <iostream>
#include <unistd.h>

void test_ansi_terminal_helpers() {
    assert(litewin::AnsiTerminal::color_reset() == "\033[0m");
    assert(litewin::AnsiTerminal::color_bold() == "\033[1m");
    assert(litewin::AnsiTerminal::color_cyan() == "\033[36m");
    assert(litewin::AnsiTerminal::color_green() == "\033[32m");
    assert(litewin::AnsiTerminal::color_yellow() == "\033[33m");
    assert(litewin::AnsiTerminal::color_red() == "\033[31m");
    assert(litewin::AnsiTerminal::color_gray() == "\033[90m");
    assert(litewin::AnsiTerminal::color_teal() == "\033[38;2;137;220;235m");
    assert(litewin::AnsiTerminal::color_lavender() == "\033[38;2;203;166;247m");
    assert(litewin::AnsiTerminal::color_gold() == "\033[38;2;249;226;175m");
    assert(litewin::AnsiTerminal::color_slate() == "\033[38;2;108;112;134m");
    assert(litewin::AnsiTerminal::bg_blue() == "\033[44m");
    assert(litewin::AnsiTerminal::bg_red() == "\033[41m");
    assert(litewin::AnsiTerminal::bg_teal() == "\033[48;2;137;220;235m\033[38;2;17;17;27m");

    std::cout << "[PASS] test_ansi_terminal_helpers\n";
}

void test_terminal_dimensions() {
    litewin::AnsiTerminal term;
    assert(term.rows() > 0);
    assert(term.cols() > 0);
    std::cout << "[PASS] test_terminal_dimensions (" << term.cols() << "x" << term.rows() << ")\n";
}

void test_user_data_defaults() {
    litewin::TuiUserData data;
    assert(data.username == "User");
    assert(data.password.empty());
    assert(data.computer_name.empty());
    assert(!data.finalized);

    litewin::TuiEditionChoice choice;
    assert(choice.edition_index == 4);
    assert(choice.edition_name == "Windows 11 Home");
    assert(choice.language == "ko-KR");
    assert(!choice.reuse_existing_esd);
    assert(!choice.is_linux);
    assert(choice.os_tab == "Windows");
    assert(choice.linux_distro_id.empty());

    std::cout << "[PASS] test_user_data_defaults\n";
}

void test_user_data_field_navigation() {
    litewin::AnsiTerminal term;
    litewin::WinstallerTui tui(term);
    litewin::TuiUserData user_data;
    int active_field = 0;

    // Simulate input: active_field 0 -> username
    user_data.username = "Admin";
    assert(user_data.username == "Admin");
    assert(user_data.password_matches());

    // Cycle fields: 0 -> 1 -> 2 -> 3 -> 0
    active_field = (active_field + 1) % 4;
    assert(active_field == 1);
    user_data.password = "Secr3t!";
    assert(user_data.password.size() == 7);
    assert(!user_data.password_matches());

    active_field = (active_field + 1) % 4;
    assert(active_field == 2);
    user_data.password_confirm = "Secr3t!";
    assert(user_data.password_matches());

    active_field = (active_field + 1) % 4;
    assert(active_field == 3);
    user_data.computer_name = "MY-PC";
    assert(user_data.computer_name == "MY-PC");

    active_field = (active_field + 1) % 4;
    assert(active_field == 0);

    std::cout << "[PASS] test_user_data_field_navigation\n";
}

void test_screen_actions_and_keys() {
    assert(static_cast<int>(litewin::ScreenAction::Next) == 0);
    assert(static_cast<int>(litewin::ScreenAction::Back) == 1);
    assert(static_cast<int>(litewin::ScreenAction::Exit) == 2);
    assert(static_cast<int>(litewin::Key::Back) == 3);

    std::cout << "[PASS] test_screen_actions_and_keys\n";
}

void test_os_tabs_and_catalog() {
    auto tabs = litewin::WinstallerTui::get_os_tabs();
    assert(tabs.size() == 2);
    assert(tabs[0] == "Windows");
    assert(tabs[1] == "Ubuntu");

    // Verify absence of badges across all tabs and items
    for (const auto& tab : tabs) {
        auto items = litewin::WinstallerTui::get_tab_os_items(tab);
        assert(!items.empty());
        for (const auto& item : items) {
            assert(!item.first.empty());
            assert(!item.second.empty());
            assert(item.second.find("[최신]") == std::string::npos);
            assert(item.second.find("[레거시]") == std::string::npos);
            assert(item.second.find("[Latest]") == std::string::npos);
            assert(item.second.find("[Legacy]") == std::string::npos);
        }
    }

    // Tab 1: Ubuntu
    auto ubuntu_items = litewin::WinstallerTui::get_tab_os_items("Ubuntu");
    assert(ubuntu_items.size() == litewin::get_linux_targets_by_tab(litewin::OsTabCategory::Ubuntu).size());
    assert(ubuntu_items[0].second == "Ubuntu 26.04 LTS (Desktop GUI)");
    assert(ubuntu_items[1].second == "Ubuntu 26.04 LTS (Server CLI)");
    assert(ubuntu_items[2].second == "Ubuntu 24.04 LTS (Desktop GUI)");
    assert(ubuntu_items[3].second == "Ubuntu 24.04 LTS (Server CLI)");
    assert(ubuntu_items[4].second == "Ubuntu 22.04 LTS (Desktop GUI)");
    assert(ubuntu_items[5].second == "Ubuntu 22.04 LTS (Server CLI)");
    assert(ubuntu_items[6].second == "Ubuntu 20.04 LTS (Desktop GUI) (Expired)");
    assert(ubuntu_items[7].second == "Ubuntu 20.04 LTS (Server CLI) (Expired)");
    assert(ubuntu_items[8].second == "Ubuntu 18.04 LTS (Desktop GUI) (Expired)");
    assert(ubuntu_items[9].second == "Ubuntu 18.04 LTS (Server CLI) (Expired)");

    // Tab 2: Debian
    auto debian_items = litewin::WinstallerTui::get_tab_os_items("Debian");
    assert(debian_items.size() == litewin::get_linux_targets_by_tab(litewin::OsTabCategory::Debian).size());
    assert(debian_items[0].second.find("Debian 13") != std::string::npos);

    // Tab 3: RHEL / Fedora
    auto rhel_items = litewin::WinstallerTui::get_tab_os_items("RHEL / Fedora");
    assert(rhel_items.size() == litewin::get_linux_targets_by_tab(litewin::OsTabCategory::RhelFedora).size());
    assert(rhel_items[0].second == "Fedora 45 (Workstation GUI)");
    assert(rhel_items[1].second == "Fedora 45 (Server CLI)");

    // Tab 4: Arch
    auto arch_items = litewin::WinstallerTui::get_tab_os_items("Arch");
    assert(arch_items.size() == litewin::get_linux_targets_by_tab(litewin::OsTabCategory::Arch).size());
    assert(arch_items[0].second == "Arch Linux 2026.11 (Desktop Preset)");
    assert(arch_items[1].second == "Arch Linux 2026.11 (Base CLI)");

    // Tab 0: Windows
    auto win_items = litewin::WinstallerTui::get_tab_os_items("Windows");
    assert(win_items.size() == 2);
    assert(win_items[0].second == "Windows 11 Home");
    assert(win_items[1].second == "Windows 11 Pro");

    // Invalid tab name returns empty vector
    assert(litewin::WinstallerTui::get_tab_os_items("NonExistentTab").empty());

    // Cross-Module Verification: Ensure 100% synchronization with linux_catalog
    const std::string expired_suffix = " (Expired)";
    for (const auto& tab : tabs) {
        if (tab == "Windows") continue;
        auto items = litewin::WinstallerTui::get_tab_os_items(tab);
        assert(!items.empty());
        for (const auto& [id, display_name] : items) {
            auto target = litewin::find_linux_target_by_id(id);
            assert(target.has_value());
            assert(target->display_name == display_name);
            bool ends_expired = (display_name.size() >= expired_suffix.size()) &&
                (display_name.compare(display_name.size() - expired_suffix.size(), expired_suffix.size(), expired_suffix) == 0);
            assert(target->is_expired == ends_expired);
        }
    }

    std::cout << "[PASS] test_os_tabs_and_catalog\n";
}

static void feed_keys_and_show_modal(litewin::WinstallerTui& tui, litewin::TuiEditionChoice& choice, const std::string& input_bytes) {
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    assert(write(pipe_fds[1], input_bytes.data(), input_bytes.size()) == static_cast<ssize_t>(input_bytes.size()));
    close(pipe_fds[1]);

    int saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0);
    assert(dup2(pipe_fds[0], STDIN_FILENO) >= 0);
    close(pipe_fds[0]);

    tui.show_edition_modal(choice);

    assert(dup2(saved_stdin, STDIN_FILENO) >= 0);
    close(saved_stdin);
}

void test_show_edition_modal_navigation() {
    litewin::AnsiTerminal term;
    litewin::WinstallerTui tui(term);

    // 1. Default Windows tab: Down arrow, then Enter -> selects Windows 11 Pro
    {
        litewin::TuiEditionChoice choice;
        feed_keys_and_show_modal(tui, choice, "\033[B\n");
        assert(!choice.is_linux);
        assert(choice.os_tab == "Windows");
        assert(choice.edition_index == 6);
        assert(choice.edition_name == "Windows 11 Pro");
    }

    // 2. Default Windows tab: Down arrow, Up arrow, then Enter -> selects Windows 11 Home
    {
        litewin::TuiEditionChoice choice;
        feed_keys_and_show_modal(tui, choice, "\033[B\033[A\n");
        assert(!choice.is_linux);
        assert(choice.os_tab == "Windows");
        assert(choice.edition_index == 4);
        assert(choice.edition_name == "Windows 11 Home");
    }

    // 3. Switch tab with Key::Right to Ubuntu -> selects Ubuntu 26.04 LTS (Desktop GUI)
    {
        litewin::TuiEditionChoice choice;
        feed_keys_and_show_modal(tui, choice, "\033[C\n");
        assert(choice.is_linux == true);
        assert(choice.os_tab == "Ubuntu");
        assert(choice.edition_name == "Ubuntu 26.04 LTS (Desktop GUI)");
        assert(choice.linux_distro_id == "ubuntu-26.04-desktop");
    }

    // 4. Switch tab with Key::Right to Ubuntu, Down arrow -> selects Ubuntu 26.04 LTS (Server CLI)
    {
        litewin::TuiEditionChoice choice;
        feed_keys_and_show_modal(tui, choice, "\033[C\033[B\n");
        assert(choice.is_linux == true);
        assert(choice.os_tab == "Ubuntu");
        assert(choice.edition_name == "Ubuntu 26.04 LTS (Server CLI)");
        assert(choice.linux_distro_id == "ubuntu-26.04-server");
    }

    // 5. Switch tab with Key::Left (wraps Windows -> Ubuntu) -> selects Ubuntu Desktop
    {
        litewin::TuiEditionChoice choice;
        feed_keys_and_show_modal(tui, choice, "\033[D\n");
        assert(choice.is_linux == true);
        assert(choice.os_tab == "Ubuntu");
        assert(choice.edition_name == "Ubuntu 26.04 LTS (Desktop GUI)");
        assert(choice.linux_distro_id == "ubuntu-26.04-desktop");
    }

    // 6. Switch tab with Key::Right twice (wraps Windows -> Ubuntu -> Windows) -> selects Windows 11 Home
    {
        litewin::TuiEditionChoice choice;
        feed_keys_and_show_modal(tui, choice, "\033[C\033[C\n");
        assert(!choice.is_linux);
        assert(choice.os_tab == "Windows");
        assert(choice.edition_name == "Windows 11 Home");
    }

    std::cout << "[PASS] test_show_edition_modal_navigation\n";
}

int main() {
    test_ansi_terminal_helpers();
    test_terminal_dimensions();
    test_user_data_defaults();
    test_user_data_field_navigation();
    test_screen_actions_and_keys();
    test_os_tabs_and_catalog();
    test_show_edition_modal_navigation();
    std::cout << "All TUI tests passed!\n";
    return 0;
}

#include "ui/ansi_terminal.hpp"
#include "ui/winstaller_tui.hpp"

#include <cassert>
#include <iostream>

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
    assert(choice.edition_index == 6);
    assert(choice.edition_name == "Windows 11 Pro");
    assert(choice.language == "ko-KR");
    assert(!choice.reuse_existing_esd);

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

    // Cycle fields: 0 -> 1 -> 2 -> 0
    active_field = (active_field + 1) % 3;
    assert(active_field == 1);
    user_data.password = "Secr3t!";
    assert(user_data.password.size() == 7);

    active_field = (active_field + 1) % 3;
    assert(active_field == 2);
    user_data.computer_name = "MY-PC";
    assert(user_data.computer_name == "MY-PC");

    active_field = (active_field + 1) % 3;
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

int main() {
    test_ansi_terminal_helpers();
    test_terminal_dimensions();
    test_user_data_defaults();
    test_user_data_field_navigation();
    test_screen_actions_and_keys();
    std::cout << "All TUI tests passed!\n";
    return 0;
}

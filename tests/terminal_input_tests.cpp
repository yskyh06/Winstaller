#include "ui/ansi_terminal.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

#define TEST_REQUIRE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

struct StdinPipeFixture {
    int pipefds[2]{-1, -1};
    int saved_stdin{-1};

    StdinPipeFixture() {
        if (pipe(pipefds) != 0) {
            std::perror("pipe failed");
            std::exit(1);
        }
        saved_stdin = dup(STDIN_FILENO);
        dup2(pipefds[0], STDIN_FILENO);
        close(pipefds[0]);
    }

    void write_data(const std::string& str) {
        ssize_t w = write(pipefds[1], str.data(), str.size());
        (void)w;
    }

    void close_write() {
        if (pipefds[1] != -1) {
            close(pipefds[1]);
            pipefds[1] = -1;
        }
    }

    ~StdinPipeFixture() {
        close_write();
        if (saved_stdin != -1) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
    }
};

void test_read_key_zero_and_queued_pipe() {
    StdinPipeFixture fix;
    litewin::AnsiTerminal term;

    auto ev_empty = term.read_key(0);
    TEST_REQUIRE(ev_empty.key == litewin::Key::None, "Empty pipe read_key(0) must return None");

    fix.write_data("k");
    auto ev_k = term.read_key(0);
    TEST_REQUIRE(ev_k.key == litewin::Key::Char && ev_k.ch == 'k', "Queued 'k' read by read_key(0)");

    auto ev_after = term.read_key(0);
    TEST_REQUIRE(ev_after.key == litewin::Key::None, "Drained pipe read_key(0) must return None");

    std::cout << "[PASS] test_read_key_zero_and_queued_pipe\n";
}

void test_read_key_escape_sequences() {
    StdinPipeFixture fix;
    litewin::AnsiTerminal term;

    for (const auto& sequence : {"\033OP", "\033[[A", "\033[11~"}) {
        fix.write_data(sequence);
        TEST_REQUIRE(term.read_key(0).key == litewin::Key::F1, "F1 encoding");
    }

    fix.write_data("\033[17~");
    auto ev_f6 = term.read_key(0);
    TEST_REQUIRE(ev_f6.key == litewin::Key::F6, "F6 sequence \\033[17~");

    fix.write_data("\033[18~");
    auto ev_f7 = term.read_key(0);
    TEST_REQUIRE(ev_f7.key == litewin::Key::F7, "F7 sequence \\033[18~");

    fix.write_data("\033[19~");
    auto ev_f8 = term.read_key(0);
    TEST_REQUIRE(ev_f8.key == litewin::Key::F8, "F8 sequence \\033[19~");

    fix.write_data("\033[A");
    auto ev_up = term.read_key(0);
    TEST_REQUIRE(ev_up.key == litewin::Key::Up, "Up arrow sequence \\033[A");

    fix.write_data("\033");
    auto ev_esc = term.read_key(0);
    TEST_REQUIRE(ev_esc.key == litewin::Key::Escape, "Single Esc key");

    std::cout << "[PASS] test_read_key_escape_sequences\n";
}

void test_read_key_eof_behavior() {
    StdinPipeFixture fix;
    litewin::AnsiTerminal term;

    fix.close_write();

    auto ev1 = term.read_key(0);
    TEST_REQUIRE(ev1.key == litewin::Key::None, "EOF on read_key(0) returns None without blocking");

    auto ev2 = term.read_key(50);
    TEST_REQUIRE(ev2.key == litewin::Key::None, "EOF on read_key(50) returns None without blocking");

    auto ev3 = term.read_key(-1);
    TEST_REQUIRE(ev3.key == litewin::Key::None, "EOF on read_key(-1) returns None without blocking");

    std::cout << "[PASS] test_read_key_eof_behavior\n";
}

void test_pure_font_policy() {
    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(3840, 2160) == "ter-v32b-double", "4K chooses ter-v32b-double");
    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(2560, 1440) == "ter-v32b", "1440p chooses ter-v32b");
    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(1920, 1080) == "ter-v32b", "1080p chooses ter-v32b");
    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(1280, 720) == "ter-v24b", "720p chooses ter-v24b");
    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(640, 480) == "ter-v16n", "640x480 chooses ter-v16n");

    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(0, 0, 320, 90) == "ter-v32b-double", "Ultrawide 320x90 chooses ter-v32b-double");
    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(0, 0, 160, 45) == "ter-v32b", "Large 160x45 chooses ter-v32b");
    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(0, 0, 80, 24) == "ter-v24b", "Standard 80x24 chooses ter-v24b");
    TEST_REQUIRE(litewin::AnsiTerminal::choose_auto_font(0, 0, 60, 20) == "ter-v16n", "Sub-minimum 60x20 chooses ter-v16n");

    litewin::AnsiTerminal term;
    TEST_REQUIRE(!term.is_manual_font(), "manual_font_set_ must be false initially and after auto");

    std::cout << "[PASS] test_pure_font_policy\n";
}

void test_hover_rendering() {
    using litewin::AnsiTerminal;
    using litewin::Rect;
    const std::string source = "\033[36mA─BC\033[0m";
    const auto result = AnsiTerminal::decorate_hover(source, 5, 10, Rect{5, 11, 2, 1});
    TEST_REQUIRE(result == "\033[36mA\033[7m─\033[27m\033[7mB\033[27mC\033[0m", "Only hovered UTF8 cells reversed; colors preserved");
    TEST_REQUIRE(litewin::visible_width(result) == 4, "Hover doesn't move text");
    TEST_REQUIRE(AnsiTerminal::decorate_hover(source, 4, 10, Rect{5,11,2,1}) == source, "Other row unchanged");
    TEST_REQUIRE(AnsiTerminal::decorate_hover(source, 5, 10, std::nullopt) == source, "Absent hover unchanged");
}

int main() {
    test_read_key_zero_and_queued_pipe();
    test_read_key_escape_sequences();
    test_read_key_eof_behavior();
    test_pure_font_policy();
    test_hover_rendering();
    std::cout << "All terminal input tests passed!\n";
    return 0;
}

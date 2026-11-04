#include "ui/ansi_terminal.hpp"

#include <iostream>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>

namespace litewin {

AnsiTerminal::AnsiTerminal() {
    update_size();
    enter_raw_mode();
    hide_cursor();
}

AnsiTerminal::~AnsiTerminal() {
    show_cursor();
    exit_raw_mode();
}

void AnsiTerminal::enter_raw_mode() {
    if (raw_mode_active_) return;
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &orig_termios_) == 0) {
            termios raw = orig_termios_;
            raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
            raw.c_iflag &= ~(IXON | ICRNL);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
            raw_mode_active_ = true;
        }
    }
}

void AnsiTerminal::exit_raw_mode() {
    if (raw_mode_active_) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
        raw_mode_active_ = false;
    }
}

void AnsiTerminal::hide_cursor() {
    std::cout << "\033[?25l" << std::flush;
}

void AnsiTerminal::show_cursor() {
    std::cout << "\033[?25h" << std::flush;
}

void AnsiTerminal::clear() {
    std::cout << "\033[2J\033[H" << std::flush;
}

void AnsiTerminal::set_cursor(int row, int col) {
    std::cout << "\033[" << row << ";" << col << "H";
}

void AnsiTerminal::print_at(int row, int col, const std::string& text) {
    set_cursor(row, col);
    std::cout << text << std::flush;
}

void AnsiTerminal::fill_rect(int row, int col, int width, int height, char ch) {
    std::string line(width > 0 ? width : 1, ch);
    for (int r = 0; r < height; ++r) {
        set_cursor(row + r, col);
        std::cout << line;
    }
    std::cout << std::flush;
}

void AnsiTerminal::draw_box(int row, int col, int width, int height, const std::string& title, bool rounded) {
    if (width < 2 || height < 2) return;

    std::string tl = rounded ? "╭" : "┌";
    std::string tr = rounded ? "╮" : "┐";
    std::string bl = rounded ? "╰" : "└";
    std::string br = rounded ? "╯" : "┘";

    // Top border
    set_cursor(row, col);
    std::cout << color_slate() << tl;
    int title_len = static_cast<int>(title.size());
    if (!title.empty() && width > title_len + 4) {
        std::cout << " " << color_bold() << color_teal() << title << color_reset() << color_slate() << " ";
        for (int c = 0; c < width - title_len - 4; ++c) std::cout << "─";
    } else {
        for (int c = 0; c < width - 2; ++c) std::cout << "─";
    }
    std::cout << tr << color_reset();

    // Side borders
    for (int r = 1; r < height - 1; ++r) {
        set_cursor(row + r, col);
        std::cout << color_slate() << "│" << color_reset();
        set_cursor(row + r, col + width - 1);
        std::cout << color_slate() << "│" << color_reset();
    }

    // Bottom border
    set_cursor(row + height - 1, col);
    std::cout << color_slate() << bl;
    for (int c = 0; c < width - 2; ++c) std::cout << "─";
    std::cout << br << color_reset() << std::flush;
}

std::string AnsiTerminal::color_reset()    { return "\033[0m"; }
std::string AnsiTerminal::color_bold()     { return "\033[1m"; }
std::string AnsiTerminal::color_cyan()     { return "\033[36m"; }
std::string AnsiTerminal::color_green()    { return "\033[32m"; }
std::string AnsiTerminal::color_yellow()   { return "\033[33m"; }
std::string AnsiTerminal::color_red()      { return "\033[31m"; }
std::string AnsiTerminal::color_gray()     { return "\033[90m"; }
std::string AnsiTerminal::color_teal()     { return "\033[38;2;137;220;235m"; }
std::string AnsiTerminal::color_lavender() { return "\033[38;2;203;166;247m"; }
std::string AnsiTerminal::color_gold()     { return "\033[38;2;249;226;175m"; }
std::string AnsiTerminal::color_slate()    { return "\033[38;2;108;112;134m"; }
std::string AnsiTerminal::bg_blue()        { return "\033[44m"; }
std::string AnsiTerminal::bg_red()         { return "\033[41m"; }
std::string AnsiTerminal::bg_surface()     { return "\033[48;2;49;50;68m"; }
std::string AnsiTerminal::bg_teal()        { return "\033[48;2;137;220;235m\033[38;2;17;17;27m"; }

void AnsiTerminal::update_size() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows_ = ws.ws_row;
        cols_ = ws.ws_col;
    } else {
        rows_ = 24;
        cols_ = 80;
    }
}

KeyEvent AnsiTerminal::read_key(int timeout_ms) {
    struct pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0 || !(pfd.revents & POLLIN)) {
        return KeyEvent{Key::None, 0};
    }

    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return KeyEvent{Key::None, 0};
    }

    if (c == 27) { // ESC or escape sequence
        struct pollfd seq_pfd{};
        seq_pfd.fd = STDIN_FILENO;
        seq_pfd.events = POLLIN;
        if (poll(&seq_pfd, 1, 50) > 0 && (seq_pfd.revents & POLLIN)) {
            char seq[2] = {0, 0};
            if (read(STDIN_FILENO, &seq[0], 1) == 1) {
                if (seq[0] == '[') {
                    if (read(STDIN_FILENO, &seq[1], 1) == 1) {
                        switch (seq[1]) {
                        case 'A': return KeyEvent{Key::Up, 0};
                        case 'B': return KeyEvent{Key::Down, 0};
                        case 'C': return KeyEvent{Key::Right, 0};
                        case 'D': return KeyEvent{Key::Left, 0};
                        default: break;
                        }
                    }
                }
            }
        }
        return KeyEvent{Key::Escape, 0};
    } else if (c == 10 || c == 13) {
        return KeyEvent{Key::Enter, 0};
    } else if (c == 9) {
        return KeyEvent{Key::Tab, 0};
    } else if (c == 127 || c == 8) {
        return KeyEvent{Key::Backspace, 0};
    } else if (c == 'b' || c == 'B') {
        return KeyEvent{Key::Back, c};
    } else if (c == ' ') {
        return KeyEvent{Key::Space, ' '};
    } else {
        return KeyEvent{Key::Char, c};
    }
}

} // namespace litewin

#include "ui/ansi_terminal.hpp"
#include "ui/tui_layout.hpp"
#include "platform/process_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/kd.h>
#include <linux/vt.h>
#endif

namespace litewin {

namespace {
bool native_console_colors() {
#if defined(__linux__)
    int mode = 0;
    return ioctl(STDOUT_FILENO, KDGETMODE, &mode) == 0 && mode == KD_TEXT;
#else
    return false;
#endif
}
struct DisplaySession {
    std::mutex mutex;
    std::string font = "ter-v24b";
    bool manual = false;
};
DisplaySession& display_session() {
    static DisplaySession session;
    return session;
}
} // namespace

AnsiTerminal::AnsiTerminal() {
    {
        auto& session = display_session();
        std::lock_guard lock(session.mutex);
        current_font_ = last_good_font_ = session.font;
        manual_font_set_ = session.manual;
    }
    update_size();
    enter_raw_mode();
    hide_cursor();
    if (is_vt()) {
        if (manual_font_set_) set_font(current_font_);
        else font_auto();
    }
}

AnsiTerminal::~AnsiTerminal() {
    hide_pointer();
    if (is_vt()) std::cout << "\033[?0c";
    show_cursor();
    exit_raw_mode();
}

void AnsiTerminal::draw_pointer(int row, int col) {
    if (!mouse_.is_enabled() || !is_vt() || row < 1 || row > rows_ || col < 1 || col > cols_) return;
    if (pointer_visible_ && cursor_row_ == row && cursor_col_ == col) return;
    cursor_row_ = row;
    cursor_col_ = col;
    ever_moved_ = true;
    set_cursor(row, col);
    // Linux VT software cursor with the blinking hardware cursor hidden.
    if (!native_cursor_set_) {
        std::cout << "\033[?81;0;64c";
        native_cursor_set_ = true;
    }
    show_cursor();
    pointer_visible_ = true;
}

void AnsiTerminal::hide_pointer() {
    if (!pointer_visible_) return;
    hide_cursor();
    pointer_visible_ = false;
}

void AnsiTerminal::enter_raw_mode() {
    native_cursor_set_ = false;
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
            if (is_vt()) {
                std::cout << "\033%G" << std::flush;
                mouse_.set_bounds(rows_, cols_);
                mouse_.set_enabled(true);
            }
        }
    }
}

void AnsiTerminal::exit_raw_mode() {
    if (raw_mode_active_) {
        if (is_vt()) {
            auto& session = display_session();
            std::lock_guard lock(session.mutex);
            session.font = current_font_;
            session.manual = manual_font_set_;
        }
        hide_pointer();
        if (is_vt()) std::cout << "\033[?0c";
        native_cursor_set_ = false;
        mouse_.set_enabled(false);
        mouse_.reset();
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

const std::string& AnsiTerminal::check_mark() {
    if (!check_mark_.empty()) return check_mark_;
    check_mark_ = "✓";
#if defined(__linux__)
    if (is_vt()) {
        std::vector<unipair> entries(4096);
        unimapdesc mapping{static_cast<unsigned short>(entries.size()), entries.data()};
        check_mark_ = "+";
        if (ioctl(STDOUT_FILENO, GIO_UNIMAP, &mapping) == 0) {
            for (unsigned short i = 0; i < mapping.entry_ct; ++i) {
                if (entries[i].unicode == 0x221a) check_mark_ = "√";
                if (entries[i].unicode == 0x2713) {
                    check_mark_ = "✓";
                    break;
                }
            }
        }
    }
#endif
    return check_mark_;
}

void AnsiTerminal::clear() {
    hide_pointer();
    std::cout << color_reset() << "\033[2J\033[H" << std::flush;
}

void AnsiTerminal::set_cursor(int row, int col) {
    std::cout << "\033[" << row << ";" << col << "H";
}

std::string AnsiTerminal::decorate_hover(const std::string& text, int row, int col, std::optional<Rect> area) {
    if (!area || area->empty() || row < area->top() || row > area->bottom()) return text;
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t start = i;
        if (text[i] == '\033') {
            ++i;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size() && (text[i] < '@' || text[i] > '~')) ++i;
                if (i < text.size()) ++i;
            }
            out.append(text, start, i - start);
            continue;
        }
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t length = c < 0x80 ? 1 : ((c & 0xe0) == 0xc0 ? 2 : ((c & 0xf0) == 0xe0 ? 3 : 4));
        length = std::min(length, text.size() - i);
        const bool hovered = area->contains(row, col++);
        if (hovered) out += "\033[7m";
        out.append(text, i, length);
        if (hovered) out += "\033[27m";
        i += length;
    }
    return out;
}

void AnsiTerminal::print_at(int row, int col, const std::string& text) {
    hide_pointer();
    if (row < 1 || row > rows_ || col < 1 || col > cols_) return;
    int available = cols_ - col + 1;
    std::string clipped = clip_to_width(text, available);
    set_cursor(row, col);
    std::cout << decorate_hover(clipped, row, col, hover_area_) << std::flush;
}

void AnsiTerminal::fill_rect(int row, int col, int width, int height, char ch) {
    hide_pointer();
    if (row > rows_ || col > cols_) return;
    int eff_width = std::min(width, cols_ - col + 1);
    int eff_height = std::min(height, rows_ - row + 1);
    if (eff_width <= 0 || eff_height <= 0) return;

    std::cout << color_reset();
    std::string line(eff_width, ch);
    for (int r = 0; r < eff_height; ++r) {
        set_cursor(row + r, col);
        std::cout << line;
    }
    std::cout << std::flush;
}

void AnsiTerminal::draw_box(int row, int col, int width, int height, const std::string& title, bool rounded) {
    hide_pointer();
    if (width < 2 || height < 2) return;
    if (row > rows_ || col > cols_) return;

    std::string tl = "┌";
    std::string tr = "┐";
    std::string bl = "└";
    std::string br = "┘";
    (void)rounded;

    int eff_width = std::min(width, cols_ - col + 1);
    int eff_height = std::min(height, rows_ - row + 1);
    if (eff_width < 2 || eff_height < 2) return;

    // Top border
    set_cursor(row, col);
    std::cout << color_slate() << tl;
    int title_len = visible_width(title);
    if (!title.empty() && eff_width > title_len + 4) {
        std::cout << " " << color_bold() << color_teal() << title << color_reset() << color_slate() << " ";
        for (int c = 0; c < eff_width - title_len - 4; ++c) std::cout << "─";
    } else {
        for (int c = 0; c < eff_width - 2; ++c) std::cout << "─";
    }
    std::cout << tr << color_reset();

    // Side borders
    for (int r = 1; r < eff_height - 1; ++r) {
        set_cursor(row + r, col);
        std::cout << color_slate() << "│" << color_reset();
        set_cursor(row + r, col + eff_width - 1);
        std::cout << color_slate() << "│" << color_reset();
    }

    // Bottom border
    set_cursor(row + eff_height - 1, col);
    std::cout << color_slate() << bl;
    for (int c = 0; c < eff_width - 2; ++c) std::cout << "─";
    std::cout << br << color_reset() << std::flush;
}

std::string AnsiTerminal::color_reset()    { return "\033[0m"; }
std::string AnsiTerminal::color_bold()     { return "\033[1m"; }
std::string AnsiTerminal::color_cyan()     { return "\033[36m"; }
std::string AnsiTerminal::color_green()    { return "\033[32m"; }
std::string AnsiTerminal::color_yellow()   { return "\033[33m"; }
std::string AnsiTerminal::color_red()      { return "\033[31m"; }
std::string AnsiTerminal::color_gray()     { return native_console_colors() ? "\033[37m" : "\033[90m"; }
std::string AnsiTerminal::color_teal()     { return native_console_colors() ? "\033[36m" : "\033[38;2;137;220;235m"; }
std::string AnsiTerminal::color_lavender() { return native_console_colors() ? "\033[35m" : "\033[38;2;203;166;247m"; }
std::string AnsiTerminal::color_gold()     { return native_console_colors() ? "\033[33m" : "\033[38;2;249;226;175m"; }
std::string AnsiTerminal::color_slate()    { return native_console_colors() ? "\033[37m" : "\033[38;2;108;112;134m"; }
std::string AnsiTerminal::bg_blue()        { return "\033[44m"; }
std::string AnsiTerminal::bg_red()         { return "\033[41m"; }
std::string AnsiTerminal::bg_surface()     { return native_console_colors() ? "\033[40m" : "\033[48;2;49;50;68m"; }
std::string AnsiTerminal::bg_teal()        { return native_console_colors() ? "\033[46;30m" : "\033[48;2;137;220;235m\033[38;2;17;17;27m"; }

void AnsiTerminal::update_size() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows_ = ws.ws_row;
        cols_ = ws.ws_col;
    } else if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows_ = ws.ws_row;
        cols_ = ws.ws_col;
    } else {
        rows_ = 24;
        cols_ = 80;
    }
    mouse_.set_bounds(rows_, cols_);
}

bool AnsiTerminal::is_vt() const {
#if defined(__linux__) && defined(KDGETMODE) && defined(KD_TEXT)
    int mode = 0;
    int fd = isatty(STDIN_FILENO) ? STDIN_FILENO : STDOUT_FILENO;
    if (ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_TEXT) {
#if defined(VT_GETSTATE)
        struct vt_stat vs{};
        if (ioctl(fd, VT_GETSTATE, &vs) == 0) {
            const char* tn = ttyname(fd);
            if (tn && std::strncmp(tn, "/dev/tty", 8) == 0 && std::isdigit(tn[8])) {
                int tty_num = std::atoi(tn + 8);
                if (tty_num > 0 && vs.v_active != tty_num) {
                    return false;
                }
            }
        }
#endif
        return true;
    }
#endif
    return false;
}

std::string AnsiTerminal::choose_auto_font(int fb_w, int fb_h, int current_cols, int current_rows, const std::string& current_font) {
    if (fb_w > 0 || fb_h > 0) {
        if ((fb_w >= 2560 && fb_h >= 1536) || (fb_w == 0 && fb_h >= 1536) || (fb_h == 0 && fb_w >= 2560)) {
            return "ter-v32b-double";
        } else if ((fb_w >= 1280 && fb_h >= 768) || (fb_w == 0 && fb_h >= 768) || (fb_h == 0 && fb_w >= 1280)) {
            return "ter-v32b";
        } else if ((fb_w >= 960 && fb_h >= 576) || (fb_w == 0 && fb_h >= 576) || (fb_h == 0 && fb_w >= 960)) {
            return "ter-v24b";
        } else {
            return "ter-v16n";
        }
    }

    int gw = 12;
    int gh = 24;
    if (current_font == "ter-v16n") { gw = 8; gh = 16; }
    else if (current_font == "ter-v24b") { gw = 12; gh = 24; }
    else if (current_font == "ter-v32b") { gw = 16; gh = 32; }
    else if (current_font == "ter-v32b-double") { gw = 32; gh = 64; }

    int est_w = current_cols * gw;
    int est_h = current_rows * gh;
    if (est_w >= 2560 && est_h >= 1536) {
        return "ter-v32b-double";
    } else if (est_w >= 1280 && est_h >= 768) {
        return "ter-v32b";
    } else if (est_w >= 960 && est_h >= 576) {
        return "ter-v24b";
    } else {
        return "ter-v16n";
    }
}

bool AnsiTerminal::set_font(const std::string& font_name) {
    if (!is_vt()) {
        return false;
    }

    static const std::vector<std::string> allowed = {
        "ter-v16n",
        "ter-v24b",
        "ter-v32b",
        "ter-v32b-double"
    };

    if (std::find(allowed.begin(), allowed.end(), font_name) == allowed.end()) {
        return false;
    }

    auto build_font_args = [](const std::string& font) {
        std::vector<std::string> args;
        const char* tty = ttyname(STDIN_FILENO);
        if (!tty) tty = ttyname(STDOUT_FILENO);
        if (tty && std::strncmp(tty, "/dev/tty", 8) == 0) {
            args.push_back("-C");
            args.push_back(tty);
        }
        if (font == "ter-v32b-double") {
            args.push_back("--double");
            args.push_back("ter-v32b");
        } else {
            args.push_back(font);
        }
        return args;
    };

    native_cursor_set_ = false;
    check_mark_.clear();
    ProcessResult res = ProcessRunner::run("setfont", build_font_args(font_name));
    if (!res.success()) {
        return false;
    }

    update_size();
    rows_ = std::max(1, rows_);
    cols_ = std::max(1, cols_);
    if (rows_ < 24 || cols_ < 80) {
        if (!last_good_font_.empty() && last_good_font_ != font_name) {
            ProcessResult rb_res = ProcessRunner::run("setfont", build_font_args(last_good_font_));
            if (rb_res.success()) {
                current_font_ = last_good_font_;
            }
            update_size();
            rows_ = std::max(1, rows_);
            cols_ = std::max(1, cols_);
        }
        return false;
    }

    last_good_font_ = font_name;
    current_font_ = font_name;
    manual_font_set_ = true;
    return true;
}

void AnsiTerminal::font_smaller() {
    if (!is_vt()) return;
    if (current_font_ == "ter-v32b-double") {
        set_font("ter-v32b");
    } else if (current_font_ == "ter-v32b") {
        set_font("ter-v24b");
    } else if (current_font_ == "ter-v24b") {
        set_font("ter-v16n");
    }
}

void AnsiTerminal::font_larger() {
    if (!is_vt()) return;
    if (current_font_ == "ter-v16n") {
        set_font("ter-v24b");
    } else if (current_font_ == "ter-v24b") {
        set_font("ter-v32b");
    } else if (current_font_ == "ter-v32b") {
        set_font("ter-v32b-double");
    }
}

void AnsiTerminal::font_auto() {
    if (!is_vt()) return;

    int fb_w = 0;
    int fb_h = 0;
    std::ifstream fb("/sys/class/graphics/fb0/virtual_size");
    if (fb.is_open()) {
        std::string line;
        if (std::getline(fb, line)) {
            auto comma = line.find(',');
            if (comma != std::string::npos) {
                try {
                    fb_w = std::stoi(line.substr(0, comma));
                    fb_h = std::stoi(line.substr(comma + 1));
                } catch (...) {}
            }
        }
    }

    std::string chosen = choose_auto_font(fb_w, fb_h, cols_, rows_, current_font_);
    if (chosen == current_font_) {
        manual_font_set_ = false;
        return;
    }

    static const std::vector<std::string> candidates = {
        "ter-v32b-double",
        "ter-v32b",
        "ter-v24b",
        "ter-v16n"
    };

    auto it = std::find(candidates.begin(), candidates.end(), chosen);
    if (it == candidates.end()) it = candidates.begin();

    for (; it != candidates.end(); ++it) {
        if (*it == current_font_) {
            break;
        }
        if (set_font(*it)) {
            break;
        }
    }

    manual_font_set_ = false;
}

KeyEvent AnsiTerminal::read_key(int timeout_ms) {
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        if (mouse_.is_enabled() && ever_moved_ && !pointer_visible_) {
            draw_pointer(cursor_row_, cursor_col_);
        }
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
            if (ws.ws_row != rows_ || ws.ws_col != cols_) {
                rows_ = ws.ws_row;
                cols_ = ws.ws_col;
                mouse_.set_bounds(rows_, cols_);
                return KeyEvent{Key::Resize, 0};
            }
        }

        if (timeout_ms == 0) {
            struct pollfd pfd{};
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
                if (mouse_.is_enabled()) {
                    if (auto mev = mouse_.poll_event()) {
                        if (mev->kind == MouseKind::Move) draw_pointer(mev->row, mev->col);
                        return KeyEvent{Key::Mouse, 0, *mev};
                    }
                }
                return KeyEvent{Key::None, 0};
            }
        } else {
            pollfd keyboard{STDIN_FILENO, POLLIN, 0};
            const bool keyboard_ready = poll(&keyboard, 1, 0) > 0 && (keyboard.revents & (POLLIN | POLLHUP));
            if (!keyboard_ready && mouse_.is_enabled()) {
                if (auto mev = mouse_.poll_event()) {
                    if (mev->kind == MouseKind::Move) draw_pointer(mev->row, mev->col);
                    return KeyEvent{Key::Mouse, 0, *mev};
                }
            }
        }

        int slice_ms = 25;
        if (timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= timeout_ms) {
                return KeyEvent{Key::None, 0};
            }
            slice_ms = std::min(slice_ms, static_cast<int>(timeout_ms - elapsed));
        }

        struct pollfd pfd{};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;

        int ret = (timeout_ms == 0) ? 1 : poll(&pfd, 1, slice_ms);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return KeyEvent{Key::None, 0};
        }
        if (ret == 0) {
            continue;
        }

        if (pfd.revents & (POLLERR | POLLNVAL)) {
            return KeyEvent{Key::None, 0};
        }
        if ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) {
            return KeyEvent{Key::None, 0};
        }

        char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            return KeyEvent{Key::None, 0};
        }

        if (c == 27) {
            int esc_timeout = (timeout_ms == 0) ? 0 : 40;
            struct pollfd seq_pfd{};
            seq_pfd.fd = STDIN_FILENO;
            seq_pfd.events = POLLIN;
            if (poll(&seq_pfd, 1, esc_timeout) > 0 && (seq_pfd.revents & POLLIN)) {
                char seq[3] = {0, 0, 0};
                if (read(STDIN_FILENO, &seq[0], 1) == 1) {
                    if (seq[0] == 'O') {
                        char final = 0;
                        if (poll(&seq_pfd, 1, esc_timeout) > 0 && read(STDIN_FILENO, &final, 1) == 1 && final == 'P') return KeyEvent{Key::F1, 0};
                    }
                    if (seq[0] == '[') {
                        if (poll(&seq_pfd, 1, esc_timeout) > 0 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                            switch (seq[1]) {
                            case '[': {
                                char final = 0;
                                if (poll(&seq_pfd, 1, esc_timeout) > 0 && read(STDIN_FILENO, &final, 1) == 1 && final == 'A') return KeyEvent{Key::F1, 0};
                                break;
                            }
                            case 'A': return KeyEvent{Key::Up, 0};
                            case 'B': return KeyEvent{Key::Down, 0};
                            case 'C': return KeyEvent{Key::Right, 0};
                            case 'D': return KeyEvent{Key::Left, 0};
                            case '1': {
                                if (poll(&seq_pfd, 1, esc_timeout) > 0 && read(STDIN_FILENO, &seq[2], 1) == 1) {
                                    if (seq[2] == '1') {
                                        char final = 0;
                                        if (poll(&seq_pfd, 1, esc_timeout) > 0 && read(STDIN_FILENO, &final, 1) == 1 && final == '~') return KeyEvent{Key::F1, 0};
                                    }
                                    if (seq[2] == '7' || seq[2] == '8' || seq[2] == '9') {
                                        if (poll(&seq_pfd, 1, esc_timeout) > 0) {
                                            char t = 0;
                                            (void)read(STDIN_FILENO, &t, 1);
                                        }
                                        if (seq[2] == '7') return KeyEvent{Key::F6, 0};
                                        if (seq[2] == '8') return KeyEvent{Key::F7, 0};
                                        if (seq[2] == '9') return KeyEvent{Key::F8, 0};
                                    }
                                }
                                break;
                            }
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
}

} // namespace litewin

#pragma once

#include <string>
#include <vector>
#include <termios.h>

namespace litewin {

enum class Key {
    None,
    Enter,
    Escape,
    Tab,
    Backspace,
    Up,
    Down,
    Left,
    Right,
    Space,
    Char
};

struct KeyEvent {
    Key key = Key::None;
    char ch = 0;
};

class AnsiTerminal {
public:
    AnsiTerminal();
    ~AnsiTerminal();

    // Terminal mode management
    void enter_raw_mode();
    void exit_raw_mode();
    void hide_cursor();
    void show_cursor();
    void clear();

    // Coordinates & drawing
    void set_cursor(int row, int col);
    void print_at(int row, int col, const std::string& text);
    void draw_box(int row, int col, int width, int height, const std::string& title = "");
    void fill_rect(int row, int col, int width, int height, char ch = ' ');

    // Color helpers
    static std::string color_reset();
    static std::string color_bold();
    static std::string color_cyan();
    static std::string color_green();
    static std::string color_yellow();
    static std::string color_red();
    static std::string color_gray();
    static std::string bg_blue();
    static std::string bg_red();

    // Input handling (timeout_ms: -1 = blocking, 0 = non-blocking, >0 = timeout)
    KeyEvent read_key(int timeout_ms = -1);

    // Terminal dimensions
    [[nodiscard]] int rows() const { return rows_; }
    [[nodiscard]] int cols() const { return cols_; }
    void update_size();

private:
    termios orig_termios_{};
    bool raw_mode_active_ = false;
    int rows_ = 24;
    int cols_ = 80;
};

} // namespace litewin

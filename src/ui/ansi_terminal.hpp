#pragma once

#include <string>
#include <vector>
#include <optional>
#include "ui/tui_layout.hpp"
#include <termios.h>
#include "ui/console_mouse.hpp"

namespace litewin {

enum class Key {
    None,       // 0
    Enter,      // 1
    Escape,     // 2
    Back,       // 3 (CRITICAL: preserved for tests)
    Tab,        // 4
    Backspace,  // 5
    Up,         // 6
    Down,       // 7
    Left,       // 8
    Right,      // 9
    Space,      // 10
    Char,       // 11
    Resize,     // 12 (Appended)
    F6,         // 13 (Appended: Font smaller)
    F7,         // 14 (Appended: Font larger)
    F8,         // 15 (Appended: Font auto)
    Mouse,      // 16 (Appended: VT mouse event)
    Refresh,    // 17 (Appended: redraw)
    F1          // 18: details
};

struct KeyEvent {
    Key key = Key::None;
    char ch = 0;
    MouseEvent mouse{};
};

class AnsiTerminal {
public:
    AnsiTerminal();
    virtual ~AnsiTerminal();

    // Terminal mode management
    void enter_raw_mode();
    void exit_raw_mode();
    void hide_cursor();
    void show_cursor();
    const std::string& check_mark();
    void clear();

    // Coordinates & drawing
    void set_cursor(int row, int col);
    void set_hover_area(std::optional<Rect> area) { hover_area_ = area; }
    static std::string decorate_hover(const std::string& text, int row, int col, std::optional<Rect> area);
    void print_at(int row, int col, const std::string& text);
    void draw_box(int row, int col, int width, int height, const std::string& title = "", bool rounded = true);
    void fill_rect(int row, int col, int width, int height, char ch = ' ');

    // Color helpers
    static std::string color_reset();
    static std::string color_bold();
    static std::string color_cyan();
    static std::string color_green();
    static std::string color_yellow();
    static std::string color_red();
    static std::string color_gray();
    static std::string color_teal();
    static std::string color_lavender();
    static std::string color_gold();
    static std::string color_slate();
    static std::string bg_blue();
    static std::string bg_red();
    static std::string bg_surface();
    static std::string bg_teal();

    // Input handling (timeout_ms: -1 = blocking, 0 = non-blocking, >0 = timeout)
    virtual KeyEvent read_key(int timeout_ms = -1);

    // Terminal dimensions
    [[nodiscard]] int rows() const { return rows_; }
    [[nodiscard]] int cols() const { return cols_; }
    void update_size();

    // Console mouse & pointer management (Linux VT only)
    [[nodiscard]] ConsoleMouse& mouse() noexcept { return mouse_; }
    [[nodiscard]] const ConsoleMouse& mouse() const noexcept { return mouse_; }
    void draw_pointer(int row, int col);
    void hide_pointer();
    [[nodiscard]] int cursor_row() const noexcept { return cursor_row_; }
    [[nodiscard]] int cursor_col() const noexcept { return cursor_col_; }

    // Console font management (Linux VT only)
    [[nodiscard]] bool is_vt() const;
    bool set_font(const std::string& font_name);
    void font_smaller();
    void font_larger();
    void font_auto();
    [[nodiscard]] std::string current_font() const { return current_font_; }
    [[nodiscard]] bool is_manual_font() const { return manual_font_set_; }
    static std::string choose_auto_font(int fb_w, int fb_h, int current_cols = 80, int current_rows = 24, const std::string& current_font = "ter-v24b");

private:
    termios orig_termios_{};
    bool raw_mode_active_ = false;
    int rows_ = 24;
    int cols_ = 80;
    std::string current_font_ = "ter-v24b";
    std::string last_good_font_ = "ter-v24b";
    bool manual_font_set_ = false;
    ConsoleMouse mouse_;
    int cursor_row_ = 1;
    int cursor_col_ = 1;
    bool pointer_visible_ = false;
    bool ever_moved_ = false;
    bool native_cursor_set_ = false;
    std::string check_mark_;
    std::optional<Rect> hover_area_;
};

} // namespace litewin

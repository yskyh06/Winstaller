#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace litewin {

// Calculate visible column width of text, ignoring ANSI escape sequences.
// Single-width UTF-8 glyphs (including box drawing symbols) count as 1 column.
inline int visible_width(std::string_view text) {
    int width = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\033') {
            ++i;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size() && (text[i] < '@' || text[i] > '~')) {
                    ++i;
                }
                if (i < text.size()) ++i;
            }
        } else if ((static_cast<unsigned char>(text[i]) & 0x80) != 0) {
            unsigned char b = static_cast<unsigned char>(text[i]);
            int seq_len = 1;
            if ((b & 0xE0) == 0xC0) seq_len = 2;
            else if ((b & 0xF0) == 0xE0) seq_len = 3;
            else if ((b & 0xF8) == 0xF0) seq_len = 4;
            i += seq_len;
            ++width;
        } else {
            ++width;
            ++i;
        }
    }
    return width;
}

// Clip text to a maximum visible column width without splitting multi-byte UTF-8 or ANSI sequences.
inline std::string clip_to_width(std::string_view text, int max_cols) {
    if (max_cols <= 0) return "";
    std::string result;
    int cur_width = 0;
    std::size_t i = 0;
    bool has_active_style = false;

    while (i < text.size()) {
        if (text[i] == '\033') {
            std::size_t start = i++;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size() && (text[i] < '@' || text[i] > '~')) {
                    ++i;
                }
                if (i < text.size()) ++i;
            }
            result.append(text.substr(start, i - start));
            has_active_style = true;
        } else if ((static_cast<unsigned char>(text[i]) & 0x80) != 0) {
            unsigned char b = static_cast<unsigned char>(text[i]);
            int seq_len = 1;
            if ((b & 0xE0) == 0xC0) seq_len = 2;
            else if ((b & 0xF0) == 0xE0) seq_len = 3;
            else if ((b & 0xF8) == 0xF0) seq_len = 4;

            if (cur_width + 1 > max_cols) break;
            if (i + seq_len <= text.size()) {
                result.append(text.substr(i, seq_len));
                cur_width += 1;
                i += seq_len;
            } else {
                break;
            }
        } else {
            if (cur_width + 1 > max_cols) break;
            result.push_back(text[i]);
            cur_width += 1;
            ++i;
        }
    }

    if (has_active_style) {
        result += "\033[0m";
    }
    return result;
}

// Basic 2D Rect representing viewport and control boundaries
struct Rect {
    int row = 0;
    int col = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr int top() const noexcept { return row; }
    [[nodiscard]] constexpr int left() const noexcept { return col; }
    [[nodiscard]] constexpr int bottom() const noexcept { return row + height - 1; }
    [[nodiscard]] constexpr int right() const noexcept { return col + width - 1; }

    [[nodiscard]] constexpr bool contains(int r, int c) const noexcept {
        return r >= row && r < row + height && c >= col && c < col + width;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return width <= 0 || height <= 0;
    }

    [[nodiscard]] constexpr bool operator==(const Rect& o) const noexcept {
        return row == o.row && col == o.col && width == o.width && height == o.height;
    }
    [[nodiscard]] constexpr bool operator!=(const Rect& o) const noexcept {
        return !(*this == o);
    }
};

// Responsive layout constraints & computed screen geometry
struct ScreenGeometry {
    int term_rows = 24;
    int term_cols = 80;
    bool is_tiny = false;

    Rect header;
    Rect content_box;
    Rect footer;

    static constexpr int kMinCols = 80;
    static constexpr int kMinRows = 24;
    static constexpr int kMaxContentWidth = 90;

    static ScreenGeometry compute(int rows, int cols, int target_w = 76, int target_h = 18, int max_w = kMaxContentWidth) {
        ScreenGeometry g;
        g.term_rows = rows;
        g.term_cols = cols;
        g.is_tiny = (rows < kMinRows || cols < kMinCols);

        if (g.is_tiny) {
            int safe_cols = std::max(0, cols);
            int safe_rows = std::max(0, rows);
            int w = std::max(0, std::min(56, safe_cols));
            int h = std::max(0, std::min(10, safe_rows));
            int r = (safe_rows > h) ? std::max(1, (safe_rows - h) / 2) : 1;
            int c = (safe_cols > w) ? std::max(1, (safe_cols - w) / 2) : 1;
            g.content_box = Rect{r, c, w, h};
            g.header = Rect{safe_rows > 0 ? 1 : 0, safe_cols > 0 ? 1 : 0, safe_cols, safe_rows > 0 ? 1 : 0};
            g.footer = Rect{safe_rows, safe_cols > 0 ? 1 : 0, safe_cols, safe_rows > 0 ? 1 : 0};
            return g;
        }

        int w = std::min(cols - 4, target_w);
        w = std::min(w, max_w);
        w = std::max(w, 56);

        int h_col = std::max(2, (cols - w) / 2);
        g.header = Rect{1, h_col, w, 3};

        int max_h = rows - 4;
        int h = std::min(target_h, max_h);
        h = std::max(h, 14);
        int c_row = 4;
        if (rows > h + 5) {
            c_row = 4 + (rows - 4 - h) / 3;
        }
        g.content_box = Rect{c_row, h_col, w, h};
        g.footer = Rect{rows, 1, cols, 1};

        return g;
    }
};

// Interactive control geometry representations ready for future mouse hit testing
struct ButtonControl {
    int id = 0;
    std::string label;
    Rect bounds;
};

struct TabControl {
    int id = 0;
    std::string label;
    Rect bounds;
};

struct ListItemControl {
    int id = 0;
    std::string label;
    Rect bounds;
};

struct FormFieldControl {
    int id = 0;
    std::string label;
    Rect bounds;
    Rect input_bounds;
};

} // namespace litewin

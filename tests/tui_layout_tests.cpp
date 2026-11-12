#include "ui/tui_layout.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

#define TEST_REQUIRE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while (0)

void test_rect_geometry() {
    litewin::Rect r{5, 10, 40, 15};
    TEST_REQUIRE(r.top() == 5, "Rect top mismatch");
    TEST_REQUIRE(r.left() == 10, "Rect left mismatch");
    TEST_REQUIRE(r.bottom() == 19, "Rect bottom mismatch");
    TEST_REQUIRE(r.right() == 49, "Rect right mismatch");
    TEST_REQUIRE(r.contains(5, 10), "Rect contains top-left");
    TEST_REQUIRE(r.contains(19, 49), "Rect contains bottom-right");
    TEST_REQUIRE(!r.contains(4, 10), "Rect does not contain above");
    TEST_REQUIRE(!r.contains(20, 10), "Rect does not contain below");
    TEST_REQUIRE(!r.empty(), "Rect not empty check");

    litewin::Rect empty_r{0, 0, 0, 10};
    TEST_REQUIRE(empty_r.empty(), "Rect empty check");
    std::cout << "[PASS] test_rect_geometry\n";
}

void test_visible_width_and_clipping() {
    std::string plain = "Hello world";
    TEST_REQUIRE(litewin::visible_width(plain) == 11, "Plain text visible width");

    std::string styled = "\033[1m\033[36mHello \033[32mworld\033[0m";
    TEST_REQUIRE(litewin::visible_width(styled) == 11, "Styled ANSI text visible width ignores escape codes");

    std::string box = "┌─ Hello ─┐";
    TEST_REQUIRE(litewin::visible_width(box) == 11, "UTF-8 single-width box-drawing characters");

    std::string clipped_box = litewin::clip_to_width(box, 5);
    TEST_REQUIRE(litewin::visible_width(clipped_box) == 5, "Clipped box drawing visible width equals 5");

    std::string clipped_styled = litewin::clip_to_width(styled, 5);
    TEST_REQUIRE(litewin::visible_width(clipped_styled) == 5, "Clipped ANSI styled text visible width equals 5");

    std::cout << "[PASS] test_visible_width_and_clipping\n";
}

void test_layout_standard_80x24() {
    auto g = litewin::ScreenGeometry::compute(24, 80);
    TEST_REQUIRE(!g.is_tiny, "80x24 must not trigger is_tiny");
    TEST_REQUIRE(g.content_box.width <= 76, "Content width bounded at 80x24");
    TEST_REQUIRE(g.content_box.height >= 14, "Content height sufficient at 80x24");
    TEST_REQUIRE(g.content_box.row >= 4, "Content placed below header at 80x24");
    TEST_REQUIRE(g.content_box.bottom() <= 23, "Content bottom fits inside 24 rows");
    TEST_REQUIRE(g.header.width <= 76, "Header width bounded at 80x24");
    std::cout << "[PASS] test_layout_standard_80x24\n";
}

void test_layout_tiny_60x20_and_sub_minimum() {
    auto g1 = litewin::ScreenGeometry::compute(1, 1);
    TEST_REQUIRE(g1.is_tiny, "1x1 must trigger is_tiny recovery prompt");
    TEST_REQUIRE(g1.content_box.width <= 1 && g1.content_box.height <= 1, "1x1 content fits screen");
    TEST_REQUIRE(g1.content_box.row >= 0 && g1.content_box.col >= 0, "1x1 coordinates non-negative");

    auto g60 = litewin::ScreenGeometry::compute(20, 60);
    TEST_REQUIRE(g60.is_tiny, "60x20 triggers is_tiny recovery prompt (< 80x24)");
    TEST_REQUIRE(g60.content_box.width <= 56, "60x20 content width fits screen");
    TEST_REQUIRE(g60.content_box.bottom() <= 20, "60x20 content bottom within screen bounds");

    auto g_sub_col = litewin::ScreenGeometry::compute(24, 79);
    TEST_REQUIRE(g_sub_col.is_tiny, "79 cols must trigger is_tiny recovery prompt");

    auto g_sub_row = litewin::ScreenGeometry::compute(23, 80);
    TEST_REQUIRE(g_sub_row.is_tiny, "23 rows must trigger is_tiny recovery prompt");

    auto g_small = litewin::ScreenGeometry::compute(12, 40);
    TEST_REQUIRE(g_small.is_tiny, "40x12 must trigger is_tiny recovery prompt");
    std::cout << "[PASS] test_layout_tiny_60x20_and_sub_minimum\n";
}

void test_layout_medium_100x30_and_wide_120x24() {
    auto g100 = litewin::ScreenGeometry::compute(30, 100);
    TEST_REQUIRE(!g100.is_tiny, "100x30 is not tiny");
    TEST_REQUIRE(g100.content_box.width <= 76, "100x30 content width is preserved");
    TEST_REQUIRE(g100.content_box.col > 2, "100x30 content is centered");

    auto g120 = litewin::ScreenGeometry::compute(24, 120);
    TEST_REQUIRE(!g120.is_tiny, "120x24 is not tiny");
    TEST_REQUIRE(g120.content_box.width <= 90, "120x24 width capped at max 90 cols");
    TEST_REQUIRE(g120.content_box.col >= 15, "120x24 content is horizontally centered");
    std::cout << "[PASS] test_layout_medium_100x30_and_wide_120x24\n";
}

void test_layout_large_160x45_and_ultrawide_320x90() {
    auto g160 = litewin::ScreenGeometry::compute(45, 160);
    TEST_REQUIRE(!g160.is_tiny, "160x45 is not tiny");
    TEST_REQUIRE(g160.content_box.width <= 90, "160x45 content width capped at 90");
    TEST_REQUIRE(g160.content_box.col >= 35, "160x45 content is horizontally centered");

    auto g320 = litewin::ScreenGeometry::compute(90, 320);
    TEST_REQUIRE(!g320.is_tiny, "320x90 is not tiny");
    TEST_REQUIRE(g320.content_box.width <= 90, "320x90 content width capped at 90 to avoid giant stretched box");
    TEST_REQUIRE(g320.content_box.col >= 100, "320x90 content is properly centered on ultrawide display");
    std::cout << "[PASS] test_layout_large_160x45_and_ultrawide_320x90\n";
}

int main() {
    test_rect_geometry();
    test_visible_width_and_clipping();
    test_layout_standard_80x24();
    test_layout_tiny_60x20_and_sub_minimum();
    test_layout_medium_100x30_and_wide_120x24();
    test_layout_large_160x45_and_ultrawide_320x90();
    std::cout << "All TUI layout tests passed!\n";
    return 0;
}

#include "ui/console_mouse.hpp"

#include <linux/input.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#define TEST_REQUIRE(cond) \
    do { \
        if (!(cond)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define TEST_REQUIRE_EQ(actual, expected) \
    do { \
        if ((actual) != (expected)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #actual " == " #expected \
                << " (actual: " << (actual) << ", expected: " << (expected) << ") at " \
                << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

namespace {

struct input_event make_ev(uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ev;
}

void test_relative_movement() {
    litewin::MouseDecoder decoder(24, 80);
    decoder.set_rel_step(1, 1);

    TEST_REQUIRE_EQ(decoder.current_col(), 1);
    TEST_REQUIRE_EQ(decoder.current_row(), 1);

    decoder.feed(make_ev(EV_REL, REL_X, 5));
    decoder.feed(make_ev(EV_REL, REL_Y, 3));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));

    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::Move);
    TEST_REQUIRE_EQ(ev->col, 6);
    TEST_REQUIRE_EQ(ev->row, 4);
    TEST_REQUIRE(!decoder.has_events());

    decoder.feed(make_ev(EV_REL, REL_X, -20));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE_EQ(ev->col, 1);
    TEST_REQUIRE_EQ(ev->row, 4);

    decoder.feed(make_ev(EV_REL, REL_X, 200));
    decoder.feed(make_ev(EV_REL, REL_Y, 200));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE_EQ(ev->col, 80);
    TEST_REQUIRE_EQ(ev->row, 24);
}

void test_relative_accumulation() {
    litewin::MouseDecoder decoder(24, 80);
    decoder.set_rel_step(4, 4);

    decoder.feed(make_ev(EV_REL, REL_X, 2));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(!decoder.has_events());
    TEST_REQUIRE_EQ(decoder.current_col(), 1);

    decoder.feed(make_ev(EV_REL, REL_X, 2));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::Move);
    TEST_REQUIRE_EQ(ev->col, 2);
    TEST_REQUIRE(!decoder.has_events());
}

void test_absolute_movement() {
    litewin::MouseDecoder decoder(25, 80);
    decoder.set_abs_x_info(0, 1000);
    decoder.set_abs_y_info(0, 500);

    decoder.feed(make_ev(EV_ABS, ABS_X, 0));
    decoder.feed(make_ev(EV_ABS, ABS_Y, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE_EQ(decoder.current_col(), 1);
    TEST_REQUIRE_EQ(decoder.current_row(), 1);

    decoder.feed(make_ev(EV_ABS, ABS_X, 1000));
    decoder.feed(make_ev(EV_ABS, ABS_Y, 500));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::Move);
    TEST_REQUIRE_EQ(ev->col, 80);
    TEST_REQUIRE_EQ(ev->row, 25);

    decoder.feed(make_ev(EV_ABS, ABS_X, 500));
    decoder.feed(make_ev(EV_ABS, ABS_Y, 250));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE_EQ(ev->col, 41);
    TEST_REQUIRE_EQ(ev->row, 13);

    decoder.feed(make_ev(EV_ABS, ABS_X, 5000));
    decoder.feed(make_ev(EV_ABS, ABS_Y, -500));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE_EQ(ev->col, 80);
    TEST_REQUIRE_EQ(ev->row, 1);
}

void test_absolute_pixel_interiors() {
    // FHD uses 16x32 cells; 4K uses 32x64. Both have 120x33 cells.
    for (int scale : {1, 2}) {
        litewin::MouseDecoder decoder(33, 120);
        decoder.set_abs_x_info(0, 32767);
        decoder.set_abs_y_info(0, 32767);
        const int x = 950 * scale * 32767LL / (1920 * scale - 1);
        const int y = 368 * scale * 32767LL / (1080 * scale - 1);
        decoder.feed(make_ev(EV_ABS, ABS_X, x));
        decoder.feed(make_ev(EV_ABS, ABS_Y, y));
        decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
        TEST_REQUIRE_EQ(decoder.current_col(), 60);
        TEST_REQUIRE_EQ(decoder.current_row(), 12);
        decoder.resync_state(false, x, y);
        TEST_REQUIRE_EQ(decoder.current_col(), 60);
        TEST_REQUIRE_EQ(decoder.current_row(), 12);
    }
}

void test_right_button_packets_and_resync() {
    litewin::MouseDecoder decoder(24, 80);
    decoder.feed(make_ev(EV_KEY, BTN_RIGHT, 1));
    decoder.feed(make_ev(EV_KEY, BTN_RIGHT, 2)); // repeat is ignored
    decoder.feed(make_ev(EV_KEY, BTN_RIGHT, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev && ev->kind == litewin::MouseKind::RightPress);
    ev = decoder.pop_event();
    TEST_REQUIRE(ev && ev->kind == litewin::MouseKind::RightRelease);
    TEST_REQUIRE(!decoder.has_events());
    decoder.resync_state(false, std::nullopt, std::nullopt, true);
    decoder.feed(make_ev(EV_KEY, BTN_RIGHT, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(!decoder.has_events());
    decoder.feed(make_ev(EV_KEY, BTN_RIGHT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev && ev->kind == litewin::MouseKind::RightPress);
    decoder.set_bounds(30, 100);
    decoder.feed(make_ev(EV_KEY, BTN_RIGHT, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(!decoder.has_events());
}

void test_shared_position_seed_keeps_buttons() {
    litewin::MouseDecoder pointer(33, 120), buttons(33, 120);
    pointer.set_abs_x_info(0, 32767); pointer.set_abs_y_info(0, 32767);
    pointer.feed(make_ev(EV_ABS, ABS_X, 16000));
    pointer.feed(make_ev(EV_ABS, ABS_Y, 11000));
    pointer.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    buttons.set_position(pointer.current_row(), pointer.current_col());
    buttons.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    buttons.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = buttons.pop_event();
    TEST_REQUIRE(ev && ev->kind == litewin::MouseKind::LeftPress);
    TEST_REQUIRE_EQ(ev->row, pointer.current_row());
    TEST_REQUIRE_EQ(ev->col, pointer.current_col());
    buttons.set_position(20, 60); // A batch on another device must not clear held state.
    buttons.feed(make_ev(EV_KEY, BTN_LEFT, 0));
    buttons.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = buttons.pop_event();
    TEST_REQUIRE(ev && ev->kind == litewin::MouseKind::LeftRelease);
    TEST_REQUIRE_EQ(ev->row, 20);
    TEST_REQUIRE_EQ(ev->col, 60);
}

void test_wheel() {
    litewin::MouseDecoder decoder(24, 80);

    decoder.feed(make_ev(EV_REL, REL_WHEEL, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::Wheel);
    TEST_REQUIRE_EQ(ev->wheel, 1);

    decoder.feed(make_ev(EV_REL, REL_WHEEL, -1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::Wheel);
    TEST_REQUIRE_EQ(ev->wheel, -1);
}

void test_press_release_and_duplicate() {
    litewin::MouseDecoder decoder(24, 80);

    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::LeftPress);
    TEST_REQUIRE(decoder.is_button_down());

    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(!decoder.has_events());
    TEST_REQUIRE(decoder.is_button_down());

    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::LeftRelease);
    TEST_REQUIRE(!decoder.is_button_down());

    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(!decoder.has_events());
}

void test_syn_dropped_and_resync() {
    litewin::MouseDecoder decoder(24, 80);

    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value() && ev->kind == litewin::MouseKind::LeftPress);

    // Feed SYN_DROPPED: must clear old queue and enter dropping state
    decoder.feed(make_ev(EV_SYN, SYN_DROPPED, 0));
    TEST_REQUIRE(!decoder.needs_resync());
    ev = decoder.pop_event();
    TEST_REQUIRE(ev && ev->kind == litewin::MouseKind::Cancel);

    // Feeds during drop must be ignored
    decoder.feed(make_ev(EV_REL, REL_X, 10));
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    TEST_REQUIRE(!decoder.needs_resync());
    TEST_REQUIRE(!decoder.has_events());

    // Next SYN_REPORT completes drop boundary; signals needs_resync
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(decoder.needs_resync());
    TEST_REQUIRE(!decoder.has_events());

    // Resync called by wrapper after boundary
    decoder.resync_state(true, std::nullopt, std::nullopt);
    TEST_REQUIRE(!decoder.needs_resync());
    TEST_REQUIRE(!decoder.has_events()); // Must not invent clicks

    // Physical release of held button must be suppressed
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(!decoder.has_events());

    // Fresh press works
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value() && ev->kind == litewin::MouseKind::LeftPress);
}

void test_screen_resize() {
    litewin::MouseDecoder decoder(24, 80);
    decoder.set_rel_step(1, 1);

    decoder.feed(make_ev(EV_REL, REL_X, 70));
    decoder.feed(make_ev(EV_REL, REL_Y, 20));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    decoder.pop_event();

    TEST_REQUIRE_EQ(decoder.current_col(), 71);
    TEST_REQUIRE_EQ(decoder.current_row(), 21);

    // Queue click before resize
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(decoder.has_events());

    // Same-size bounds call must NOT clear events
    decoder.set_bounds(24, 80);
    TEST_REQUIRE(decoder.has_events());

    // Resize screen: must clear queued events and clamp
    decoder.set_bounds(15, 40);
    TEST_REQUIRE(!decoder.has_events());
    TEST_REQUIRE_EQ(decoder.current_col(), 40);
    TEST_REQUIRE_EQ(decoder.current_row(), 15);

    // Releasing the held button after resize must be suppressed
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(!decoder.has_events());

    // Next press works
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value() && ev->kind == litewin::MouseKind::LeftPress);
}

void test_reset_drain_and_suppress() {
    litewin::MouseDecoder decoder(24, 80);

    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(decoder.has_events());

    // Reset with button physically down
    decoder.reset(true);
    TEST_REQUIRE(!decoder.has_events());

    // Release must be suppressed
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 0));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    TEST_REQUIRE(!decoder.has_events());

    // Next press succeeds
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    auto ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::LeftPress);

    // Reset with button physically released
    decoder.reset(false);
    TEST_REQUIRE(!decoder.has_events());
    decoder.feed(make_ev(EV_KEY, BTN_LEFT, 1));
    decoder.feed(make_ev(EV_SYN, SYN_REPORT, 0));
    ev = decoder.pop_event();
    TEST_REQUIRE(ev.has_value());
    TEST_REQUIRE(ev->kind == litewin::MouseKind::LeftPress);
}

} // namespace

int main() {
    try {
        test_relative_movement();
        test_relative_accumulation();
        test_absolute_movement();
        test_absolute_pixel_interiors();
        test_right_button_packets_and_resync();
        test_shared_position_seed_keeps_buttons();
        test_wheel();
        test_press_release_and_duplicate();
        test_syn_dropped_and_resync();
        test_screen_resize();
        test_reset_drain_and_suppress();
        std::cout << "All console mouse synthetic tests passed successfully.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed with exception: " << ex.what() << std::endl;
        return 1;
    }
}

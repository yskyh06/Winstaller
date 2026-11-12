#include "ui/mouse_controls.hpp"
#include "ui/winstaller_tui.hpp"

#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#define REQUIRE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "[REQUIRE FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " (" #cond ")" << std::endl; \
            std::exit(1); \
        } \
    } while (false)

namespace litewin {

class ScriptedTerminal : public AnsiTerminal {
public:
    std::function<KeyEvent(ScriptedTerminal&)> key_hook;
    std::deque<KeyEvent> event_queue;

    KeyEvent read_key(int timeout_ms = -1) override {
        (void)timeout_ms;
        if (key_hook) {
            return key_hook(*this);
        }
        if (!event_queue.empty()) {
            auto ev = event_queue.front();
            event_queue.pop_front();
            return ev;
        }
        return KeyEvent{Key::None, 0};
    }
};

}

using namespace litewin;

static void test_leave_and_return() {
    MouseControls mc;
    mc.register_area(10, 5, 5, 10, 2, 42);

    MouseEvent press_ev{};
    press_ev.kind = MouseKind::LeftPress;
    press_ev.row = 5; press_ev.col = 5;
    auto hit = mc.process_event(press_ev);
    REQUIRE(!hit.is_click());
    REQUIRE(mc.armed_id() == 10);

    // Motion leave
    MouseEvent leave_ev{};
    leave_ev.kind = MouseKind::Move;
    leave_ev.row = 1; leave_ev.col = 1;
    hit = mc.process_event(leave_ev);
    REQUIRE(!hit.is_click());
    REQUIRE(mc.armed_id() == -1);

    // Return
    MouseEvent return_ev{};
    return_ev.kind = MouseKind::Move;
    return_ev.row = 5; return_ev.col = 5;
    hit = mc.process_event(return_ev);
    REQUIRE(hit.is_hover());
    REQUIRE(mc.armed_id() == -1); // remains disarmed after leaving

    // Release
    MouseEvent rel_ev{};
    rel_ev.kind = MouseKind::LeftRelease;
    rel_ev.row = 5; rel_ev.col = 5;
    hit = mc.process_event(rel_ev);
    REQUIRE(!hit.is_click());
}

static void test_duplicate_press() {
    MouseControls mc;
    mc.register_area(10, 5, 5, 10, 2, 42);

    MouseEvent p1{}; p1.kind = MouseKind::LeftPress; p1.row = 5; p1.col = 5;
    mc.process_event(p1);
    REQUIRE(mc.armed_id() == 10);

    MouseEvent p2{}; p2.kind = MouseKind::LeftPress; p2.row = 5; p2.col = 6;
    mc.process_event(p2);
    REQUIRE(mc.armed_id() == 10);

    MouseEvent rel{}; rel.kind = MouseKind::LeftRelease; rel.row = 5; rel.col = 6;
    auto hit = mc.process_event(rel);
    REQUIRE(hit.is_click());
    REQUIRE(hit.control_id == 10);
    REQUIRE(hit.data == 42);
}

static void test_generation_invalidation() {
    MouseControls mc;
    mc.register_area(10, 5, 5, 10, 2, 42);

    MouseEvent press{}; press.kind = MouseKind::LeftPress; press.row = 5; press.col = 5;
    mc.process_event(press);
    REQUIRE(mc.armed_id() == 10);

    mc.advance_generation();
    REQUIRE(mc.armed_id() == -1);

    MouseEvent rel{}; rel.kind = MouseKind::LeftRelease; rel.row = 5; rel.col = 5;
    auto hit = mc.process_event(rel);
    REQUIRE(!hit.is_click());
}

static void test_resize_invalidation() {
    MouseControls mc;
    mc.register_area(10, 5, 5, 10, 2, 42);

    MouseEvent press{}; press.kind = MouseKind::LeftPress; press.row = 5; press.col = 5;
    mc.process_event(press);
    REQUIRE(mc.armed_id() == 10);

    mc.on_resize();
    REQUIRE(mc.armed_id() == -1);
    REQUIRE(mc.areas().empty());

    MouseEvent rel{}; rel.kind = MouseKind::LeftRelease; rel.row = 5; rel.col = 5;
    auto hit = mc.process_event(rel);
    REQUIRE(!hit.is_click());
}

static void test_wheel_invalidation() {
    MouseControls mc;
    mc.register_area(10, 5, 5, 10, 2, 42);

    MouseEvent press{}; press.kind = MouseKind::LeftPress; press.row = 5; press.col = 5;
    mc.process_event(press);
    REQUIRE(mc.armed_id() == 10);

    MouseEvent wheel{}; wheel.kind = MouseKind::Wheel; wheel.row = 5; wheel.col = 5; wheel.wheel = 1;
    auto hit_w = mc.process_event(wheel);
    REQUIRE(hit_w.is_wheel());
    REQUIRE(!hit_w.is_click());
    REQUIRE(mc.armed_id() == -1);

    MouseEvent rel{}; rel.kind = MouseKind::LeftRelease; rel.row = 5; rel.col = 5;
    auto hit = mc.process_event(rel);
    REQUIRE(!hit.is_click());
}

static void test_hover_noactivate() {
    MouseControls mc;
    mc.register_area(10, 5, 5, 10, 2, 42);

    MouseEvent mv{}; mv.kind = MouseKind::Move; mv.row = 5; mv.col = 5;
    auto hit = mc.process_event(mv);
    REQUIRE(hit.is_hover());
    REQUIRE(!hit.is_click());
    REQUIRE(hit.control_id == 10);
}

static void test_wheel_noactivate() {
    MouseControls mc;
    mc.register_area(10, 5, 5, 10, 2, 42);

    MouseEvent wh{}; wh.kind = MouseKind::Wheel; wh.row = 5; wh.col = 5; wh.wheel = -1;
    auto hit = mc.process_event(wh);
    REQUIRE(hit.is_wheel());
    REQUIRE(!hit.is_click());
    REQUIRE(hit.wheel_delta == -1);
}

static void test_area_id_and_data_mismatch_on_release() {
    MouseControls mc;
    mc.register_area(10, 5, 5, 10, 2, 100);

    MouseEvent press{}; press.kind = MouseKind::LeftPress; press.row = 5; press.col = 5;
    mc.process_event(press);
    REQUIRE(mc.armed_id() == 10);
    REQUIRE(mc.armed_data() == 100);

    // Area layout changes under same coordinates (data changed from 100 to 200)
    mc.clear();
    mc.register_area(10, 5, 5, 10, 2, 200);

    MouseEvent rel{}; rel.kind = MouseKind::LeftRelease; rel.row = 5; rel.col = 5;
    auto hit = mc.process_event(rel);
    REQUIRE(!hit.is_click());
}

static void test_ui_show_edition_modal() {
    ScriptedTerminal term;
    WinstallerTui tui(term);
    TuiEditionChoice choice;
    choice.os_tab = "Windows";
    choice.edition_index = 4; // Home initially
    choice.edition_name = "Windows 11 Home";

    std::vector<int> target_controls = {301, 1}; // Pro item (301), then Select button (1)
    std::size_t step = 0;
    bool press_phase = true;

    term.key_hook = [&](ScriptedTerminal&) -> KeyEvent {
        if (step >= target_controls.size()) {
            return KeyEvent{Key::Escape, 0};
        }
        int target_id = target_controls[step];
        const auto& areas = tui.mouse_controls().areas();
        const RegisteredHitArea* target_area = nullptr;
        for (const auto& a : areas) {
            if (a.id == target_id) {
                target_area = &a;
                break;
            }
        }
        if (!target_area) {
            throw std::runtime_error("Target control not found: " + std::to_string(target_id));
        }
        int r = target_area->bounds.row + target_area->bounds.height / 2;
        int c = target_area->bounds.col + target_area->bounds.width / 2;

        MouseEvent mev{};
        mev.row = r;
        mev.col = c;
        if (press_phase) {
            mev.kind = MouseKind::LeftPress;
            press_phase = false;
            return KeyEvent{Key::Mouse, 0, mev};
        } else {
            mev.kind = MouseKind::LeftRelease;
            press_phase = true;
            step++;
            return KeyEvent{Key::Mouse, 0, mev};
        }
    };

    tui.show_edition_modal(choice);
    REQUIRE(choice.edition_index == 6);
    REQUIRE(choice.edition_name == "Windows 11 Pro");
}

static void test_ui_prompt_user_account() {
    ScriptedTerminal term;
    WinstallerTui tui(term);
    TuiUserData user_data;
    user_data.username = "tester";
    user_data.password = "secret123";
    user_data.password_confirm = "secret123";
    user_data.computer_name = "test-pc";

    std::vector<int> target_controls = {1}; // Start Installation button (1)
    std::size_t step = 0;
    bool press_phase = true;

    term.key_hook = [&](ScriptedTerminal&) -> KeyEvent {
        if (step >= target_controls.size()) {
            return KeyEvent{Key::Escape, 0};
        }
        int target_id = target_controls[step];
        const auto& areas = tui.mouse_controls().areas();
        const RegisteredHitArea* target_area = nullptr;
        for (const auto& a : areas) {
            if (a.id == target_id) {
                target_area = &a;
                break;
            }
        }
        if (!target_area) {
            throw std::runtime_error("Target control not found: " + std::to_string(target_id));
        }
        int r = target_area->bounds.row + target_area->bounds.height / 2;
        int c = target_area->bounds.col + target_area->bounds.width / 2;

        MouseEvent mev{};
        mev.row = r;
        mev.col = c;
        if (press_phase) {
            mev.kind = MouseKind::LeftPress;
            press_phase = false;
            return KeyEvent{Key::Mouse, 0, mev};
        } else {
            mev.kind = MouseKind::LeftRelease;
            press_phase = true;
            step++;
            return KeyEvent{Key::Mouse, 0, mev};
        }
    };

    ScreenAction action = tui.prompt_user_account(user_data, true);
    REQUIRE(action == ScreenAction::Next);
    REQUIRE(user_data.finalized == true);
}

static KeyEvent at_control(WinstallerTui& ui, int id, MouseKind kind, int wheel = 0) {
    const auto* a = ui.mouse_controls().find_area(id);
    if (!a) throw std::runtime_error("Missing control " + std::to_string(id));
    return {Key::Mouse, 0, {kind, a->bounds.row, a->bounds.col + a->bounds.width / 2, wheel}};
}

static void test_hover_keeps_keyboard_selection() {
    ScriptedTerminal term;
    WinstallerTui ui(term);
    TuiEditionChoice choice;
    int step = 0;
    term.key_hook = [&](ScriptedTerminal&) {
        REQUIRE(step < 6);
        if (step++ == 0) return at_control(ui, 301, MouseKind::Move);
        return KeyEvent{Key::Enter};
    };
    ui.show_edition_modal(choice);
    REQUIRE(choice.edition_index == 4);
}

static void test_hover_keeps_account_focus() {
    ScriptedTerminal term;
    WinstallerTui ui(term);
    WindowsSettingsSession session;
    ui.configure_progress(&session);
    TuiUserData data;
    data.username = "tester";
    int step = 0;
    term.key_hook = [&](ScriptedTerminal&) {
        REQUIRE(step < 6);
        if (step++ == 0) return at_control(ui, 11, MouseKind::Move);
        if (step == 2) return KeyEvent{Key::Char, 'z'};
        return KeyEvent{Key::Escape};
    };
    ui.prompt_user_account(data, false);
    REQUIRE(data.username == "testerz");
    REQUIRE(data.password.empty());
}

static void test_context_and_f1_preserve_choice() {
    for (const bool keyboard : {false, true}) {
        ScriptedTerminal term;
        WinstallerTui ui(term);
        TuiEditionChoice choice;
        int step = 0;
        term.key_hook = [&](ScriptedTerminal&) {
            REQUIRE(step < 9);
            const int s = step++;
            if (s == 0) return at_control(ui, 301, MouseKind::Move);
            if (keyboard) {
                if (s == 1) return KeyEvent{Key::F1};
                if (s == 2) { REQUIRE(ui.last_details_key() == "os.item.win11-home"); return KeyEvent{Key::Escape}; }
            } else {
                if (s == 1) return at_control(ui, 301, MouseKind::RightPress);
                if (s == 2) return at_control(ui, 301, MouseKind::RightRelease);
                if (s == 3) { REQUIRE(ui.last_details_key() == "os.item.win11-pro"); return KeyEvent{Key::Escape}; }
            }
            return KeyEvent{Key::Enter};
        };
        ui.show_edition_modal(choice);
        REQUIRE(choice.edition_index == 4);
        REQUIRE(choice.edition_name == "Windows 11 Home");
    }
}

static void test_wheel_scroll_without_selecting() {
    ScriptedTerminal term;
    WinstallerTui ui(term);
    TuiEditionChoice choice;
    std::vector<std::pair<std::string, std::string>> languages;
    for (int i = 0; i < 20; ++i) languages.emplace_back("code" + std::to_string(i), "Language " + std::to_string(i));
    int selected = 0, step = 0;
    term.key_hook = [&](ScriptedTerminal&) {
        REQUIRE(step < 7);
        const int s = step++;
        if (s == 0) return at_control(ui, 300, MouseKind::Wheel, -8);
        REQUIRE(ui.mouse_controls().find_area(308));
        if (s == 1) return at_control(ui, 1, MouseKind::LeftPress);
        if (s == 2) return at_control(ui, 1, MouseKind::LeftRelease);
        return KeyEvent{Key::Escape};
    };
    ui.show_language_modal(choice, selected, languages);
    REQUIRE(selected == 0);
    REQUIRE(choice.language == "code0");
}

static void test_mixed_buttons_and_input_cancel() {
    MouseControls mc;
    mc.register_area(1, 5, 5, 20, 1);
    auto event = [&](MouseKind kind) { return mc.process_event({kind, 5, 6, 0}); };
    event(MouseKind::LeftPress);
    event(MouseKind::RightPress);
    REQUIRE(!event(MouseKind::LeftRelease).is_click());
    REQUIRE(!event(MouseKind::RightRelease).is_context());
    event(MouseKind::RightPress);
    REQUIRE(event(MouseKind::RightRelease).is_context());
    event(MouseKind::LeftPress);
    event(MouseKind::Cancel);
    REQUIRE(!event(MouseKind::LeftRelease).is_click());
    event(MouseKind::LeftPress);
    REQUIRE(event(MouseKind::LeftRelease).is_click());
    event(MouseKind::Move);
    REQUIRE(mc.hover_area());
    mc.clear(); // Repainting regions must not clear hover.
    mc.register_area(1, 5, 5, 20, 1);
    REQUIRE(mc.hover_area());
    mc.process_event({MouseKind::Move, 1, 1, 0});
    REQUIRE(!mc.hover_area());
}

static void test_progress_details_are_nonblocking() {
    ScriptedTerminal term;
    WinstallerTui ui(term);
    WindowsSettingsSession session;
    ui.configure_progress(&session);
    TuiUserData data;
    data.username = "tester";
    int field = 0;
    ui.render_progress("Download", 10, 10, "Working", data, field);
    term.event_queue = {{Key::F1}, {Key::None}};
    REQUIRE(ui.handle_user_data_input(data, field));
    REQUIRE(ui.last_details_key() == "progress.settings");
    ui.render_progress("Download", 20, 20, "Still working", data, field);
    REQUIRE(ui.mouse_controls().find_area(9000));
    term.event_queue = {{Key::Char, 'x'}, {Key::Escape}, {Key::None}};
    REQUIRE(ui.handle_user_data_input(data, field));
    REQUIRE(data.username == "tester");
    REQUIRE(field == 0);
    ui.render_progress("Download", 30, 30, "Continued", data, field);
    REQUIRE(ui.mouse_controls().find_area(6000));
    REQUIRE(!ui.mouse_controls().find_area(9000));
}

static void test_details_cannot_confirm_disk_wipe() {
    ScriptedTerminal term;
    WinstallerTui ui(term);
    DiskInfo disk{};
    disk.path = "/dev/test-only"; disk.name = "test-only"; disk.type = "disk";
    disk.size_bytes = 64ULL * 1024 * 1024 * 1024;
    disk.children.push_back(DiskInfo{});
    int step = 0;
    term.key_hook = [&](ScriptedTerminal&) {
        REQUIRE(step < 12);
        const int s = step++;
        if (s == 0) return KeyEvent{Key::Enter}; // Open existing-partition confirmation.
        if (s == 1) return at_control(ui, 1, MouseKind::RightPress);
        if (s == 2) return at_control(ui, 1, MouseKind::RightRelease);
        if (s == 3) {
            REQUIRE(ui.last_details_key() == "disk-wipe.confirm");
            return KeyEvent{Key::Escape}; // Close details, no wipe confirmation.
        }
        if (s == 4) return KeyEvent{Key::Escape}; // Cancel wipe.
        return KeyEvent{Key::Escape}; // Exit disk selection.
    };
    const auto result = ui.prompt_disk_selection({disk});
    REQUIRE(result.first == ScreenAction::Exit);
    REQUIRE(result.second.empty());
}

int main() {
    try {
        test_details_cannot_confirm_disk_wipe();
        test_hover_keeps_keyboard_selection();
        test_hover_keeps_account_focus();
        test_context_and_f1_preserve_choice();
        test_wheel_scroll_without_selecting();
        test_mixed_buttons_and_input_cancel();
        test_progress_details_are_nonblocking();
        test_leave_and_return();
        test_duplicate_press();
        test_generation_invalidation();
        test_resize_invalidation();
        test_wheel_invalidation();
        test_hover_noactivate();
        test_wheel_noactivate();
        test_area_id_and_data_mismatch_on_release();
        test_ui_show_edition_modal();
        test_ui_prompt_user_account();
        std::cout << "All mouse controls and UI tests passed successfully!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}

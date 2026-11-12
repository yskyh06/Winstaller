#include "ui/winstaller_tui.hpp"
#include "core/linux_catalog.hpp"
#include "platform/network_inspector.hpp"
#include "platform/network_probe.hpp"
#include "platform/network_configurator.hpp"
#include "platform/process_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace litewin {

WinstallerTui::WinstallerTui(AnsiTerminal& term) : term_(term) {}

DetailsContent WinstallerTui::details_for(const std::string& key) {
    // A stable key is the extension point for future local descriptions.
    return DetailsContent{key, "Details", "Details coming soon."};
}

void WinstallerTui::draw_details_overlay(const DetailsContent& content) {
    const int w = std::min(60, term_.cols() - 4);
    const int h = std::min(9, term_.rows() - 2);
    const int row = std::max(1, (term_.rows() - h) / 2);
    const int col = std::max(1, (term_.cols() - w) / 2);
    term_.set_hover_area(mouse_controls_.hover_area());
    term_.fill_rect(row, col, w, h, ' ');
    term_.draw_box(row, col, w, h, content.title, true);
    term_.print_at(row + 2, col + 4, content.body);
    const std::string close = " [ Close (Enter / Esc) ] ";
    term_.print_at(row + h - 2, col + 4, AnsiTerminal::bg_teal() + close + AnsiTerminal::color_reset());
    mouse_controls_.clear();
    mouse_controls_.register_area(9000, row + h - 2, col + 4, static_cast<int>(close.size()), 1, 0, "details.close");
}

void WinstallerTui::show_details(const DetailsContent& content) {
    auto saved = mouse_controls_;
    {
        MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "details");
        while (true) {
            if (!show_recovery_prompt()) break;
            term_.clear(); // A standalone details view must not leave fragments of its parent.
            draw_details_overlay(content);
            const auto ev = term_.read_key(-1);
            if (ev.key == Key::None || ev.key == Key::Escape || ev.key == Key::Enter) break;
            if (handle_global_key(ev.key)) continue;
            if (ev.key == Key::Mouse) {
                const auto hit = mouse_controls_.process_event(ev.mouse);
                if (hit.is_click() && hit.control_id == 9000) break;
            }
        }
    }
    saved.advance_generation(); // Restore regions, never a pending press or hover.
    mouse_controls_ = std::move(saved);
    term_.set_hover_area(std::nullopt);
}

KeyEvent WinstallerTui::read_control_key(int timeout_ms, int focused_id, const std::string& focused_key) {
    const auto ev = term_.read_key(timeout_ms);
    if (progress_details_) {
        bool close = ev.key == Key::Escape || ev.key == Key::Enter;
        if (ev.key == Key::Mouse) {
            const auto hit = mouse_controls_.process_event(ev.mouse);
            close = hit.is_click() && hit.control_id == 9000;
        }
        if (close) {
            progress_details_.reset();
            mouse_controls_.advance_generation(); mouse_controls_.clear(); term_.mouse().reset();
            term_.set_hover_area(std::nullopt);
            return {Key::Refresh};
        }
        if (ev.key == Key::Resize || ev.key == Key::F6 || ev.key == Key::F7 || ev.key == Key::F8) return ev;
        return ev.key == Key::None ? ev : KeyEvent{Key::Refresh};
    }
    std::string details_key;
    if (ev.key == Key::F1) {
        const auto* area = mouse_controls_.find_area(focused_id);
        details_key = focused_key.empty() ? (area ? area->details_key : "") : focused_key;
    } else if (ev.key == Key::Mouse && (ev.mouse.kind == MouseKind::RightPress || ev.mouse.kind == MouseKind::RightRelease)) {
        const auto hit = mouse_controls_.process_event(ev.mouse);
        if (hit.is_context()) {
            const auto* area = mouse_controls_.find_area(hit.control_id);
            if (area) details_key = area->details_key;
        }
        if (details_key.empty()) return {Key::Refresh};
    } else {
        if (ev.key != Key::None && ev.key != Key::Mouse) {
            mouse_controls_.advance_generation();
            term_.set_hover_area(std::nullopt);
        }
        return ev;
    }
    if (details_key.empty()) return {Key::Refresh};
    last_details_key_ = details_key;
    const auto content = details_for(details_key);
    if (timeout_ms == 0) {
        progress_details_ = content;
        mouse_controls_.advance_generation(); mouse_controls_.clear(); term_.mouse().reset();
    } else {
        show_details(content);
    }
    return {Key::Refresh};
}

void WinstallerTui::draw_footer() {
    int r = term_.rows();
    std::string footer_text = progress_initialized_
        ? " F6: Font- | F7: Font+ | F8: Auto | Installation continues"
        : " F6: Font- | F7: Font+ | F8: Auto | F1: Details | Esc: Exit";
    if (term_.cols() < 80) {
        footer_text = " F6:- F7:+ Esc:Exit";
    }
    term_.print_at(r, 2, AnsiTerminal::color_slate() + footer_text + AnsiTerminal::color_reset());
}

bool WinstallerTui::handle_global_key(Key key) {
    if (key == Key::Refresh) {
        return true;
    } else if (key == Key::Resize) {
        term_.update_size();
        mouse_controls_.on_resize();
        if (!term_.is_manual_font()) {
            term_.font_auto();
        }
        term_.clear();
        return true;
    } else if (key == Key::F6) {
        mouse_controls_.advance_generation();
        term_.font_smaller();
        term_.clear();
        return true;
    } else if (key == Key::F7) {
        mouse_controls_.advance_generation();
        term_.font_larger();
        term_.clear();
        return true;
    } else if (key == Key::F8) {
        mouse_controls_.advance_generation();
        term_.font_auto();
        term_.clear();
        return true;
    }
    return false;
}

bool WinstallerTui::show_recovery_prompt() {
    term_.set_hover_area(mouse_controls_.hover_area());
    term_.update_size();
    while (term_.cols() < 80 || term_.rows() < 24) {
        term_.clear();
        int r = std::max(1, (term_.rows() - 6) / 2);
        int c = std::max(1, (term_.cols() - 56) / 2);
        term_.print_at(r, c, AnsiTerminal::color_red() + AnsiTerminal::color_bold() +
            "[!] Terminal Too Small: " + std::to_string(term_.cols()) + "x" + std::to_string(term_.rows()) +
            AnsiTerminal::color_reset());
        term_.print_at(r + 2, c, "Minimum required terminal size is 80x24.");
        term_.print_at(r + 3, c, "Please enlarge window or press F6 for smaller font.");
        term_.print_at(r + 5, c, AnsiTerminal::color_slate() + "F6: Font- | F7: Font+ | F8: Auto | Esc: Exit" + AnsiTerminal::color_reset());

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Escape || ev.key == Key::None) {
            return false;
        }
        if (handle_global_key(ev.key)) {
            term_.update_size();
        }
    }
    return true;
}

void WinstallerTui::draw_header(const std::string& subtitle) {
    term_.set_hover_area(mouse_controls_.hover_area());
    term_.clear();
    auto geom = ScreenGeometry::compute(term_.rows(), term_.cols(), 76, 18, 90);
    int col = geom.header.col;
    int w = geom.header.width;
    term_.draw_box(1, col, w, 3, "Winstaller", true);

    std::string main_title = "* Winstaller - Automated OS Deployment Tool";
    if (w < 50) main_title = "* Winstaller";
    term_.print_at(2, col + 3, AnsiTerminal::color_bold() + AnsiTerminal::color_teal() + main_title + AnsiTerminal::color_reset());

    if (!subtitle.empty()) {
        int sub_col = col + w - static_cast<int>(subtitle.size()) - 3;
        if (sub_col > col + static_cast<int>(main_title.size()) + 4) {
            term_.print_at(2, sub_col, AnsiTerminal::color_slate() + subtitle + AnsiTerminal::color_reset());
        }
    }
    draw_footer();
}

ScreenAction WinstallerTui::prompt_eula() {
    progress_initialized_ = false;
    progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "eula");
    int selected = 0; // 0 = Agree, 1 = Exit
    while (true) {
        if (!show_recovery_prompt()) return ScreenAction::Exit;
        draw_header("Disclaimer & EULA");

        auto geom = ScreenGeometry::compute(term_.rows(), term_.cols(), 76, 18, 90);
        int row = geom.content_box.row;
        int col = geom.content_box.col;
        int w = geom.content_box.width;
        int h = geom.content_box.height;

        term_.draw_box(row, col, w, h, "Disclaimer & Terms of Use", true);

        term_.print_at(row + 2, col + 4, AnsiTerminal::color_bold() + AnsiTerminal::color_gold() + "Notice & Liability Agreement" + AnsiTerminal::color_reset());
        term_.print_at(row + 4, col + 4, "1. Winstaller is an automated zero-touch deployment utility.");
        term_.print_at(row + 5, col + 4, "2. ALL DATA, partitions, and files on the target drive will be");
        term_.print_at(row + 6, col + 4, "   PERMANENTLY DESTROYED and replaced with chosen OS.");
        term_.print_at(row + 7, col + 4, "3. The authors assume NO LIABILITY for data loss or hardware issues.");
        term_.print_at(row + 9, col + 4, "Ensure all essential data is backed up before continuing.");
        term_.print_at(row + 11, col + 4, AnsiTerminal::color_lavender() + "Do you accept these terms and wish to proceed?" + AnsiTerminal::color_reset());

        int btn_row = row + h - 3;
        int btn1_col = col + 6;
        int btn2_col = col + std::max(36, w - 30);
        std::string b1_s = " [ Agree & Continue (Enter) ] ";
        std::string b2_s = " [ Exit & Reboot (Esc) ] ";

        if (selected == 0) {
            term_.print_at(btn_row, btn1_col, AnsiTerminal::bg_teal() + b1_s + AnsiTerminal::color_reset());
            term_.print_at(btn_row, btn2_col, AnsiTerminal::color_slate() + b2_s + AnsiTerminal::color_reset());
        } else {
            term_.print_at(btn_row, btn1_col, AnsiTerminal::color_slate() + b1_s + AnsiTerminal::color_reset());
            term_.print_at(btn_row, btn2_col, AnsiTerminal::bg_red() + AnsiTerminal::color_bold() + b2_s + AnsiTerminal::color_reset());
        }

        mouse_controls_.clear();
        mouse_controls_.register_area(1, btn_row, btn1_col, static_cast<int>(b1_s.size()), 1);
        mouse_controls_.register_area(2, btn_row, btn2_col, static_cast<int>(b2_s.size()), 1);

        auto ev = read_control_key(-1, selected + 1);
        if (ev.key == Key::None) return ScreenAction::Exit;
        if (handle_global_key(ev.key)) continue;
        if (ev.key == Key::Mouse) {
            auto hit = mouse_controls_.process_event(ev.mouse);
            if (hit.is_hover()) {
                continue;
            } else if (hit.is_click()) {
                if (hit.control_id == 1) return ScreenAction::Next;
                if (hit.control_id == 2) return ScreenAction::Exit;
            }
            continue;
        }
        if (ev.key == Key::Left || ev.key == Key::Right || ev.key == Key::Tab) {
            selected = 1 - selected;
        } else if (ev.key == Key::Enter) {
            return (selected == 0) ? ScreenAction::Next : ScreenAction::Exit;
        } else if (ev.key == Key::Escape) {
            return ScreenAction::Exit;
        }
    }
}

ScreenAction WinstallerTui::prompt_wifi() {
    progress_initialized_ = false;
    progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "network");
    std::string status_notice;
    while (true) {
        if (!show_recovery_prompt()) return ScreenAction::Exit;
        draw_header("Network Setup");

        auto geom = ScreenGeometry::compute(term_.rows(), term_.cols(), 76, 18, 90);
        int row = geom.content_box.row;
        int col = geom.content_box.col;
        int w = geom.content_box.width;
        int h = geom.content_box.height;

        term_.draw_box(row, col, w, h, "Network Connectivity", true);
        term_.print_at(row + 2, col + 4, AnsiTerminal::color_slate() + "Probing network interfaces..." + AnsiTerminal::color_reset());

        bool ethernet_online = false;
        auto inv_res = inspect_network_interfaces();
        if (const auto* inv = std::get_if<NetworkInventory>(&inv_res)) {
            ethernet_online = has_active_ethernet(*inv);
        }

        auto probe = probe_internet(default_internet_probe_targets(), true);
        if (probe.is_online || ethernet_online) {
            term_.print_at(row + 2, col + 4, AnsiTerminal::color_green() + "[OK] Active Internet Connection Detected." + AnsiTerminal::color_reset() + "                    ");
            term_.print_at(row + 4, col + 4, "Winstaller is connected and ready to download OS images.        ");

            int countdown = 5;
            bool cancel_auto = false;
            while (countdown > 0 && !cancel_auto) {
                term_.print_at(row + 6, col + 4, "Advancing to disk selection in " + AnsiTerminal::color_teal() + AnsiTerminal::color_bold() +
                    std::to_string(countdown) + "s" + AnsiTerminal::color_reset() + "... (or press Enter)          ");

                int b_row = row + h - 3;
                std::string b1_s = " [ Continue (" + std::to_string(countdown) + "s / Enter) ] ";
                std::string b2_s = " [ Wi-Fi Tool (W) ] ";
                std::string b3_s = " [ Exit (Esc) ] ";
                term_.print_at(b_row, col + 4, AnsiTerminal::bg_teal() + b1_s + AnsiTerminal::color_reset());
                term_.print_at(b_row, col + 34, b2_s);
                term_.print_at(b_row, col + 54, b3_s);

                mouse_controls_.clear();
                mouse_controls_.register_area(1, b_row, col + 4, static_cast<int>(b1_s.size()), 1);
                mouse_controls_.register_area(2, b_row, col + 34, static_cast<int>(b2_s.size()), 1);
                mouse_controls_.register_area(3, b_row, col + 54, static_cast<int>(b3_s.size()), 1);

                auto ev = term_.read_key(1000);
                if (handle_global_key(ev.key)) {
                    draw_header("Network Setup");
                    break;
                }
                if (ev.key == Key::Mouse) {
                    auto hit = mouse_controls_.process_event(ev.mouse);
                    if (hit.is_click()) {
                        if (hit.control_id == 1) return ScreenAction::Next;
                        if (hit.control_id == 2) {
                            term_.show_cursor(); term_.exit_raw_mode();
                            (void)open_network_settings(NetworkConfigUiMode::tui_only);
                            term_.enter_raw_mode(); term_.hide_cursor();
                            mouse_controls_.advance_generation(); cancel_auto = true; break;
                        }
                        if (hit.control_id == 3) return ScreenAction::Exit;
                    }
                    continue;
                }
                if (ev.key == Key::Enter) return ScreenAction::Next;
                if (ev.key == Key::Back) return ScreenAction::Back;
                if (ev.key == Key::Escape) return ScreenAction::Exit;
                if (ev.key == Key::Char && (ev.ch == 'w' || ev.ch == 'W')) {
                    term_.show_cursor();
                    term_.exit_raw_mode();
                    (void)open_network_settings(NetworkConfigUiMode::tui_only);
                    term_.enter_raw_mode();
                    term_.hide_cursor();
                    mouse_controls_.advance_generation();
                    cancel_auto = true;
                    break;
                }
                --countdown;
            }
            if (!cancel_auto) {
                return ScreenAction::Next;
            }
            continue;
        }

        term_.print_at(row + 2, col + 4, AnsiTerminal::color_yellow() + "[!] No active internet connection detected." + AnsiTerminal::color_reset() + "              ");
        term_.print_at(row + 4, col + 4, "You may connect to Wi-Fi now, or continue if you have a local image cache.");
        if (!status_notice.empty()) {
            term_.print_at(row + 5, col + 4, AnsiTerminal::color_red() + status_notice.substr(0, std::max(20, w - 8)) + AnsiTerminal::color_reset());
        }
        term_.print_at(row + 7, col + 6, "• Press " + AnsiTerminal::color_teal() + "Enter" + AnsiTerminal::color_reset() + " to open Wi-Fi / Network Manager (nmtui / iwctl)");
        term_.print_at(row + 8, col + 6, "• Press " + AnsiTerminal::color_teal() + "R" + AnsiTerminal::color_reset() + " to rescan network connectivity");
        term_.print_at(row + 9, col + 6, "• Press " + AnsiTerminal::color_teal() + "S" + AnsiTerminal::color_reset() + " to skip and continue in Offline Mode");
        term_.print_at(row + 10, col + 6, "• Press " + AnsiTerminal::color_teal() + "B" + AnsiTerminal::color_reset() + " to go Back  |  " + AnsiTerminal::color_teal() + "Esc" + AnsiTerminal::color_reset() + " to Reboot");

        int b_row = row + h - 3;
        std::string b1_s = " [ Open Wi-Fi (Enter) ] ";
        std::string b2_s = " [ Rescan (R) ] ";
        std::string b3_s = " [ Skip (S) ] ";
        std::string b4_s = " [ Back (B) ] ";
        term_.print_at(b_row, col + 4, AnsiTerminal::bg_teal() + b1_s + AnsiTerminal::color_reset());
        term_.print_at(b_row, col + 28, b2_s);
        term_.print_at(b_row, col + 44, b3_s);
        term_.print_at(b_row, col + 58, b4_s);

        mouse_controls_.clear();
        mouse_controls_.register_area(1, b_row, col + 4, static_cast<int>(b1_s.size()), 1);
        mouse_controls_.register_area(2, b_row, col + 28, static_cast<int>(b2_s.size()), 1);
        mouse_controls_.register_area(3, b_row, col + 44, static_cast<int>(b3_s.size()), 1);
        mouse_controls_.register_area(4, b_row, col + 58, static_cast<int>(b4_s.size()), 1);

        bool re_probe = false;
        while (!re_probe) {
            auto ev = term_.read_key(2500);
            if (handle_global_key(ev.key)) {
                re_probe = true;
                break;
            }
            if (ev.key == Key::None) {
                re_probe = true;
                break;
            }
            if (ev.key == Key::Mouse) {
                auto hit = mouse_controls_.process_event(ev.mouse);
                if (hit.is_click()) {
                    if (hit.control_id == 1) {
                        term_.show_cursor(); term_.exit_raw_mode();
                        auto cfg_res = open_network_settings(NetworkConfigUiMode::tui_only);
                        term_.enter_raw_mode(); term_.hide_cursor();
                        mouse_controls_.advance_generation();
                        if (!cfg_res.executed) status_notice = cfg_res.error_message;
                        else status_notice.clear();
                        re_probe = true; break;
                    } else if (hit.control_id == 2) {
                        status_notice.clear(); re_probe = true; break;
                    } else if (hit.control_id == 3) {
                        return ScreenAction::Next;
                    } else if (hit.control_id == 4) {
                        return ScreenAction::Back;
                    }
                }
                continue;
            }
            if (ev.key == Key::Enter) {
                term_.show_cursor();
                term_.exit_raw_mode();
                auto cfg_res = open_network_settings(NetworkConfigUiMode::tui_only);
                term_.enter_raw_mode();
                term_.hide_cursor();
                if (!cfg_res.executed) {
                    status_notice = cfg_res.error_message;
                } else {
                    status_notice.clear();
                }
                re_probe = true;
            } else if (ev.key == Key::Char && (ev.ch == 'r' || ev.ch == 'R')) {
                status_notice.clear();
                re_probe = true;
            } else if (ev.key == Key::Char && (ev.ch == 's' || ev.ch == 'S')) {
                return ScreenAction::Next;
            } else if (ev.key == Key::Back) {
                return ScreenAction::Back;
            } else if (ev.key == Key::Escape) {
                return ScreenAction::Exit;
            }
        }
    }
}

void WinstallerTui::show_eula_modal() {
    progress_initialized_ = false;
    progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "terms");
    while (true) {
        if (!show_recovery_prompt()) return;
        term_.update_size();
        int m_w = std::min(74, term_.cols() - 4);
        int m_h = std::min(16, term_.rows() - 4);
        int m_col = std::max(2, (term_.cols() - m_w) / 2);
        int m_row = std::max(3, (term_.rows() - m_h) / 2);

        term_.fill_rect(m_row, m_col, m_w, m_h, ' ');
        term_.draw_box(m_row, m_col, m_w, m_h, "Winstaller - Terms of Use & Liability Waiver", true);
        term_.print_at(m_row + 2, m_col + 4, AnsiTerminal::color_gold() + AnsiTerminal::color_bold() + "[ Terms of Use & Liability Disclaimer ]" + AnsiTerminal::color_reset());
        term_.print_at(m_row + 4, m_col + 4, "1. Winstaller is an automated deployment utility provided 'AS-IS'.");
        term_.print_at(m_row + 5, m_col + 4, "2. ALL DATA on the selected target drive will be PERMANENTLY ERASED.");
        term_.print_at(m_row + 6, m_col + 4, "3. The authors assume NO LIABILITY for data loss, hardware damage, or");
        term_.print_at(m_row + 7, m_col + 4, "   licensing issues resulting from the use of this software.");
        term_.print_at(m_row + 8, m_col + 4, "4. Users are solely responsible for selecting drives and keeping backups.");
        term_.print_at(m_row + 10, m_col + 4, AnsiTerminal::color_slate() + "By continuing installation, you agree to all these terms." + AnsiTerminal::color_reset());

        std::string close_s = " [ Close / Return (Enter / Esc) ] ";
        int b_c = m_col + std::max(4, (m_w - 34) / 2);
        term_.print_at(m_row + m_h - 2, b_c, AnsiTerminal::bg_teal() + close_s + AnsiTerminal::color_reset());

        mouse_controls_.clear();
        mouse_controls_.register_area(1, m_row + m_h - 2, b_c, static_cast<int>(close_s.size()), 1);

        auto ev = read_control_key(-1, 1);
        if (ev.key == Key::None) break;
        if (handle_global_key(ev.key)) continue;
        if (ev.key == Key::Mouse) {
            auto hit = mouse_controls_.process_event(ev.mouse);
            if (hit.is_click() && hit.control_id == 1) break;
            continue;
        }
        if (ev.key == Key::Enter || ev.key == Key::Escape || (ev.key == Key::Char && (ev.ch == 'e' || ev.ch == 'E'))) {
            break;
        }
    }
    mouse_controls_.advance_generation();
}

std::pair<ScreenAction, std::string> WinstallerTui::prompt_disk_selection(const std::vector<DiskInfo>& disks) {
    progress_initialized_ = false;
    progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "disks");
    int selected_idx = 0;
    int top_idx = 0;
    bool follow_selection = true;
    while (true) {
        if (!show_recovery_prompt()) return {ScreenAction::Exit, ""};
        draw_header("Target Disk Selection");

        auto geom = ScreenGeometry::compute(term_.rows(), term_.cols(), 76, 18, 90);
        int row = geom.content_box.row;
        int col = geom.content_box.col;
        int w = geom.content_box.width;
        int h = geom.content_box.height;

        if (disks.empty()) {
            term_.draw_box(row, col, w, h, "No Eligible Disk Found", true);
            term_.print_at(row + 3, col + 4, AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "[!] No eligible internal storage drives (>= 32 GB) found." + AnsiTerminal::color_reset());
            term_.print_at(row + 5, col + 4, "Please verify that a target storage drive of at least 32 GB is attached");
            term_.print_at(row + 6, col + 4, "and visible to the controller (NVMe / SATA / VirtIO / SCSI).");
            int b_row = row + h - 3;
            std::string r1_s = " [ Rescan Drives (Enter) ] ";
            std::string r2_s = " [ Back (B) ] ";
            std::string r3_s = " [ Exit & Reboot (Esc) ] ";
            term_.print_at(b_row, col + 4, AnsiTerminal::bg_teal() + r1_s + AnsiTerminal::color_reset());
            term_.print_at(b_row, col + 34, r2_s);
            term_.print_at(b_row, col + 50, r3_s);

            mouse_controls_.clear();
            mouse_controls_.register_area(1, b_row, col + 4, static_cast<int>(r1_s.size()), 1);
            mouse_controls_.register_area(2, b_row, col + 34, static_cast<int>(r2_s.size()), 1);
            mouse_controls_.register_area(3, b_row, col + 50, static_cast<int>(r3_s.size()), 1);

            while (true) {
                auto ev = read_control_key(-1, 1);
                if (ev.key == Key::None) return {ScreenAction::Exit, ""};
                if (handle_global_key(ev.key)) break;
                if (ev.key == Key::Mouse) {
                    auto hit = mouse_controls_.process_event(ev.mouse);
                    if (ev.mouse.kind == MouseKind::Move) break;
                    if (hit.is_click()) {
                        if (hit.control_id == 1) return {ScreenAction::Next, ""};
                        if (hit.control_id == 2) return {ScreenAction::Back, ""};
                        if (hit.control_id == 3) return {ScreenAction::Exit, ""};
                    }
                    continue;
                }
                if (ev.key == Key::Enter || (ev.key == Key::Char && (ev.ch == 'r' || ev.ch == 'R'))) {
                    return {ScreenAction::Next, ""};
                }
                if (ev.key == Key::Back) return {ScreenAction::Back, ""};
                if (ev.key == Key::Escape) return {ScreenAction::Exit, ""};
            }
            continue;
        }

        term_.draw_box(row, col, w, h, "Target Storage Disk Selection", true);
        term_.print_at(row + 2, col + 4, AnsiTerminal::color_bold() + "Select target drive for OS installation:" + AnsiTerminal::color_reset());
        term_.print_at(row + 3, col + 4, AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "[!] WARNING: All existing data on this drive will be WIPED!" + AnsiTerminal::color_reset());

        int max_visible = std::max(2, (h - 9) / 2);
        top_idx = std::clamp(top_idx, 0, std::max(0, static_cast<int>(disks.size()) - max_visible));
        if (follow_selection) {
            if (selected_idx < top_idx) top_idx = selected_idx;
            if (selected_idx >= top_idx + max_visible) top_idx = selected_idx - max_visible + 1;
            follow_selection = false;
        }

        for (int i = 0; i < max_visible && (top_idx + i) < static_cast<int>(disks.size()); ++i) {
            int idx = top_idx + i;
            int d_row = row + 5 + i * 2;
            const auto& d = disks[idx];
            std::uint64_t total_gb = d.size_bytes / (1024ULL * 1024 * 1024);
            std::uint64_t allocated_bytes = 0;
            for (const auto& child : d.children) {
                allocated_bytes += child.size_bytes;
            }
            std::uint64_t free_gb = (d.size_bytes > allocated_bytes)
                ? ((d.size_bytes - allocated_bytes) / (1024ULL * 1024 * 1024))
                : 0;

            std::string model = d.model.value_or(d.name);
            std::string tran = d.transport.value_or(d.path.find("nvme") != std::string::npos ? "nvme" : "disk");
            std::string space_info = std::to_string(total_gb) + "G (Free: " + std::to_string(free_gb) + "G)";
            std::string label = d.path + " [" + model + ", " + space_info + "] (" + tran + ")";
            if (!d.children.empty()) {
                label += " [" + std::to_string(d.children.size()) + " part]";
            }
            int max_lbl = std::max(20, w - 10);
            label = clip_to_width(label, max_lbl);

            if (idx == selected_idx) {
                term_.print_at(d_row, col + 4, AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + " > (*) " + label + AnsiTerminal::color_reset());
            } else {
                term_.print_at(d_row, col + 4, AnsiTerminal::color_slate() + "   ( ) " + label + AnsiTerminal::color_reset());
            }
        }

        term_.print_at(row + h - 4, col + 4, AnsiTerminal::color_slate() + "* Continuing (Enter) constitutes agreement to Terms & Liability Waiver." + AnsiTerminal::color_reset());

        int b_row = row + h - 2;
        std::string b1 = " [ Agree & Install (Enter) ] ";
        std::string b2 = " [ EULA (E) ] ";
        std::string b3 = " [ Back (B) ] ";
        std::string b4 = " [ Esc ] ";
        term_.print_at(b_row, col + 4, AnsiTerminal::bg_teal() + b1 + AnsiTerminal::color_reset());
        term_.print_at(b_row, col + 35, b2);
        term_.print_at(b_row, col + 49, b3);
        term_.print_at(b_row, col + 62, b4);

        mouse_controls_.clear();
        for (int i = 0; i < max_visible && (top_idx + i) < static_cast<int>(disks.size()); ++i) {
            int idx = top_idx + i;
            int d_row = row + 5 + i * 2;
            mouse_controls_.register_area(100 + idx, d_row, col + 4, std::max(20, w - 10) + 8, 1, idx, "disk.item." + disks[idx].path);
        }
        mouse_controls_.register_area(1, b_row, col + 4, static_cast<int>(b1.size()), 1);
        mouse_controls_.register_area(2, b_row, col + 35, static_cast<int>(b2.size()), 1);
        mouse_controls_.register_area(3, b_row, col + 49, static_cast<int>(b3.size()), 1);
        mouse_controls_.register_area(4, b_row, col + 62, static_cast<int>(b4.size()), 1);

        auto ev = read_control_key(-1, 100 + selected_idx, "disk.item." + disks[selected_idx].path);
        if (ev.key == Key::None) return {ScreenAction::Exit, ""};
        if (ev.key != Key::Mouse && ev.key != Key::None) follow_selection = true;
        if (handle_global_key(ev.key)) continue;
        if (ev.key == Key::Mouse) {
            auto hit = mouse_controls_.process_event(ev.mouse);
            if (hit.is_wheel()) {
                if (hit.control_id >= 100) {
                    top_idx = static_cast<int>(std::clamp<long long>(static_cast<long long>(top_idx) - hit.wheel_delta, 0, std::max(0, static_cast<int>(disks.size()) - max_visible)));
                    mouse_controls_.advance_generation();
                    term_.mouse().reset();
                    term_.set_hover_area(std::nullopt);
                }
                continue;
            } else if (hit.is_hover()) {
                continue;
            } else if (hit.is_click()) {
                if (hit.control_id >= 100) {
                    selected_idx = hit.data;
                    continue;
                } else if (hit.control_id == 2) {
                    show_eula_modal();
                    continue;
                } else if (hit.control_id == 3) {
                    return {ScreenAction::Back, ""};
                } else if (hit.control_id == 4) {
                    return {ScreenAction::Exit, ""};
                } else if (hit.control_id == 1) {
                    ev.key = Key::Enter;
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }

        if (ev.key == Key::Up) {
            if (selected_idx > 0) --selected_idx;
            continue;
        } else if (ev.key == Key::Down) {
            if (selected_idx + 1 < static_cast<int>(disks.size())) ++selected_idx;
            continue;
        } else if (ev.key == Key::Char && (ev.ch == 'e' || ev.ch == 'E')) {
            show_eula_modal();
            continue;
        } else if (ev.key == Key::Back) {
            return {ScreenAction::Back, ""};
        } else if (ev.key == Key::Escape) {
            return {ScreenAction::Exit, ""};
        } else if (ev.key != Key::Enter) {
            continue;
        }

        if (!disks[selected_idx].children.empty() && !confirm_disk_wipe(disks[selected_idx])) continue;
        return {ScreenAction::Next, disks[selected_idx].path};
    }
}

bool WinstallerTui::confirm_disk_wipe(const DiskInfo& disk) {
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "disk-wipe");
    while (true) {
        if (!show_recovery_prompt()) return false;
        const int w = std::min(68, term_.cols() - 4);
        const int row = std::max(4, (term_.rows() - 10) / 2);
        const int col = std::max(2, (term_.cols() - w) / 2);
        term_.fill_rect(row, col, w, 10, ' ');
        term_.draw_box(row, col, w, 10, " DISK ERROR: EXISTING PARTITIONS ", true);
        term_.print_at(row + 2, col + 4, AnsiTerminal::color_red() + "[!] Disk has " + std::to_string(disk.children.size()) + " existing partition(s)!" + AnsiTerminal::color_reset());
        term_.print_at(row + 4, col + 4, "Winstaller requires a clean disk. Existing data will be wiped.");
        term_.print_at(row + 5, col + 4, "Do you want to FORCE WIPE all partitions and proceed?");
        const std::string wipe = " [ Force Wipe (Enter) ] ", cancel = " [ Cancel (Esc / B) ] ";
        term_.print_at(row + 7, col + 4, AnsiTerminal::bg_red() + wipe + AnsiTerminal::color_reset());
        term_.print_at(row + 7, col + 36, cancel);
        mouse_controls_.clear();
        mouse_controls_.register_area(1, row + 7, col + 4, static_cast<int>(wipe.size()), 1, 0, "disk-wipe.confirm");
        mouse_controls_.register_area(2, row + 7, col + 36, static_cast<int>(cancel.size()), 1, 0, "disk-wipe.cancel");
        const auto ev = read_control_key(-1, 1);
        if (handle_global_key(ev.key)) continue;
        if (ev.key == Key::None || ev.key == Key::Escape || ev.key == Key::Back) return false;
        if (ev.key == Key::Enter) return true;
        if (ev.key == Key::Mouse) {
            const auto hit = mouse_controls_.process_event(ev.mouse);
            if (hit.is_click() && hit.control_id == 1) return true;
            if (hit.is_click() && hit.control_id == 2) return false;
        }
    }
}

std::vector<std::string> WinstallerTui::get_os_tabs() {
    return {"Windows", "Ubuntu"};
}

std::vector<std::pair<std::string, std::string>> WinstallerTui::get_tab_os_items(const std::string& tab_name) {
    if (tab_name == "Windows") {
        return {
            {"win11-home", "Windows 11 Home"},
            {"win11-pro", "Windows 11 Pro"}
        };
    }

    std::optional<OsTabCategory> category;
    if (tab_name == "Ubuntu") {
        category = OsTabCategory::Ubuntu;
    } else if (tab_name == "Debian") {
        category = OsTabCategory::Debian;
    } else if (tab_name == "RHEL / Fedora") {
        category = OsTabCategory::RhelFedora;
    } else if (tab_name == "Arch") {
        category = OsTabCategory::Arch;
    }

    if (!category.has_value()) {
        return {};
    }

    const auto targets = get_linux_targets_by_tab(*category);
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(targets.size());
    for (const auto& target : targets) {
        items.emplace_back(target.id, target.display_name);
    }
    return items;
}

void WinstallerTui::show_edition_modal(TuiEditionChoice& choice) {
    progress_initialized_ = false;
    progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "os");
    const auto tabs = catalog_tabs();
    int active_tab_idx = 0;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (tabs[i] == choice.os_tab) {
            active_tab_idx = i;
            break;
        }
    }
    if (choice.is_linux && active_tab_idx == 0 && tabs.size() > 1) {
        active_tab_idx = 1;
    }

    auto items = catalog_items(tabs[active_tab_idx]);
    int cur_item_idx = 0;
    if (!catalog_ && tabs[active_tab_idx] == "Windows") {
        cur_item_idx = (choice.edition_index == 6) ? 1 : 0;
    } else {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if ((choice.remote_target && items[i].first == choice.remote_target->product) || items[i].first == choice.linux_distro_id || items[i].second == choice.edition_name) {
                cur_item_idx = i;
                break;
            }
        }
    }

    int top_idx = 0;
    bool follow_selection = true;
    while (true) {
        if (!show_recovery_prompt()) return;
        draw_header("Select Operating System");
        term_.update_size();
        int m_w = std::min(76, term_.cols() - 4);
        int m_h = std::min(18, term_.rows() - 4);
        int m_col = std::max(2, (term_.cols() - m_w) / 2);
        int m_row = std::max(3, (term_.rows() - m_h) / 2);

        term_.fill_rect(m_row, m_col, m_w, m_h, ' ');
        term_.draw_box(m_row, m_col, m_w, m_h, "Select Target Operating System", true);

        std::string tab_bar;
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            if (static_cast<int>(i) == active_tab_idx) {
                tab_bar += AnsiTerminal::bg_teal() + AnsiTerminal::color_bold() + " [ " + tabs[i] + " ] " + AnsiTerminal::color_reset();
            } else {
                tab_bar += AnsiTerminal::color_slate() + " [ " + tabs[i] + " ] " + AnsiTerminal::color_reset();
            }
            if (i + 1 < tabs.size()) {
                tab_bar += AnsiTerminal::color_gray() + "|" + AnsiTerminal::color_reset();
            }
        }
        term_.print_at(m_row + 2, m_col + 3, tab_bar);

        std::string div_line;
        for (int c = 0; c < m_w - 6; ++c) div_line += "─";
        term_.print_at(m_row + 3, m_col + 3, AnsiTerminal::color_slate() + div_line + AnsiTerminal::color_reset());

        int max_display = std::max(3, m_h - 8);
        top_idx = std::clamp(top_idx, 0, std::max(0, static_cast<int>(items.size()) - max_display));
        if (follow_selection) {
            if (cur_item_idx < top_idx) top_idx = cur_item_idx;
            if (cur_item_idx >= top_idx + max_display) top_idx = cur_item_idx - max_display + 1;
            follow_selection = false;
        }

        for (int i = 0; i < max_display && (top_idx + i) < static_cast<int>(items.size()); ++i) {
            int idx = top_idx + i;
            int line_row = m_row + 4 + i;
            const auto& item = items[idx];
            std::string item_label = clip_to_width(item.second, m_w - 10);
            if (idx == cur_item_idx) {
                term_.print_at(line_row, m_col + 4, AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + " > (*) " + item_label + AnsiTerminal::color_reset());
            } else {
                term_.print_at(line_row, m_col + 4, AnsiTerminal::color_slate() + "   ( ) " + item_label + AnsiTerminal::color_reset());
            }
        }

        term_.print_at(m_row + m_h - 3, m_col + 4, AnsiTerminal::color_slate() + "Left/Right: Tab  │  Up/Down: Select  │  Enter: Confirm" + AnsiTerminal::color_reset());
        std::string ed_btn_sel = " [ Select (Enter) ] ";
        std::string ed_btn_esc = " [ Cancel (Esc) ] ";
        term_.print_at(m_row + m_h - 2, m_col + 14, AnsiTerminal::bg_teal() + ed_btn_sel + AnsiTerminal::color_reset());
        term_.print_at(m_row + m_h - 2, m_col + 42, ed_btn_esc);

        mouse_controls_.clear();
        int cur_tab_col = m_col + 3;
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            int tab_w = static_cast<int>(tabs[i].size()) + 6;
            mouse_controls_.register_area(200 + static_cast<int>(i), m_row + 2, cur_tab_col, tab_w, 1, static_cast<int>(i), "os.tab." + tabs[i]);
            cur_tab_col += tab_w + 1;
        }
        for (int i = 0; i < max_display && (top_idx + i) < static_cast<int>(items.size()); ++i) {
            int idx = top_idx + i;
            int line_row = m_row + 4 + i;
            mouse_controls_.register_area(300 + idx, line_row, m_col + 4, m_w - 8, 1, idx, "os.item." + items[idx].first);
        }
        mouse_controls_.register_area(1, m_row + m_h - 2, m_col + 14, static_cast<int>(ed_btn_sel.size()), 1);
        mouse_controls_.register_area(2, m_row + m_h - 2, m_col + 42, static_cast<int>(ed_btn_esc.size()), 1);

        auto ev = read_control_key(-1, 300 + cur_item_idx, items.empty() ? "" : "os.item." + items[cur_item_idx].first);
        if (ev.key == Key::None) break;
        if (ev.key != Key::Mouse && ev.key != Key::None) follow_selection = true;
        if (handle_global_key(ev.key)) continue;
        if (ev.key == Key::Mouse) {
            auto hit = mouse_controls_.process_event(ev.mouse);
            if (hit.is_wheel()) {
                if (hit.control_id >= 300) {
                    top_idx = static_cast<int>(std::clamp<long long>(static_cast<long long>(top_idx) - hit.wheel_delta, 0, std::max(0, static_cast<int>(items.size()) - max_display)));
                    mouse_controls_.advance_generation();
                    term_.mouse().reset();
                    term_.set_hover_area(std::nullopt);
                }
                continue;
            } else if (hit.is_hover()) {
                continue;
            } else if (hit.is_click()) {
                if (hit.control_id >= 200 && hit.control_id < 300) {
                    active_tab_idx = hit.data;
                    top_idx = 0; follow_selection = true;
                    mouse_controls_.advance_generation();
                    items = catalog_items(tabs[active_tab_idx]);
                    cur_item_idx = 0;
                    continue;
                } else if (hit.control_id >= 300) {
                    cur_item_idx = hit.data;
                    continue;
                } else if (hit.control_id == 2) {
                    break;
                } else if (hit.control_id == 1) {
                    ev.key = Key::Enter;
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }

        if (ev.key == Key::Left) {
            active_tab_idx = (active_tab_idx + static_cast<int>(tabs.size()) - 1) % static_cast<int>(tabs.size());
            items = catalog_items(tabs[active_tab_idx]);
            cur_item_idx = 0;
        } else if (ev.key == Key::Right) {
            active_tab_idx = (active_tab_idx + 1) % static_cast<int>(tabs.size());
            items = catalog_items(tabs[active_tab_idx]);
            cur_item_idx = 0;
        } else if (ev.key == Key::Up) {
            if (!items.empty()) {
                cur_item_idx = (cur_item_idx + static_cast<int>(items.size()) - 1) % static_cast<int>(items.size());
            }
        } else if (ev.key == Key::Down || ev.key == Key::Tab) {
            if (!items.empty()) {
                cur_item_idx = (cur_item_idx + 1) % static_cast<int>(items.size());
            }
        } else if (ev.key == Key::Enter) {
            if (!items.empty() && cur_item_idx >= 0 && cur_item_idx < static_cast<int>(items.size())) {
                const auto& sel = items[cur_item_idx];
                if (catalog_) {
                    auto target = catalog_->find(sel.first,choice.language);
                    if (!target) for (const auto& item:catalog_->targets) if(item.product==sel.first){target=item;break;}
                    if (!target || !target->supported) continue;
                    select_remote(choice,*target);
                    break;
                }
                choice.os_tab = tabs[active_tab_idx];
                choice.edition_name = sel.second;
                if (tabs[active_tab_idx] == "Windows") {
                    choice.is_linux = false;
                    choice.linux_distro_id = "";
                    choice.edition_index = (cur_item_idx == 1) ? 6 : 4;
                } else {
                    choice.is_linux = true;
                    choice.linux_distro_id = sel.first;
                    choice.reuse_existing_esd = false;
                }
            }
            break;
        } else if (ev.key == Key::Escape) {
            break;
        }
    }
}

void WinstallerTui::show_language_modal(TuiEditionChoice& choice, int& lang_idx, const std::vector<std::pair<std::string, std::string>>& languages) {
    if (languages.empty()) return;
    progress_initialized_ = false;
    progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "language");
    int n = static_cast<int>(languages.size());
    int cur_sel = std::clamp(lang_idx, 0, n - 1);

    int top_idx = 0;
    bool follow_selection = true;
    while (true) {
        if (!show_recovery_prompt()) return;
        draw_header("Select Language");
        term_.update_size();
        int m_w = std::min(60, term_.cols() - 4);
        int m_h = std::min(13, term_.rows() - 4);
        int m_col = std::max(2, (term_.cols() - m_w) / 2);
        int m_row = std::max(4, (term_.rows() - m_h) / 2);

        term_.fill_rect(m_row, m_col, m_w, m_h, ' ');
        term_.draw_box(m_row, m_col, m_w, m_h, "Select Target Language", true);

        int max_disp = std::max(2, m_h - 6);
        top_idx = std::clamp(top_idx, 0, std::max(0, n - max_disp));
        if (follow_selection) {
            if (cur_sel < top_idx) top_idx = cur_sel;
            if (cur_sel >= top_idx + max_disp) top_idx = cur_sel - max_disp + 1;
            follow_selection = false;
        }

        for (int i = 0; i < max_disp && (top_idx + i) < n; ++i) {
            int idx = top_idx + i;
            int line_row = m_row + 2 + i;
            std::string lang_label = clip_to_width(languages[idx].second, m_w - 8);
            if (idx == cur_sel) {
                term_.print_at(line_row, m_col + 4, AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + " > (*) " + lang_label + AnsiTerminal::color_reset());
            } else {
                term_.print_at(line_row, m_col + 4, AnsiTerminal::color_slate() + "   ( ) " + lang_label + AnsiTerminal::color_reset());
            }
        }

        int b_row = m_row + m_h - 2;
        std::string lg_btn_sel = " [ Select (Enter) ] ";
        std::string lg_btn_esc = " [ Cancel (Esc) ] ";
        term_.print_at(b_row, m_col + 8, AnsiTerminal::bg_teal() + lg_btn_sel + AnsiTerminal::color_reset());
        term_.print_at(b_row, m_col + 32, lg_btn_esc);

        mouse_controls_.clear();
        for (int i = 0; i < max_disp && (top_idx + i) < n; ++i) {
            int idx = top_idx + i;
            int line_row = m_row + 2 + i;
            mouse_controls_.register_area(300 + idx, line_row, m_col + 4, m_w - 8, 1, idx, "language.item." + languages[idx].first);
        }
        mouse_controls_.register_area(1, b_row, m_col + 8, static_cast<int>(lg_btn_sel.size()), 1);
        mouse_controls_.register_area(2, b_row, m_col + 32, static_cast<int>(lg_btn_esc.size()), 1);

        auto ev = read_control_key(-1, 300 + cur_sel, "language.item." + languages[cur_sel].first);
        if (ev.key == Key::None) break;
        if (ev.key != Key::Mouse && ev.key != Key::None) follow_selection = true;
        if (handle_global_key(ev.key)) continue;
        if (ev.key == Key::Mouse) {
            auto hit = mouse_controls_.process_event(ev.mouse);
            if (hit.is_wheel()) {
                if (hit.control_id >= 300) {
                    top_idx = static_cast<int>(std::clamp<long long>(static_cast<long long>(top_idx) - hit.wheel_delta, 0, std::max(0, n - max_disp)));
                    mouse_controls_.advance_generation();
                    term_.mouse().reset();
                    term_.set_hover_area(std::nullopt);
                }
                continue;
            } else if (hit.is_hover()) {
                continue;
            } else if (hit.is_click()) {
                if (hit.control_id >= 300) {
                    cur_sel = hit.data;
                    continue;
                } else if (hit.control_id == 2) {
                    break;
                } else if (hit.control_id == 1) {
                    lang_idx = cur_sel;
                    choice.language = languages[lang_idx].first;
                    break;
                }
            }
            continue;
        } else if (ev.key == Key::Up) {
            cur_sel = (cur_sel + n - 1) % n;
        } else if (ev.key == Key::Down || ev.key == Key::Tab) {
            cur_sel = (cur_sel + 1) % n;
        } else if (ev.key == Key::Enter) {
            lang_idx = cur_sel;
            choice.language = languages[lang_idx].first;
            break;
        } else if (ev.key == Key::Escape) {
            break;
        }
    }
}

std::pair<ScreenAction, TuiEditionChoice> WinstallerTui::prompt_edition_selection(const std::string& detected_oem_edition, bool cached_esd_found) {
    progress_initialized_ = false;
    progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "options");
    TuiEditionChoice choice;
    if (!detected_oem_edition.empty() && detected_oem_edition.find("Pro") != std::string::npos) {
        choice.edition_index = 6;
        choice.edition_name = "Windows 11 Pro";
    } else {
        choice.edition_index = 4;
        choice.edition_name = "Windows 11 Home";
    }
    choice.is_linux = false;
    choice.os_tab = "Windows";
    choice.linux_distro_id = "";
    choice.language = "ko-KR";
    choice.reuse_existing_esd = cached_esd_found;

    std::vector<std::pair<std::string, std::string>> languages = {
        {"ko-KR", "Korean (ko-KR)"},
        {"en-US", "English (en-US)"},
        {"ja-JP", "Japanese (ja-JP)"},
        {"zh-CN", "Chinese Simplified (zh-CN)"},
        {"de-DE", "German (de-DE)"},
        {"fr-FR", "French (fr-FR)"},
        {"es-ES", "Spanish (es-ES)"}
    };
    int lang_idx = 0;
    int cur_row = 0; // 0 = Edition, 1 = Language, 2 = Install Source, 3 = Start Button

    const auto builtin_choice=choice;
    const auto builtin_languages=languages;
    bool interacted=false;
    if (updates_) catalog_=updates_->snapshot().catalog;
    auto initial_target=[&] {
        choice=builtin_choice;languages=builtin_languages;lang_idx=0;
        choice.remote_target.reset();
        if(catalog_) {
            auto found=std::find_if(catalog_->targets.begin(),catalog_->targets.end(),[](const auto& t){return t.supported;});
            if(found!=catalog_->targets.end())select_remote(choice,*found);
            else choice.edition_name="No compatible OS in the verified catalog";
        }
    };
    initial_target();
    while (true) {
        auto update_snapshot=updates_?updates_->snapshot():UpdateSnapshot{};
        if(!interacted && update_snapshot.catalog!=catalog_){catalog_=update_snapshot.catalog;initial_target();}
        if(choice.remote_target) {
            languages=catalog_->languages(choice.remote_target->product);
            lang_idx=0;
            for(size_t i=0;i<languages.size();++i)if(languages[i].first==choice.language)lang_idx=static_cast<int>(i);
            if(auto target=catalog_->find(choice.remote_target->product,choice.language))select_remote(choice,*target);
        }
        const bool selectable=!catalog_ || (choice.remote_target && choice.remote_target->supported);
        if (!show_recovery_prompt()) return {ScreenAction::Exit, choice};
        draw_header("Installation Options");

        auto geom = ScreenGeometry::compute(term_.rows(), term_.cols(), 76, 18, 90);
        int row = geom.content_box.row;
        int col = geom.content_box.col;
        int w = geom.content_box.width;
        int h = geom.content_box.height;

        term_.draw_box(row, col, w, h, "Installation Options", true);

        term_.print_at(row + 2, col + 4, "System Architecture: " + AnsiTerminal::color_bold() + "x86_64" + AnsiTerminal::color_reset());
        std::string oem_str = detected_oem_edition.empty() ? "None (Default: Home)" : (detected_oem_edition + " [Detected]");
        term_.print_at(row + 3, col + 4, "ACPI OEM License   : " + AnsiTerminal::color_green() + oem_str + AnsiTerminal::color_reset());

        auto row_active = [&](int r) {
            return cur_row == r ? (AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + "> " + AnsiTerminal::color_reset()) : "  ";
        };

        std::string ed_field = "[ " + choice.edition_name + " ]";
        term_.print_at(row + 5, col + 4, row_active(0) + AnsiTerminal::color_bold() + "[1] Target OS       : " + AnsiTerminal::color_reset() + AnsiTerminal::color_cyan() + ed_field + AnsiTerminal::color_reset());

        std::string lang_field = "[ " + languages[lang_idx].second + " ]";
        term_.print_at(row + 7, col + 4, row_active(1) + AnsiTerminal::color_bold() + "[2] Target Language : " + AnsiTerminal::color_reset() + AnsiTerminal::color_cyan() + lang_field + AnsiTerminal::color_reset());

        term_.print_at(row + 9, col + 4, row_active(2) + AnsiTerminal::color_bold() + "[3] Install Source  : " + AnsiTerminal::color_reset() + (cur_row == 2 && !choice.is_linux ? AnsiTerminal::color_teal() + "(Press Space to toggle)" + AnsiTerminal::color_reset() : ""));
        if (choice.is_linux) {
            term_.print_at(row + 10, col + 8, AnsiTerminal::color_green() + "[*] Official HTTPS Mirror (Direct Stream & Verify)" + AnsiTerminal::color_reset());
        } else if (cached_esd_found) {
            std::string esd_check = choice.reuse_existing_esd ? "[X] Local Cache on P5 (Fastest, skip download)" : "[ ] Fresh Download from Microsoft CDN";
            term_.print_at(row + 10, col + 8, AnsiTerminal::color_green() + esd_check + AnsiTerminal::color_reset());
        } else {
            term_.print_at(row + 10, col + 8, AnsiTerminal::color_slate() + "[!] Fresh Download from Microsoft CDN (No local cache)" + AnsiTerminal::color_reset());
        }

        term_.print_at(row + 12, col + 4, AnsiTerminal::color_slate() + "Up/Down: Move  |  Enter/Space: Change Option" + AnsiTerminal::color_reset());

        const bool has_update=update_snapshot.release!=nullptr;
        if(updates_ && updates_->configured()) {
            const std::string status=has_update ? "[ Update available (U) ]" : update_snapshot.checking ? "Checking for updates..." : update_snapshot.check_error.empty() ? "[ Check updates (R) ]" : "Update check unavailable. Retry (R)";
            term_.print_at(row+13,col+4,clip_to_width(status,w-8));
        }
        int b_row = row + h - 3;
        std::string ed_next = " [ Next: Select Disk (Enter) ] ";
        std::string ed_back = " [ Back (B) ] ";
        std::string ed_exit = " [ Exit (Esc) ] ";
        if (cur_row == 3) {
            term_.print_at(b_row, col + 4, AnsiTerminal::bg_teal() + ed_next + AnsiTerminal::color_reset());
        } else {
            term_.print_at(b_row, col + 4, AnsiTerminal::color_bold() + ed_next + AnsiTerminal::color_reset());
        }
        term_.print_at(b_row, col + 38, ed_back);
        term_.print_at(b_row, col + 54, ed_exit);

        mouse_controls_.clear();
        mouse_controls_.register_area(10, row + 5, col + 4, w - 8, 1, 0);
        mouse_controls_.register_area(11, row + 7, col + 4, w - 8, 1, 1);
        if (cached_esd_found && !choice.is_linux) mouse_controls_.register_area(12, row + 9, col + 4, w - 8, 1, 2);
        mouse_controls_.register_area(1, b_row, col + 4, static_cast<int>(ed_next.size()), 1);
        mouse_controls_.register_area(2, b_row, col + 38, static_cast<int>(ed_back.size()), 1);
        mouse_controls_.register_area(3, b_row, col + 54, static_cast<int>(ed_exit.size()), 1);

        if(updates_ && !boot_acknowledged_){updates_->acknowledge_boot();boot_acknowledged_=true;}
        if(has_update)mouse_controls_.register_area(60,row+13,col+4,25,1);
        KeyEvent ev;
        do {
            ev=read_control_key(updates_ && updates_->configured()?100:-1,cur_row<3?10+cur_row:1);
            if(ev.key!=Key::None || !updates_ || !updates_->configured())break;
            const auto next=updates_->snapshot();
            if(next.catalog!=update_snapshot.catalog || next.release!=update_snapshot.release || next.checking!=update_snapshot.checking || next.check_error!=update_snapshot.check_error)break;
        } while(true);
        if(ev.key==Key::None) {
            if(updates_ && updates_->configured())continue;
            return {ScreenAction::Exit,choice};
        }
        if(ev.key!=Key::Mouse)interacted=true;
        if(ev.key==Key::Char && (ev.ch=='u'||ev.ch=='U') && has_update) {
            if(show_update_modal()){choice.restart_requested=true;return {ScreenAction::Exit,choice};}
            continue;
        }
        if(ev.key==Key::Char && (ev.ch=='r'||ev.ch=='R') && updates_){updates_->check();continue;}
        if (handle_global_key(ev.key)) continue;
        if (ev.key == Key::Mouse) {
            auto hit = mouse_controls_.process_event(ev.mouse);
            if (hit.is_wheel()) {
                continue;
            } else if (hit.is_hover()) {
                continue;
            } else if (hit.is_click()) {
                interacted=true;
                if(hit.control_id==60){if(show_update_modal()){choice.restart_requested=true;return {ScreenAction::Exit,choice};}continue;}
                if (hit.control_id == 10) { show_edition_modal(choice); cur_row = 1; continue; }
                if (hit.control_id == 11) {
                    show_language_modal(choice, lang_idx, languages);
                    if (lang_idx != 0 && cached_esd_found && !choice.is_linux) choice.reuse_existing_esd = false;
                    cur_row = 3; continue;
                }
                if (hit.control_id == 12 && cached_esd_found && !choice.is_linux) { choice.reuse_existing_esd = !choice.reuse_existing_esd; continue; }
                if (hit.control_id == 1 && selectable) return {ScreenAction::Next, choice};
                if (hit.control_id == 2) return {ScreenAction::Back, choice};
                if (hit.control_id == 3) return {ScreenAction::Exit, choice};
            }
            continue;
        } else if (ev.key == Key::Up) {
            cur_row = (cur_row + 3) % 4;
            if (cur_row == 2 && (!cached_esd_found || choice.is_linux)) cur_row = 1;
        } else if (ev.key == Key::Down || ev.key == Key::Tab) {
            cur_row = (cur_row + 1) % 4;
            if (cur_row == 2 && (!cached_esd_found || choice.is_linux)) cur_row = 3;
        } else if (ev.key == Key::Enter) {
            if (cur_row == 0) {
                show_edition_modal(choice);
                cur_row = 1;
            } else if (cur_row == 1) {
                show_language_modal(choice, lang_idx, languages);
                if (lang_idx != 0 && cached_esd_found && !choice.is_linux) {
                    choice.reuse_existing_esd = false;
                }
                cur_row = 3;
            } else if (cur_row == 2 && cached_esd_found && !choice.is_linux) {
                choice.reuse_existing_esd = !choice.reuse_existing_esd;
            } else if (cur_row == 3 && selectable) {
                return {ScreenAction::Next, choice};
            }
        } else if (ev.key == Key::Space) {
            if (cur_row == 0) {
                show_edition_modal(choice);
                cur_row = 1;
            } else if (cur_row == 1) {
                show_language_modal(choice, lang_idx, languages);
                cur_row = 3;
            } else if (cur_row == 2 && cached_esd_found && !choice.is_linux) {
                choice.reuse_existing_esd = !choice.reuse_existing_esd;
            }
        } else if (ev.key == Key::Left || ev.key == Key::Right) {
            if (cur_row == 0) {
                show_edition_modal(choice);
            }
        } else if (ev.key == Key::Back) {
            return {ScreenAction::Back, choice};
        } else if (ev.key == Key::Escape) {
            return {ScreenAction::Exit, choice};
        }
    }
}

ScreenAction WinstallerTui::prompt_user_account(TuiUserData& user_data, bool is_linux) {
    progress_initialized_ = false;
    progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "account");
    if (is_linux) {
        if (user_data.username == "User") {
            user_data.username.clear();
        } else {
            for (char& c : user_data.username) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        if (user_data.computer_name.empty()) {
            user_data.computer_name = "litewin-pc";
        }
    }

    int cur_field = 0;
    std::string error_hint;

    auto pad20 = [](const std::string& s) {
        return s.size() < 20 ? (s + std::string(20 - s.size(), ' ')) : s.substr(0, 20);
    };

    while (true) {
        if (!show_recovery_prompt()) return ScreenAction::Exit;
        draw_header(is_linux ? "Linux User Account Setup (Required)" : "User Account Setup");

        auto geom = ScreenGeometry::compute(term_.rows(), term_.cols(), 76, 18, 90);
        int row = geom.content_box.row;
        int col = geom.content_box.col;
        int w = geom.content_box.width;
        int h = geom.content_box.height;

        term_.draw_box(row, col, w, h, is_linux ? "Linux Account & Password Setup (Required)" : "User Account Setup", true);

        term_.print_at(row + 2, col + 4, AnsiTerminal::color_teal() +
            "* Set your login username and password before installation starts." + AnsiTerminal::color_reset());
        term_.print_at(row + 3, col + 4, AnsiTerminal::color_slate() +
            "  (For Linux, username is lowercase and password is required for console/sudo login)" + AnsiTerminal::color_reset());

        auto f_style = [&](int f) {
            return (cur_field == f) ? (AnsiTerminal::bg_teal() + AnsiTerminal::color_bold()) : AnsiTerminal::color_cyan();
        };
        auto row_prefix = [&](int f) {
            return (cur_field == f) ? (AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + "> " + AnsiTerminal::color_reset()) : "  ";
        };

        std::string masked_pw(user_data.password.size(), '*');
        std::string masked_pw2(user_data.password_confirm.size(), '*');

        std::string pw_status;
        if (user_data.password.empty()) {
            pw_status = AnsiTerminal::color_gold() + "[Required]" + AnsiTerminal::color_reset();
        } else if (user_data.password_matches()) {
            pw_status = AnsiTerminal::color_green() + AnsiTerminal::color_bold() + "[OK: Matched]" + AnsiTerminal::color_reset();
        } else {
            pw_status = AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "[!: Mismatch]" + AnsiTerminal::color_reset();
        }

        term_.print_at(row + 5, col + 4, row_prefix(0) + "1. Username (ID)    : " + f_style(0) + "[" + pad20(user_data.username) + "]" + AnsiTerminal::color_reset());
        term_.print_at(row + 7, col + 4, row_prefix(1) + "2. Password         : " + f_style(1) + "[" + pad20(masked_pw) + "]" + AnsiTerminal::color_reset());
        term_.print_at(row + 9, col + 4, row_prefix(2) + "3. Confirm Password : " + f_style(2) + "[" + pad20(masked_pw2) + "]" + AnsiTerminal::color_reset() + " " + pw_status);
        term_.print_at(row + 11, col + 4, row_prefix(3) + "4. Hostname (PC)    : " + f_style(3) + "[" + pad20(user_data.computer_name) + "]" + AnsiTerminal::color_reset());

        if (!error_hint.empty()) {
            term_.print_at(row + 13, col + 4, AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "! " + error_hint + AnsiTerminal::color_reset());
        } else {
            term_.print_at(row + 13, col + 4, AnsiTerminal::color_slate() + "Up/Down/Tab: Move  |  Type/Backspace: Edit  |  Enter: Next/Confirm" + AnsiTerminal::color_reset());
        }

        int b_row = row + h - 3;
        std::string act_start = " [ Start Installation (Enter) ] ";
        std::string act_back = " [ Back ] ";
        std::string act_exit = " [ Exit (Esc) ] ";
        if (cur_field == 4) {
            term_.print_at(b_row, col + 4, AnsiTerminal::bg_teal() + act_start + AnsiTerminal::color_reset());
        } else {
            term_.print_at(b_row, col + 4, AnsiTerminal::color_bold() + act_start + AnsiTerminal::color_reset());
        }
        if (cur_field == 5) {
            term_.print_at(b_row, col + 40, AnsiTerminal::bg_teal() + act_back + AnsiTerminal::color_reset());
        } else {
            term_.print_at(b_row, col + 40, act_back);
        }
        term_.print_at(b_row, col + 54, act_exit);

        mouse_controls_.clear();
        mouse_controls_.register_area(10, row + 5, col + 4, w - 8, 1, 0);
        mouse_controls_.register_area(11, row + 7, col + 4, w - 8, 1, 1);
        mouse_controls_.register_area(12, row + 9, col + 4, w - 8, 1, 2);
        mouse_controls_.register_area(13, row + 11, col + 4, w - 8, 1, 3);
        mouse_controls_.register_area(1, b_row, col + 4, static_cast<int>(act_start.size()), 1);
        mouse_controls_.register_area(2, b_row, col + 40, static_cast<int>(act_back.size()), 1);
        mouse_controls_.register_area(3, b_row, col + 54, static_cast<int>(act_exit.size()), 1);

        auto ev = read_control_key(-1, cur_field < 4 ? 10 + cur_field : (cur_field == 4 ? 1 : 2));
        if (ev.key == Key::None) return ScreenAction::Exit;
        if (handle_global_key(ev.key)) continue;
        if (ev.key == Key::Mouse) {
            auto hit = mouse_controls_.process_event(ev.mouse);
            if (hit.is_hover()) {
                continue;
            } else if (hit.is_click()) {
                if (hit.control_id >= 10 && hit.control_id <= 13) { cur_field = hit.control_id - 10; continue; }
                if (hit.control_id == 2) return ScreenAction::Back;
                if (hit.control_id == 3) return ScreenAction::Exit;
                if (hit.control_id == 1) {
                    cur_field = 4;
                    ev.key = Key::Enter;
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }

        if (ev.key == Key::Escape) {
            return ScreenAction::Exit;
        } else if (ev.key == Key::Up) {
            cur_field = (cur_field + 5) % 6;
        } else if (ev.key == Key::Down || ev.key == Key::Tab) {
            cur_field = (cur_field + 1) % 6;
        } else if (ev.key == Key::Enter) {
            if (cur_field == 5) {
                return ScreenAction::Back;
            } else if (cur_field < 4) {
                cur_field++;
            } else if (cur_field == 4) {
                if (user_data.username.empty()) {
                    error_hint = "Username cannot be empty!";
                    cur_field = 0;
                } else if (is_linux && user_data.password.empty()) {
                    error_hint = "Password is required for Linux installation!";
                    cur_field = 1;
                } else if (!user_data.password_matches()) {
                    error_hint = "Passwords do not match! Please re-enter.";
                    cur_field = 2;
                } else {
                    if (user_data.computer_name.empty()) {
                        user_data.computer_name = "litewin-pc";
                    }
                    user_data.finalized = true;
                    return ScreenAction::Next;
                }
            }
        } else if (ev.key == Key::Backspace || (ev.key == Key::Back && cur_field > 3)) {
            if (cur_field <= 3) {
                std::string* target = (cur_field == 0) ? &user_data.username :
                                      (cur_field == 1) ? &user_data.password :
                                      (cur_field == 2) ? &user_data.password_confirm :
                                                         &user_data.computer_name;
                if (!target->empty()) {
                    target->pop_back();
                    error_hint.clear();
                }
            } else if (ev.key == Key::Back) {
                return ScreenAction::Back;
            }
        } else if ((ev.key == Key::Char || ev.key == Key::Space || ev.key == Key::Back) && cur_field <= 3) {
            if (ev.ch >= 32 && ev.ch <= 126) {
                std::string* target = (cur_field == 0) ? &user_data.username :
                                      (cur_field == 1) ? &user_data.password :
                                      (cur_field == 2) ? &user_data.password_confirm :
                                                         &user_data.computer_name;
                char ch = ev.ch;
                bool valid_char = true;
                if (cur_field == 0) {
                    if (is_linux) {
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
                        if (!ok || (target->empty() && ch == '-')) valid_char = false;
                    } else {
                        const std::string_view forbidden = "/\\[]:;|=,+*?<>\"";
                        if (forbidden.find(ch) != std::string_view::npos || (target->empty() && (ch == ' ' || ch == '.'))) {
                            valid_char = false;
                        }
                    }
                } else if (cur_field == 3) {
                    if (is_linux) {
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    }
                    bool is_alnum = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
                    if (!is_alnum && (ch != '-' || target->empty())) {
                        valid_char = false;
                    }
                }
                const std::size_t max_len = (cur_field == 0 || cur_field == 3) ? 20 : 64;
                if (valid_char && target->size() < max_len) {
                    target->push_back(ch);
                    error_hint.clear();
                }
            }
        }
    }
}

void WinstallerTui::configure_progress(WindowsSettingsSession* settings, std::string compatibility) {
    compatibility_ = std::move(compatibility);
    windows_settings_ = settings;
    settings_tab_ = SettingsTab::Account;
    settings_field_ = 0;
    progress_initialized_ = false;
    progress_details_.reset();
    mouse_controls_.advance_generation();
}

void WinstallerTui::render_progress(const std::string& step_name, int step_pct,
    int overall_pct, const std::string& detail_msg, TuiUserData&, int) {
    term_.set_hover_area(mouse_controls_.hover_area());
    if (!progress_initialized_) { mouse_controls_.advance_generation(); progress_initialized_ = true; }
    term_.clear();
    mouse_controls_.clear();
    const int w = std::max(20, std::min(90, term_.cols() - 4));
    const int col = std::max(2, (term_.cols() - w) / 2);
    auto line = [&](int row, const std::string& text) {
        if (row < term_.rows()) term_.print_at(row, col + 2, clip_to_width(text, w - 4));
    };
    if (term_.rows() < 24 || term_.cols() < 80) {
        line(1, "Deploying: " + step_name + " (" + std::to_string(overall_pct) + "%)");
        line(2, "F6: smaller font | F8: Auto. Installation continues.");
        if (windows_settings_ && windows_settings_->completing())
            line(3, "Reboot decision in " + std::to_string(windows_settings_->remaining_seconds(WindowsSettingsSession::Clock::now())) + "s");
        draw_footer(); return;
    }
    term_.draw_box(1, col, w, 3, "Winstaller - Deployment", true);
    line(2, "OS installation: " + step_name);
    term_.draw_box(4, col, w, 6, "Pipeline status", true);
    line(5, detail_msg);
    const int bar_width = std::clamp(w - 30, 20, 50);
    auto bar = [&](int pct) {
        const int filled = std::clamp(pct, 0, 100) * bar_width / 100;
        std::string cells = "[" + AnsiTerminal::color_bold() + AnsiTerminal::color_green();
        for (int i = 0; i < filled; ++i) cells += "█";
        cells += AnsiTerminal::color_reset() + AnsiTerminal::color_slate();
        for (int i = filled; i < bar_width; ++i) cells += "░";
        return cells + AnsiTerminal::color_reset() + "] " + std::to_string(pct) + "%";
    };
    line(6, "Step : " + bar(step_pct));
    line(8, "Total: " + bar(overall_pct));
    term_.draw_box(10, col, w, 8, "Download information", true);
    const auto& d = download_status_;
    auto mib = [](uint64_t n) { return std::to_string(n / (1024*1024)) + " MiB"; };
    std::string phase = "Connecting / waiting";
    switch (d.phase) {
    case DownloadPhase::Downloading: phase="Downloading"; break;
    case DownloadPhase::Verifying: phase="Verifying file"; break;
    case DownloadPhase::Complete: phase="Download complete"; break;
    case DownloadPhase::Cached: phase="Using cached/local file"; break;
    case DownloadPhase::Failed: phase="Download failed"; break;
    default: break;
    }
    line(11, phase + " | " + mib(d.downloaded_bytes) + " / " + (d.total_bytes ? mib(d.total_bytes) : "unknown size"));
    std::ostringstream rate; rate << std::fixed << std::setprecision(1) << d.bytes_per_second/(1024*1024);
    std::string eta = "calculating / unavailable";
    if (d.eta_seconds && std::isfinite(*d.eta_seconds) && *d.eta_seconds >= 0 &&
        *d.eta_seconds < static_cast<double>(UINT64_MAX)) {
        const auto seconds = static_cast<uint64_t>(*d.eta_seconds);
        std::ostringstream duration;
        duration << seconds / 3600 << "h " << std::setfill('0')
                 << std::setw(2) << seconds / 60 % 60 << "m "
                 << std::setw(2) << seconds % 60 << "s";
        eta = duration.str();
    }
    line(12, "Speed: " + rate.str() + " MiB/s | Download ETA: " + eta);
    const std::string origin = d.origin == DownloadOrigin::Official ? "Official host" :
        (d.origin == DownloadOrigin::Mirror ? "Registered mirror" : "Unverified source");
    line(13, d.host.empty() ? "Server: not connected / local file" :
        "Server: " + clip_to_width(d.host, w - 15 - static_cast<int>(origin.size())) + " (" + origin + ")");
    std::string tls="Connection pending";
    switch(d.transport) {
    case DownloadTransport::HttpsVerified:tls="HTTPS certificate verified";break;
    case DownloadTransport::Http:tls="HTTP (unencrypted)";break;
    case DownloadTransport::Local:tls="Local file";break;
    case DownloadTransport::Failed:tls="Connection failed";break;
    default:break;
    }
    line(14, "Connection: " + tls);
    const char* hashes[]={"Not provided", "Pending", "Verifying", "Matched", "MISMATCH"};
    line(15, std::string("SHA-256: ") + hashes[static_cast<int>(d.hash)]);
    if (d.resumed_bytes) line(16, "Resumed: " + mib(d.resumed_bytes) + " (excluded from network speed)");
    if (windows_settings_) {
        const bool completing=windows_settings_->completing();
        std::string btn=completing ? " [ Reboot now / discard draft (Enter) ] " : " [ Settings (S / Enter) ] ";
        term_.print_at(22, col+3, AnsiTerminal::bg_teal()+btn+AnsiTerminal::color_reset());
        mouse_controls_.register_area(6000,22,col+3,static_cast<int>(btn.size()),1,0,"progress.settings");
        term_.draw_box(18, col, w, 4, "Hardware compatibility", true);
        const auto split = compatibility_.find(" | ");
        line(19, compatibility_.empty() ? "TPM: Unknown" : compatibility_.substr(0, split));
        line(20, split == std::string::npos ? "Secure Boot: Unknown" : compatibility_.substr(split + 3));
        if (completing) line(23, "Decision in " + std::to_string(windows_settings_->remaining_seconds(WindowsSettingsSession::Clock::now())) + "s. Timeout keeps last saved values.");
        if (windows_settings_->is_open()) {
            const auto& draft = windows_settings_->draft();
            term_.fill_rect(4, col, w, 20, ' ');
            term_.draw_box(4, col, w, 20, completing ? "Save or discard before reboot" : "Windows settings - download continues", true);
            mouse_controls_.clear();
            auto control = [&](int id, int row, int offset, const std::string& label, int width) {
                const auto style = settings_field_ == id ? AnsiTerminal::bg_teal() : "";
                term_.print_at(row, col + offset, style + clip_to_width(label, width) + AnsiTerminal::color_reset());
                mouse_controls_.register_area(6100 + id, row, col + offset, width, 1, id,
                    "settings.field." + std::to_string(id));
            };
            const bool account = settings_tab_ == SettingsTab::Account;
            control(11, 6, 3, account ? "[* Account ]" : "[ Account ]", 13);
            control(12, 6, 18, account ? "[ Privacy ]" : "[* Privacy ]", 13);
            if (account) {
                const bool local = draft.options.create_local_account;
                control(10, 8, 3, local ? "Account setup: Create a local account (skip Microsoft sign-in)" :
                    "Account setup: Use Windows account setup", w - 6);
                if (local) {
                    control(0, 9, 3, "Username: " + draft.options.username, w - 6);
                    control(1, 10, 3, "Password: " + std::string(draft.options.password.size(), '*'), w - 6);
                    control(2, 11, 3, "Confirm:  " + std::string(draft.password_confirm.size(), '*'), w - 6);
                }
                control(3, 12, 3, "PC name:  " + draft.options.computer_name, w - 6);
                line(14, local ? "Blank PC name generates a name; blank password allowed." :
                    "Choose or sign in to an account during Windows first setup.");
            } else {
                auto check = [&](bool enabled) { return enabled ? "[" + term_.check_mark() + "] " : "[ ] "; };
                control(4, 8, 3, check(draft.options.disable_telemetry) + "Minimize optional diagnostics", w - 6);
                control(5, 9, 3, check(draft.options.disable_advertising_id) + "Disable advertising ID", w - 6);
                control(6, 10, 3, check(draft.options.disable_tailored_experiences) + "Disable tailored experiences", w - 6);
            }
            line(15, "Left/Right: switch tab | Tab/Up/Down: focus | Space/Enter: select");
            line(16, windows_settings_->error());
            if (completing) line(17, "Reboot decision in " + std::to_string(windows_settings_->remaining_seconds(WindowsSettingsSession::Clock::now())) + "s; timeout discards edits.");
            const std::string buttons[]={completing ? "[Save & reboot]" : "[Save]",completing ? "[Discard & reboot]" : "[Cancel]","[Defaults]"};
            for(int i=0;i<3;++i) {
                const int c=col+3+i*21;
                term_.print_at(19,c,(settings_field_==7+i ? AnsiTerminal::bg_teal() : "")+buttons[i]+AnsiTerminal::color_reset());
                mouse_controls_.register_area(6107+i,19,c,static_cast<int>(buttons[i].size()),1,7+i,"settings.action."+std::to_string(i));
            }
        }
    } else {
        line(22,"Linux account settings are locked to the values confirmed before installation.");
    }
    draw_footer();
    if(progress_details_) draw_details_overlay(*progress_details_);
}

std::vector<int> WinstallerTui::settings_controls() const {
    if (settings_tab_ == SettingsTab::Privacy) return {11, 12, 4, 5, 6, 7, 8, 9};
    if (windows_settings_->draft().options.create_local_account)
        return {11, 12, 10, 0, 1, 2, 3, 7, 8, 9};
    return {11, 12, 10, 3, 7, 8, 9};
}

bool WinstallerTui::handle_user_data_input(TuiUserData&, int&) {
    bool changed=false;
    // Bounded input draining lets a busy input stream yield to completion deadlines.
    for(int n=0;n<32;++n) {
        auto ev=read_control_key(0,windows_settings_ && windows_settings_->is_open() ? 6100+settings_field_ : 6000);
        if(ev.key==Key::None) break;
        if(handle_global_key(ev.key)) {changed=true;continue;}
        bool activate=false;
        if(ev.key==Key::Mouse) {
            const int previous_hover = mouse_controls_.last_hover_id();
            auto hit = mouse_controls_.process_event(ev.mouse);
            if (ev.mouse.kind == MouseKind::Move && previous_hover != mouse_controls_.last_hover_id()) {
                term_.set_hover_area(mouse_controls_.hover_area());
                changed = true;
            }
            if(!hit.is_click()) continue;
            if(hit.control_id==6000 && windows_settings_) {
                if(windows_settings_->completing()) windows_settings_->cancel();
                else windows_settings_->open();
                changed=true;break;
            }
            if(hit.control_id>=6100 && hit.control_id<=6112) {
                settings_field_=hit.data;activate=settings_field_>=4;changed=true;
            } else continue;
        }
        if(!windows_settings_) continue; // Linux never edits account data here.
        auto& session=*windows_settings_;
        if(!session.is_open()) {
            if(ev.key==Key::Enter || (ev.key==Key::Char && (ev.ch=='s'||ev.ch=='S'))) {
                if(session.completing()) session.cancel(); else session.open();
                changed=true;break;
            }
            continue;
        }
        if(ev.key==Key::Escape) {session.cancel();changed=true;break;}
        if (ev.key == Key::Left || ev.key == Key::Right) {
            settings_tab_ = ev.key == Key::Left ? SettingsTab::Account : SettingsTab::Privacy;
            settings_field_ = settings_tab_ == SettingsTab::Account ? 10 : 4;
            changed = true;
            continue;
        }
        auto move_focus = [&](int direction) {
            const auto controls = settings_controls();
            const auto it = std::find(controls.begin(), controls.end(), settings_field_);
            const auto size = static_cast<int>(controls.size());
            const auto index = static_cast<int>(it - controls.begin());
            settings_field_ = controls[(index + direction + size) % size];
        };
        if (ev.key == Key::Tab || ev.key == Key::Down || ev.key == Key::Up) {
            move_focus(ev.key == Key::Up ? -1 : 1);
            changed = true;
            continue;
        }
        activate=activate || ev.key==Key::Enter || (ev.key==Key::Space && settings_field_>=4);
        auto& draft=session.draft();
        if(activate) {
            if(settings_field_<4) move_focus(1);
            else if(settings_field_==4) draft.options.disable_telemetry=!draft.options.disable_telemetry;
            else if(settings_field_==5) draft.options.disable_advertising_id=!draft.options.disable_advertising_id;
            else if(settings_field_==6) draft.options.disable_tailored_experiences=!draft.options.disable_tailored_experiences;
            else if(settings_field_==7) {static_cast<void>(session.save()); changed=true;break;}
            else if(settings_field_==8) {session.cancel();changed=true;break;}
            else if (settings_field_ == 9) session.restore_defaults();
            else if (settings_field_ == 10) draft.options.create_local_account = !draft.options.create_local_account;
            else {
                settings_tab_ = settings_field_ == 11 ? SettingsTab::Account : SettingsTab::Privacy;
                settings_field_ = settings_tab_ == SettingsTab::Account ? 10 : 4;
            }
            changed=true;continue;
        }
        if(settings_field_<4) {
            std::string* targets[]={&draft.options.username,&draft.options.password,&draft.password_confirm,&draft.options.computer_name};
            auto& field=*targets[settings_field_];
            if(ev.key==Key::Backspace && !field.empty()) {field.pop_back();changed=true;}
            if((ev.key==Key::Char || ev.key==Key::Space || ev.key==Key::Back) && ev.ch>=32 && ev.ch<=126) {
                const size_t max=settings_field_==0 ? 20 : settings_field_==3 ? 15 : 64;
                if(field.size()<max) {field+=ev.ch;changed=true;}
            }
        }
    }
    return changed;
}

bool WinstallerTui::prompt_reboot_countdown(int timeout_seconds) {
    progress_initialized_=false; progress_details_.reset();
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "reboot");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(std::max(0,timeout_seconds));
    int last_remaining = -1;
    bool redraw = true;
    while (std::chrono::steady_clock::now() < deadline) {
        const int remaining = static_cast<int>(std::chrono::ceil<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now()).count());
        if (redraw || remaining != last_remaining) {
            term_.clear();
            const int col = 2;
            term_.print_at(2, col, "Installation completed. Reboot in " + std::to_string(remaining) + "s.");
            const std::string label = "Account settings are locked. ";
            const std::string button = "[ Reboot now (Enter) ]";
            term_.print_at(4, col, label + button);
            mouse_controls_.clear();
            mouse_controls_.register_area(1, 4, col + static_cast<int>(label.size()), static_cast<int>(button.size()), 1);
            draw_footer();
            last_remaining = remaining;
        }
        const auto ev = term_.read_key(50);
        if (ev.key == Key::Enter) return true;
        if (ev.key == Key::Mouse && mouse_controls_.process_event(ev.mouse).is_click()) return true;
        redraw = handle_global_key(ev.key);
    }
    return true;
}

} // namespace litewin

namespace litewin {
std::vector<std::string> WinstallerTui::catalog_tabs()const{return catalog_?catalog_->tabs():get_os_tabs();}
std::vector<std::pair<std::string,std::string>> WinstallerTui::catalog_items(const std::string& tab)const{return catalog_?catalog_->products(tab):get_tab_os_items(tab);}
void WinstallerTui::select_remote(TuiEditionChoice& choice,const RemoteTarget& target) {
    choice.catalog_revision=catalog_?catalog_->revision:0;
    choice.remote_target=target;choice.edition_name=target.name;choice.os_tab=target.tab;
    choice.is_linux=!target.windows;choice.linux_distro_id=target.windows?"":target.id;
    choice.language=target.language;choice.reuse_existing_esd=false;
}
bool WinstallerTui::show_update_modal() {
    if (!updates_) return false;
    MouseControls::SceneGuard guard(mouse_controls_, &term_.mouse(), "update");
    auto snapshot = updates_->snapshot();
    if (!snapshot.release) return false;
    const auto release = snapshot.release;
    const auto medium = updates_->inspect_media();
    const bool eligible = medium.eligible && release->compatible;
    bool dirty = true, full = true;
    while (true) {
        const auto current = updates_->snapshot();
        if (current.state != snapshot.state) {
            mouse_controls_.advance_generation();
            term_.mouse().reset();
            dirty = true;
        }
        dirty = dirty || current.bytes != snapshot.bytes || current.error != snapshot.error;
        snapshot = current;
        const bool busy = snapshot.state == UpdateState::Downloading || snapshot.state == UpdateState::Committing;
        if (dirty) {
            const auto geom = ScreenGeometry::compute(term_.rows(), term_.cols(), 76, 18, 90);
            const auto row = geom.content_box.row, col = geom.content_box.col;
            const auto w = geom.content_box.width, h = geom.content_box.height;
            if (full) {
                draw_header("Winstaller Update");
                term_.draw_box(row, col, w, h, "Optional Update", true);
                term_.print_at(row+2, col+4, clip_to_width("Current: "+updates_->current_version()+"  ->  New: "+release->version, w-8));
                term_.print_at(row+4, col+4, clip_to_width(release->notes, w-8));
                term_.print_at(row+6, col+4, "Release: "+std::to_string(release->total_bytes()/(1024*1024))+" MiB; required: "+std::to_string((release->total_bytes()+64ULL*1024*1024)/(1024*1024))+" MiB");
                term_.print_at(row+7, col+4, clip_to_width(release->compatible ? medium.detail : release->incompatible_reason, w-8));
                if (!eligible) term_.print_at(row+11, col+4, "Releases: github.com/yskyh06/Winstaller/releases");
                full = false;
            }
            std::string state = "The current version stays available in the boot menu.";
            if (busy) state = (snapshot.state == UpdateState::Committing ? "Committing verified files... " : "Downloading / verifying: ")+std::to_string(snapshot.total ? 100*snapshot.bytes/snapshot.total : 0)+"%";
            if (snapshot.state == UpdateState::Ready) state = "Ready. Restart now, or continue with this session.";
            if (snapshot.state == UpdateState::Failed || snapshot.state == UpdateState::Cancelled) state = snapshot.error;
            term_.fill_rect(row+9, col+4, w-8, 2, ' ');
            term_.print_at(row+9, col+4, clip_to_width(state, w-8));
            if (busy || snapshot.state == UpdateState::Ready) {
                const int width = w-8;
                const int filled = snapshot.total ? static_cast<int>(std::min<std::uint64_t>(width, width*snapshot.bytes/snapshot.total)) : 0;
                std::string bar = AnsiTerminal::color_green();
                for (int i = 0; i < width; ++i) bar += i < filled ? "█" : "░";
                term_.print_at(row+10, col+4, bar+AnsiTerminal::color_reset());
            }
            const std::string first = snapshot.state == UpdateState::Ready ? " [ Restart now (R) ] " :
                snapshot.state == UpdateState::Committing ? " [ Committing... ] " :
                busy ? " [ Cancel (Esc) ] " : eligible ? " [ Download (Enter) ] " : " [ Update unavailable ] ";
            term_.fill_rect(row+h-3, col+4, w-8, 1, ' ');
            term_.print_at(row+h-3, col+4, first);
            term_.print_at(row+h-3, col+40, " [ Later (Esc) ] ");
            mouse_controls_.clear();
            mouse_controls_.register_area(1, row+h-3, col+4, static_cast<int>(first.size()), 1);
            mouse_controls_.register_area(2, row+h-3, col+40, 17, 1);
            dirty = false;
        }
        auto ev = read_control_key(100, 1);
        if (handle_global_key(ev.key)) { full = dirty = true; continue; }
        if (ev.key == Key::Mouse) {
            const auto hit = mouse_controls_.process_event(ev.mouse);
            if (!hit.is_click()) continue;
            if (hit.control_id == 1 && snapshot.state == UpdateState::Ready) return true;
            ev.key = hit.control_id == 1 ? Key::Enter : hit.control_id == 2 ? Key::Escape : Key::None;
        }
        if (ev.key == Key::Escape) {
            if (snapshot.state == UpdateState::Committing) continue;
            if (busy) { updates_->cancel(); continue; }
            return false;
        }
        if (snapshot.state == UpdateState::Ready && ev.key == Key::Char && (ev.ch == 'r' || ev.ch == 'R')) return true;
        if (ev.key == Key::Enter && snapshot.state != UpdateState::Ready) {
            if (snapshot.state == UpdateState::Downloading) updates_->cancel();
            else if (!busy && eligible) { updates_->begin_update(release->id); dirty = true; }
        }
        const auto next = updates_->snapshot();
        dirty = dirty || next.state != snapshot.state || next.bytes != snapshot.bytes || next.error != snapshot.error;
        // Product updates have no automatic reboot deadline.
    }
}
} // namespace litewin

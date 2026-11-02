#include "ui/winstaller_tui.hpp"
#include "platform/network_inspector.hpp"
#include "platform/network_probe.hpp"
#include "platform/network_configurator.hpp"
#include "platform/process_runner.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace litewin {

WinstallerTui::WinstallerTui(AnsiTerminal& term) : term_(term) {}

void WinstallerTui::draw_header(const std::string& subtitle) {
    term_.clear();
    int w = 76;
    int col = std::max(2, (term_.cols() - w) / 2);
    term_.draw_box(1, col, w, 3, "Winstaller", true);
    term_.print_at(2, col + 3, AnsiTerminal::color_bold() + AnsiTerminal::color_teal() + "✦ Winstaller" + AnsiTerminal::color_reset() + " - Automated Windows Deployment Tool");
    if (!subtitle.empty()) {
        int sub_col = col + w - static_cast<int>(subtitle.size()) - 3;
        if (sub_col > col + 35) {
            term_.print_at(2, sub_col, AnsiTerminal::color_slate() + subtitle + AnsiTerminal::color_reset());
        }
    }
}

ScreenAction WinstallerTui::prompt_eula() {
    term_.update_size();
    draw_header("Disclaimer & EULA");

    int w = 76;
    int h = 18;
    int col = std::max(2, (term_.cols() - w) / 2);
    int row = std::max(5, (term_.rows() - h) / 2);

    term_.draw_box(row, col, w, h, "Disclaimer & Terms of Use", true);

    term_.print_at(row + 2, col + 4, AnsiTerminal::color_bold() + AnsiTerminal::color_gold() + "Notice & Liability Agreement" + AnsiTerminal::color_reset());
    term_.print_at(row + 4, col + 4, "1. Winstaller is an automated zero-touch deployment utility.");
    term_.print_at(row + 5, col + 4, "2. ALL DATA, partitions, and files on the target drive will be");
    term_.print_at(row + 6, col + 4, "   PERMANENTLY DESTROYED and replaced with Windows 11.");
    term_.print_at(row + 7, col + 4, "3. The authors assume NO LIABILITY for data loss or hardware issues.");
    term_.print_at(row + 9, col + 4, "Ensure all essential data is backed up before continuing.");
    term_.print_at(row + 11, col + 4, AnsiTerminal::color_lavender() + "Do you accept these terms and wish to proceed?" + AnsiTerminal::color_reset());

    int selected = 0; // 0 = Agree, 1 = Exit
    while (true) {
        if (selected == 0) {
            term_.print_at(row + 14, col + 8, AnsiTerminal::bg_teal() + " [ Agree & Continue (Enter) ] " + AnsiTerminal::color_reset());
            term_.print_at(row + 14, col + 44, AnsiTerminal::color_slate() + " [ Exit & Reboot (Esc) ] " + AnsiTerminal::color_reset());
        } else {
            term_.print_at(row + 14, col + 8, AnsiTerminal::color_slate() + " [ Agree & Continue (Enter) ] " + AnsiTerminal::color_reset());
            term_.print_at(row + 14, col + 44, AnsiTerminal::bg_red() + AnsiTerminal::color_bold() + " [ Exit & Reboot (Esc) ] " + AnsiTerminal::color_reset());
        }

        auto ev = term_.read_key(-1);
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
    std::string status_notice;
    while (true) {
        term_.update_size();
        draw_header("Network Setup");

        int w = 76;
        int h = 18;
        int col = std::max(2, (term_.cols() - w) / 2);
        int row = std::max(5, (term_.rows() - h) / 2);

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
            term_.print_at(row + 4, col + 4, "Winstaller is connected and ready to download Windows images.");
            term_.print_at(row + 6, col + 4, "Press Enter to continue, W for Network Manager, or B to go Back.");

            term_.print_at(row + 14, col + 4, AnsiTerminal::bg_teal() + " [ Continue (Enter) ] " + AnsiTerminal::color_reset());
            term_.print_at(row + 14, col + 28, " [ Wi-Fi Tool (W) ] ");
            term_.print_at(row + 14, col + 50, " [ Back (B) ] ");
            term_.print_at(row + 14, col + 64, " [ Esc ] ");

            while (true) {
                auto ev = term_.read_key(-1);
                if (ev.key == Key::Enter) return ScreenAction::Next;
                if (ev.key == Key::Back) return ScreenAction::Back;
                if (ev.key == Key::Escape) return ScreenAction::Exit;
                if (ev.key == Key::Char && (ev.ch == 'w' || ev.ch == 'W')) {
                    term_.show_cursor();
                    term_.exit_raw_mode();
                    (void)open_network_settings(NetworkConfigUiMode::tui_only);
                    term_.enter_raw_mode();
                    term_.hide_cursor();
                    break;
                }
            }
            continue;
        }

        term_.print_at(row + 2, col + 4, AnsiTerminal::color_yellow() + "[!] No active internet connection detected." + AnsiTerminal::color_reset() + "              ");
        term_.print_at(row + 4, col + 4, "You may connect to Wi-Fi now, or continue if you have a local ESD cache.");
        if (!status_notice.empty()) {
            term_.print_at(row + 5, col + 4, AnsiTerminal::color_red() + status_notice.substr(0, 68) + AnsiTerminal::color_reset());
        }
        term_.print_at(row + 7, col + 6, "• Press " + AnsiTerminal::color_teal() + "Enter" + AnsiTerminal::color_reset() + " to open Wi-Fi / Network Manager (nmtui / iwctl)");
        term_.print_at(row + 8, col + 6, "• Press " + AnsiTerminal::color_teal() + "R" + AnsiTerminal::color_reset() + " to rescan network connectivity");
        term_.print_at(row + 9, col + 6, "• Press " + AnsiTerminal::color_teal() + "S" + AnsiTerminal::color_reset() + " to skip and continue in Offline Mode");
        term_.print_at(row + 10, col + 6, "• Press " + AnsiTerminal::color_teal() + "B" + AnsiTerminal::color_reset() + " to go Back  |  " + AnsiTerminal::color_teal() + "Esc" + AnsiTerminal::color_reset() + " to Reboot");

        term_.print_at(row + 14, col + 4, AnsiTerminal::bg_teal() + " [ Open Wi-Fi (Enter) ] " + AnsiTerminal::color_reset());
        term_.print_at(row + 14, col + 29, " [ Rescan (R) ] ");
        term_.print_at(row + 14, col + 46, " [ Skip (S) ] ");
        term_.print_at(row + 14, col + 61, " [ Back (B) ] ");

        bool re_probe = false;
        while (!re_probe) {
            auto ev = term_.read_key(-1);
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

std::pair<ScreenAction, std::string> WinstallerTui::prompt_disk_selection(const std::vector<DiskInfo>& disks) {
    term_.update_size();
    draw_header("Target Disk Selection");

    int w = 76;
    int h = 18;
    int col = std::max(2, (term_.cols() - w) / 2);
    int row = std::max(5, (term_.rows() - h) / 2);

    if (disks.empty()) {
        term_.draw_box(row, col, w, h, "No Eligible Disk Found", true);
        term_.print_at(row + 3, col + 4, AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "[!] No eligible internal storage drives (>= 32 GB) found." + AnsiTerminal::color_reset());
        term_.print_at(row + 5, col + 4, "Please verify that a target storage drive of at least 32 GB is attached");
        term_.print_at(row + 6, col + 4, "and visible to the controller (NVMe / SATA / VirtIO / SCSI).");
        term_.print_at(row + 14, col + 4, AnsiTerminal::bg_teal() + " [ Rescan Drives (Enter) ] " + AnsiTerminal::color_reset());
        term_.print_at(row + 14, col + 34, " [ Back (B) ] ");
        term_.print_at(row + 14, col + 50, " [ Exit & Reboot (Esc) ] ");

        while (true) {
            auto ev = term_.read_key(-1);
            if (ev.key == Key::Enter || (ev.key == Key::Char && (ev.ch == 'r' || ev.ch == 'R'))) {
                return {ScreenAction::Next, ""};
            }
            if (ev.key == Key::Back) return {ScreenAction::Back, ""};
            if (ev.key == Key::Escape) return {ScreenAction::Exit, ""};
        }
    }

    term_.draw_box(row, col, w, h, "Target Storage Disk Selection", true);
    term_.print_at(row + 2, col + 4, AnsiTerminal::color_bold() + "Select the target drive for Windows 11 installation:" + AnsiTerminal::color_reset());
    term_.print_at(row + 3, col + 4, AnsiTerminal::color_red() + "WARNING: All partitions and files on this drive will be wiped!" + AnsiTerminal::color_reset());

    int selected_idx = 0;
    while (true) {
        for (std::size_t i = 0; i < disks.size() && i < 5; ++i) {
            int d_row = row + 5 + static_cast<int>(i) * 2;
            const auto& d = disks[i];
            std::string gb = std::to_string(d.size_bytes / (1024ULL * 1024 * 1024)) + " GB";
            std::string model = d.model.value_or(d.name);
            std::string tran = d.transport.value_or(d.path.find("nvme") != std::string::npos ? "nvme" : "disk");
            std::string label = d.path + " [" + model + ", " + gb + "] (" + tran + ")";
            if (label.size() > 66) label = label.substr(0, 63) + "...";

            if (static_cast<int>(i) == selected_idx) {
                term_.print_at(d_row, col + 4, AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + " > (*) " + label + AnsiTerminal::color_reset());
            } else {
                term_.print_at(d_row, col + 4, AnsiTerminal::color_slate() + "   ( ) " + label + "    " + AnsiTerminal::color_reset());
            }
        }

        term_.print_at(row + 15, col + 4, AnsiTerminal::bg_teal() + " [ Select (Enter) ] " + AnsiTerminal::color_reset());
        term_.print_at(row + 15, col + 26, " [ Back (B) ] ");
        term_.print_at(row + 15, col + 42, " [ Exit & Reboot (Esc) ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Up) {
            if (selected_idx > 0) --selected_idx;
        } else if (ev.key == Key::Down) {
            if (selected_idx + 1 < static_cast<int>(disks.size())) ++selected_idx;
        } else if (ev.key == Key::Back) {
            return {ScreenAction::Back, ""};
        } else if (ev.key == Key::Escape) {
            return {ScreenAction::Exit, ""};
        } else if (ev.key == Key::Enter) {
            break;
        }
    }

    // Safety Confirmation Modal
    std::string chosen_disk = disks[selected_idx].path;
    int m_w = 64;
    int m_h = 10;
    int m_col = std::max(2, (term_.cols() - m_w) / 2);
    int m_row = std::max(6, (term_.rows() - m_h) / 2);

    term_.fill_rect(m_row, m_col, m_w, m_h, ' ');
    term_.draw_box(m_row, m_col, m_w, m_h, "CONFIRM DISK FORMATTING", true);
    term_.print_at(m_row + 2, m_col + 4, AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "Are you sure you want to format this drive?" + AnsiTerminal::color_reset());
    term_.print_at(m_row + 3, m_col + 4, "Target Drive: " + AnsiTerminal::color_bold() + AnsiTerminal::color_gold() + chosen_disk + AnsiTerminal::color_reset());
    term_.print_at(m_row + 4, m_col + 4, "All existing data will be permanently destroyed!");

    int confirm_sel = 1; // Default to Cancel for safety
    while (true) {
        if (confirm_sel == 0) {
            term_.print_at(m_row + 7, m_col + 6, AnsiTerminal::bg_red() + AnsiTerminal::color_bold() + " [ YES, FORMAT & INSTALL ] " + AnsiTerminal::color_reset());
            term_.print_at(m_row + 7, m_col + 38, " [ CANCEL ] ");
        } else {
            term_.print_at(m_row + 7, m_col + 6, " [ YES, FORMAT & INSTALL ] ");
            term_.print_at(m_row + 7, m_col + 38, AnsiTerminal::bg_teal() + " [ CANCEL ] " + AnsiTerminal::color_reset());
        }

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Left || ev.key == Key::Right || ev.key == Key::Tab) {
            confirm_sel = 1 - confirm_sel;
        } else if (ev.key == Key::Enter) {
            if (confirm_sel == 0) return {ScreenAction::Next, chosen_disk};
            return prompt_disk_selection(disks);
        } else if (ev.key == Key::Back) {
            return prompt_disk_selection(disks);
        } else if (ev.key == Key::Escape) {
            return {ScreenAction::Exit, ""};
        }
    }
}

std::pair<ScreenAction, TuiEditionChoice> WinstallerTui::prompt_edition_selection(const std::string& detected_oem_edition, bool cached_esd_found) {
    term_.update_size();
    draw_header("Windows Edition Selection");

    int w = 76;
    int h = 19;
    int col = std::max(2, (term_.cols() - w) / 2);
    int row = std::max(5, (term_.rows() - h) / 2);

    term_.draw_box(row, col, w, h, "Windows Edition & Options", true);

    term_.print_at(row + 2, col + 4, "System Architecture: " + AnsiTerminal::color_bold() + "x86_64 (64-bit UEFI)" + AnsiTerminal::color_reset());
    std::string oem_str = detected_oem_edition.empty() ? "None (Default to Pro)" : (detected_oem_edition + " (Detected)");
    term_.print_at(row + 3, col + 4, "ACPI OEM License   : " + AnsiTerminal::color_green() + oem_str + AnsiTerminal::color_reset());

    TuiEditionChoice choice;
    choice.edition_index = 6;
    choice.edition_name = "Windows 11 Pro";
    choice.language = "ko-KR";
    choice.reuse_existing_esd = cached_esd_found;

    const std::vector<std::pair<std::string, std::string>> languages = {
        {"ko-KR", "Korean (ko-KR)"},
        {"en-US", "English (en-US)"},
        {"ja-JP", "Japanese (ja-JP)"},
        {"zh-CN", "Chinese Simplified (zh-CN)"},
        {"de-DE", "German (de-DE)"},
        {"fr-FR", "French (fr-FR)"},
        {"es-ES", "Spanish (es-ES)"}
    };
    int lang_idx = 0;
    int cur_row = 0; // 0 = Edition, 1 = Cache, 2 = Language

    while (true) {
        auto row_marker = [&](int r, const std::string& label) {
            if (cur_row == r) {
                return AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + "> " + label + " < [ACTIVE]" + AnsiTerminal::color_reset() + "   ";
            }
            return AnsiTerminal::color_bold() + "  " + label + AnsiTerminal::color_reset() + "              ";
        };

        term_.print_at(row + 5, col + 4, row_marker(0, "[1] Windows Edition:"));
        if (choice.edition_index == 6) {
            term_.print_at(row + 6, col + 6, AnsiTerminal::color_teal() + "(*) Windows 11 Pro (Index 6) [Recommended]" + AnsiTerminal::color_reset());
            term_.print_at(row + 7, col + 6, AnsiTerminal::color_slate() + "( ) Windows 11 Home (Index 4)             " + AnsiTerminal::color_reset());
        } else {
            term_.print_at(row + 6, col + 6, AnsiTerminal::color_slate() + "( ) Windows 11 Pro (Index 6)             " + AnsiTerminal::color_reset());
            term_.print_at(row + 7, col + 6, AnsiTerminal::color_teal() + "(*) Windows 11 Home (Index 4) [Selected]   " + AnsiTerminal::color_reset());
        }

        term_.print_at(row + 9, col + 4, row_marker(1, "[2] ESD Source:"));
        if (cached_esd_found) {
            std::string esd_check = choice.reuse_existing_esd ? "[X] Reuse cached ESD on P5 (Fastest, skip download)" : "[ ] Fresh download from Microsoft CDN      ";
            term_.print_at(row + 10, col + 6, AnsiTerminal::color_green() + esd_check + AnsiTerminal::color_reset());
        } else {
            term_.print_at(row + 10, col + 6, AnsiTerminal::color_slate() + "[!] No local cache found (Will download from official CDN)" + AnsiTerminal::color_reset());
        }

        term_.print_at(row + 12, col + 4, row_marker(2, "[3] Target Language:"));
        std::string lang_label = "<< " + languages[lang_idx].second + " >> (Space / Left / Right)";
        term_.print_at(row + 13, col + 6, (cur_row == 2 ? (AnsiTerminal::bg_teal() + AnsiTerminal::color_bold()) : AnsiTerminal::color_gold()) + " " + lang_label + " " + AnsiTerminal::color_reset() + "          ");

        term_.print_at(row + 16, col + 4, AnsiTerminal::bg_teal() + " [ Start (Enter) ] " + AnsiTerminal::color_reset());
        term_.print_at(row + 16, col + 24, " [ Up/Down: Move ] ");
        term_.print_at(row + 16, col + 44, " [ Back (B) ] ");
        term_.print_at(row + 16, col + 59, " [ Exit (Esc) ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Up) {
            cur_row = (cur_row + 2) % 3;
            if (cur_row == 1 && !cached_esd_found) cur_row = 0;
        } else if (ev.key == Key::Down || ev.key == Key::Tab) {
            cur_row = (cur_row + 1) % 3;
            if (cur_row == 1 && !cached_esd_found) cur_row = 2;
        } else if (ev.key == Key::Space || ev.key == Key::Left || ev.key == Key::Right) {
            if (cur_row == 0) {
                choice.edition_index = (choice.edition_index == 6) ? 4 : 6;
                choice.edition_name = (choice.edition_index == 6) ? "Windows 11 Pro" : "Windows 11 Home";
            } else if (cur_row == 1 && cached_esd_found) {
                choice.reuse_existing_esd = !choice.reuse_existing_esd;
            } else if (cur_row == 2) {
                int n = static_cast<int>(languages.size());
                if (ev.key == Key::Left) {
                    lang_idx = (lang_idx + n - 1) % n;
                } else {
                    lang_idx = (lang_idx + 1) % n;
                }
                choice.language = languages[lang_idx].first;
                if (lang_idx != 0 && cached_esd_found) {
                    choice.reuse_existing_esd = false;
                }
            }
        } else if (ev.key == Key::Enter) {
            return {ScreenAction::Next, choice};
        } else if (ev.key == Key::Back) {
            return {ScreenAction::Back, choice};
        } else if (ev.key == Key::Escape) {
            return {ScreenAction::Exit, choice};
        }
    }
}

void WinstallerTui::render_progress(
    const std::string& step_name,
    int step_pct,
    int overall_pct,
    const std::string& detail_msg,
    TuiUserData& user_data,
    int active_field
) {
    term_.clear();
    int w = 76;
    int col = std::max(2, (term_.cols() - w) / 2);

    term_.draw_box(1, col, w, 3, "Winstaller - Deployment Active", true);
    term_.print_at(2, col + 3, AnsiTerminal::color_bold() + AnsiTerminal::color_teal() + "✦ Windows 11 Zero-Touch Installation in Progress" + AnsiTerminal::color_reset());

    // Pipeline Box
    term_.draw_box(4, col, w, 8, "Pipeline Status (" + step_name + ")", true);
    std::string safe_msg = detail_msg.substr(0, 68);
    term_.print_at(5, col + 4, safe_msg);

    // Current Step Progress Bar
    int bar_width = 46;
    int filled_step = std::clamp((step_pct * bar_width) / 100, 0, bar_width);
    std::string step_bar;
    for (int i = 0; i < bar_width; ++i) {
        step_bar += (i < filled_step) ? "█" : "░";
    }
    term_.print_at(7, col + 4, "Current Step : [" + AnsiTerminal::color_teal() + step_bar + AnsiTerminal::color_reset() + "] " + std::to_string(step_pct) + "%");

    // Overall Progress Bar
    int filled_overall = std::clamp((overall_pct * bar_width) / 100, 0, bar_width);
    std::string overall_bar;
    for (int i = 0; i < bar_width; ++i) {
        overall_bar += (i < filled_overall) ? "█" : "░";
    }
    term_.print_at(9, col + 4, "Total Progress: [" + AnsiTerminal::color_lavender() + overall_bar + AnsiTerminal::color_reset() + "] " + std::to_string(overall_pct) + "%");

    // Interactive Customization Box during deploy
    term_.draw_box(12, col, w, 11, "User Environment Customization (Live Input Enabled)", true);
    term_.print_at(13, col + 4, AnsiTerminal::color_slate() + "* Defaults ('User', no password, random PC) apply if left unchanged." + AnsiTerminal::color_reset());

    auto field_style = [&](int f) {
        return (active_field == f) ? (AnsiTerminal::bg_teal() + AnsiTerminal::color_bold()) : "";
    };

    auto pad15 = [](const std::string& s) {
        return s.size() < 15 ? (s + std::string(15 - s.size(), ' ')) : s.substr(0, 15);
    };

    std::string masked_pw(user_data.password.size(), '*');
    std::string masked_pw2(user_data.password_confirm.size(), '*');

    std::string pw_status;
    if (user_data.password.empty() && user_data.password_confirm.empty()) {
        pw_status = AnsiTerminal::color_slate() + "(No password)" + AnsiTerminal::color_reset();
    } else if (user_data.password_matches()) {
        pw_status = AnsiTerminal::color_green() + AnsiTerminal::color_bold() + "[OK: Matched]" + AnsiTerminal::color_reset();
    } else {
        pw_status = AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "[!: Mismatch]" + AnsiTerminal::color_reset();
    }

    term_.print_at(15, col + 4, "1. Username         : " + field_style(0) + "[" + pad15(user_data.username) + "]" + AnsiTerminal::color_reset());
    term_.print_at(16, col + 4, "2. Password         : " + field_style(1) + "[" + pad15(masked_pw) + "]" + AnsiTerminal::color_reset());
    term_.print_at(17, col + 4, "3. Confirm Password : " + field_style(2) + "[" + pad15(masked_pw2) + "]" + AnsiTerminal::color_reset() + " " + pw_status);
    term_.print_at(18, col + 4, "4. Computer Name    : " + field_style(3) + "[" + pad15(user_data.computer_name) + "]" + AnsiTerminal::color_reset());

    term_.print_at(20, col + 4, AnsiTerminal::color_slate() + "[Tab/Up/Down: Move]   [Type/Backspace: Edit]   [Enter: Confirm]" + AnsiTerminal::color_reset());
}

bool WinstallerTui::handle_user_data_input(TuiUserData& user_data, int& active_field) {
    bool changed = false;
    while (true) {
        auto ev = term_.read_key(0); // Non-blocking drain
        if (ev.key == Key::None) break;

        auto get_target_field = [&]() -> std::string* {
            switch (active_field) {
            case 0: return &user_data.username;
            case 1: return &user_data.password;
            case 2: return &user_data.password_confirm;
            default: return &user_data.computer_name;
            }
        };

        if (ev.key == Key::Tab || ev.key == Key::Down) {
            active_field = (active_field + 1) % 4;
            changed = true;
        } else if (ev.key == Key::Up) {
            active_field = (active_field + 3) % 4;
            changed = true;
        } else if (ev.key == Key::Enter) {
            if (active_field < 3) {
                active_field = (active_field + 1) % 4;
            } else if (user_data.password_matches()) {
                user_data.finalized = true;
            }
            changed = true;
        } else if (ev.key == Key::Backspace) {
            std::string* target = get_target_field();
            if (!target->empty()) target->pop_back();
            changed = true;
        } else if (ev.key == Key::Char || ev.key == Key::Space || ev.key == Key::Back) {
            std::string* target = get_target_field();
            if (target->size() < 15 && ev.ch >= 32 && ev.ch <= 126) {
                target->push_back(ev.ch);
            }
            changed = true;
        }
    }
    return changed;
}

bool WinstallerTui::prompt_reboot_countdown(int timeout_seconds) {
    int w = 62;
    int h = 9;
    int col = std::max(2, (term_.cols() - w) / 2);
    int row = std::max(6, (term_.rows() - h) / 2);

    for (int sec = timeout_seconds; sec > 0; --sec) {
        term_.fill_rect(row, col, w, h, ' ');
        term_.draw_box(row, col, w, h, "INSTALLATION COMPLETED", true);
        term_.print_at(row + 2, col + 4, AnsiTerminal::color_bold() + AnsiTerminal::color_green() + "Windows deployment and configuration finished!" + AnsiTerminal::color_reset());
        term_.print_at(row + 3, col + 4, "Rebooting in " + AnsiTerminal::color_gold() + AnsiTerminal::color_bold() + std::to_string(sec) + " seconds" + AnsiTerminal::color_reset() + " into Windows Setup...");
        term_.print_at(row + 5, col + 4, AnsiTerminal::color_slate() + "Press ANY KEY to pause countdown and edit settings." + AnsiTerminal::color_reset());
        term_.print_at(row + 7, col + 18, AnsiTerminal::bg_teal() + " [ Reboot Now (Enter) ] " + AnsiTerminal::color_reset());

        auto ev = term_.read_key(1000);
        if (ev.key == Key::Enter) {
            return true;
        } else if (ev.key != Key::None) {
            return false;
        }
    }
    return true;
}

} // namespace litewin
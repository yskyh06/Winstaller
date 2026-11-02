#include "ui/winstaller_tui.hpp"
#include "platform/network_probe.hpp"
#include "platform/network_configurator.hpp"
#include "platform/process_runner.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace litewin {

WinstallerTui::WinstallerTui(AnsiTerminal& term) : term_(term) {}

void WinstallerTui::draw_header(const std::string& subtitle) {
    term_.clear();
    term_.draw_box(1, 1, 78, 3, "Winstaller");
    term_.print_at(2, 4, AnsiTerminal::color_bold() + "Winstaller - Automated Windows Deployment Tool" + AnsiTerminal::color_reset());
    if (!subtitle.empty()) {
        term_.print_at(2, 54, AnsiTerminal::color_gray() + subtitle + AnsiTerminal::color_reset());
    }
}

bool WinstallerTui::prompt_eula() {
    draw_header("Disclaimer & EULA");

    term_.draw_box(4, 1, 78, 18, "Disclaimer & Liability Agreement");

    term_.print_at(6, 4, AnsiTerminal::color_bold() + "[ Terms of Use & Liability Notice ]" + AnsiTerminal::color_reset());
    term_.print_at(8, 4, "1. Winstaller is an automated deployment tool provided AS-IS.");
    term_.print_at(9, 4, "2. ALL DATA, partitions, and operating systems on the selected storage");
    term_.print_at(10, 4, "   drive will be PERMANENTLY ERASED during formatting.");
    term_.print_at(11, 4, "3. The authors assume NO RESPONSIBILITY for any data loss, hardware");
    term_.print_at(12, 4, "   damage, or software licensing issues resulting from this tool.");
    term_.print_at(14, 4, "Please ensure you have backed up all important files before proceeding.");

    term_.print_at(16, 4, AnsiTerminal::color_yellow() + "Do you understand and agree to proceed?" + AnsiTerminal::color_reset());

    int selected = 0; // 0 = Agree, 1 = Exit
    while (true) {
        if (selected == 0) {
            term_.print_at(18, 12, AnsiTerminal::bg_blue() + AnsiTerminal::color_bold() + " [ Agree & Continue (Enter) ] " + AnsiTerminal::color_reset());
            term_.print_at(18, 46, " [ Exit to Shell (Esc) ] ");
        } else {
            term_.print_at(18, 12, " [ Agree & Continue (Enter) ] ");
            term_.print_at(18, 46, AnsiTerminal::bg_red() + AnsiTerminal::color_bold() + " [ Exit to Shell (Esc) ] " + AnsiTerminal::color_reset());
        }

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Left || ev.key == Key::Right || ev.key == Key::Tab) {
            selected = 1 - selected;
        } else if (ev.key == Key::Enter) {
            return (selected == 0);
        } else if (ev.key == Key::Escape) {
            return false;
        }
    }
}

bool WinstallerTui::prompt_wifi() {
    draw_header("Network Check");

    term_.print_at(5, 4, "Verifying network connectivity...");
    auto probe = probe_internet({}, true);
    if (probe.is_online) {
        term_.print_at(7, 4, AnsiTerminal::color_green() + "[OK] Internet connection verified." + AnsiTerminal::color_reset());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    }

    term_.draw_box(5, 1, 78, 17, "Network Configuration");
    term_.print_at(7, 4, AnsiTerminal::color_yellow() + "[!] Active internet connection not detected." + AnsiTerminal::color_reset());
    term_.print_at(9, 4, "You can configure Wi-Fi or skip if using local cached ESD:");
    term_.print_at(11, 4, "[1] Launch Network Manager (iwd / nmtui)");
    term_.print_at(12, 4, "[2] Continue in Offline Mode (Use Cached ESD)");

    term_.print_at(16, 14, AnsiTerminal::bg_blue() + " [ Enter: Open Network Tool ] " + AnsiTerminal::color_reset());
    term_.print_at(16, 48, " [ Esc: Skip (Offline Mode) ] ");

    while (true) {
        auto ev = term_.read_key(-1);
        if (ev.key == Key::Enter) {
            term_.show_cursor();
            term_.exit_raw_mode();
            (void)open_network_settings(NetworkConfigUiMode::tui_only);
            term_.enter_raw_mode();
            term_.hide_cursor();
            return true;
        } else if (ev.key == Key::Escape) {
            return true;
        }
    }
}

std::string WinstallerTui::prompt_disk_selection(const std::vector<DiskInfo>& disks) {
    if (disks.empty()) return "";

    draw_header("Target Disk Selection");
    term_.draw_box(4, 1, 78, 18, "Select Target Storage Disk");

    term_.print_at(6, 4, AnsiTerminal::color_bold() + "Select the disk where Windows will be installed:" + AnsiTerminal::color_reset());
    term_.print_at(7, 4, AnsiTerminal::color_red() + "WARNING: All partitions and files on this disk will be WIPED!" + AnsiTerminal::color_reset());

    int selected_idx = 0;
    while (true) {
        for (std::size_t i = 0; i < disks.size() && i < 6; ++i) {
            int row = 9 + static_cast<int>(i) * 2;
            const auto& d = disks[i];
            std::string gb = std::to_string(d.size_bytes / (1024ULL * 1024 * 1024)) + " GB";
            std::string model = d.model.value_or(d.name);
            std::string tran = d.transport.value_or(d.path.find("nvme") != std::string::npos ? "nvme" : "disk");
            std::string label = d.path + " [" + model + ", " + gb + "] (" + tran + ")";

            if (static_cast<int>(i) == selected_idx) {
                term_.print_at(row, 4, AnsiTerminal::color_cyan() + AnsiTerminal::color_bold() + " > (*) " + label + AnsiTerminal::color_reset());
            } else {
                term_.print_at(row, 4, "   ( ) " + label + "    ");
            }
        }

        term_.print_at(19, 14, AnsiTerminal::bg_blue() + " [ Enter: Select Disk ] " + AnsiTerminal::color_reset());
        term_.print_at(19, 48, " [ Esc: Cancel / Exit ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Up) {
            if (selected_idx > 0) --selected_idx;
        } else if (ev.key == Key::Down) {
            if (selected_idx + 1 < static_cast<int>(disks.size())) ++selected_idx;
        } else if (ev.key == Key::Escape) {
            return "";
        } else if (ev.key == Key::Enter) {
            break;
        }
    }

    // Safety Modal: Confirmation
    std::string chosen_disk = disks[selected_idx].path;
    term_.fill_rect(8, 8, 62, 10, ' ');
    term_.draw_box(8, 8, 62, 10, "WARNING: FINAL ERASE CONFIRMATION");
    term_.print_at(10, 11, AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "Are you sure you want to format this drive?" + AnsiTerminal::color_reset());
    term_.print_at(11, 11, "Target Drive: " + AnsiTerminal::color_bold() + chosen_disk + AnsiTerminal::color_reset());
    term_.print_at(12, 11, "All existing data will be permanently destroyed!");

    int confirm_sel = 1; // Default to Cancel for safety
    while (true) {
        if (confirm_sel == 0) {
            term_.print_at(15, 12, AnsiTerminal::bg_red() + AnsiTerminal::color_bold() + " [ YES, FORMAT & INSTALL ] " + AnsiTerminal::color_reset());
            term_.print_at(15, 42, " [ CANCEL ] ");
        } else {
            term_.print_at(15, 12, " [ YES, FORMAT & INSTALL ] ");
            term_.print_at(15, 42, AnsiTerminal::bg_blue() + AnsiTerminal::color_bold() + " [ CANCEL ] " + AnsiTerminal::color_reset());
        }

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Left || ev.key == Key::Right || ev.key == Key::Tab) {
            confirm_sel = 1 - confirm_sel;
        } else if (ev.key == Key::Enter) {
            if (confirm_sel == 0) return chosen_disk;
            return "";
        } else if (ev.key == Key::Escape) {
            return "";
        }
    }
}

TuiEditionChoice WinstallerTui::prompt_edition_selection(const std::string& detected_oem_edition, bool cached_esd_found) {
    draw_header("Windows Edition Selection");
    term_.draw_box(4, 1, 78, 18, "Windows Edition & Installation Options");

    term_.print_at(6, 4, "System Architecture: " + AnsiTerminal::color_bold() + "x86_64 (64-bit UEFI)" + AnsiTerminal::color_reset());
    std::string oem_str = detected_oem_edition.empty() ? "None (Default to Pro)" : (detected_oem_edition + " (Detected)");
    term_.print_at(7, 4, "ACPI OEM License: " + AnsiTerminal::color_green() + oem_str + AnsiTerminal::color_reset());

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
        term_.print_at(9, 4, AnsiTerminal::color_bold() + "[1] Windows Edition:" + (cur_row == 0 ? " <--" : "   ") + AnsiTerminal::color_reset());
        if (choice.edition_index == 6) {
            term_.print_at(10, 6, AnsiTerminal::color_cyan() + "(*) Windows 11 Pro (Index 6) [Recommended]" + AnsiTerminal::color_reset());
            term_.print_at(11, 6, "( ) Windows 11 Home (Index 4)             ");
        } else {
            term_.print_at(10, 6, "( ) Windows 11 Pro (Index 6)             ");
            term_.print_at(11, 6, AnsiTerminal::color_cyan() + "(*) Windows 11 Home (Index 4) [Selected]   " + AnsiTerminal::color_reset());
        }

        term_.print_at(13, 4, AnsiTerminal::color_bold() + "[2] ESD Source:" + (cur_row == 1 ? " <--" : "   ") + AnsiTerminal::color_reset());
        if (cached_esd_found) {
            std::string esd_check = choice.reuse_existing_esd ? "[X] Reuse cached ESD on P5 (Fastest, skip download)" : "[ ] Fresh download from Microsoft CDN      ";
            term_.print_at(14, 6, AnsiTerminal::color_green() + esd_check + AnsiTerminal::color_reset());
        } else {
            term_.print_at(14, 6, AnsiTerminal::color_gray() + "[!] No local cache found (Will download from official CDN)" + AnsiTerminal::color_reset());
        }

        std::string lang_label = "[ " + languages[lang_idx].second + " ] (Space to cycle)";
        term_.print_at(16, 4, AnsiTerminal::color_bold() + "[3] Target Language: " + (cur_row == 2 ? AnsiTerminal::color_cyan() : "") + lang_label + "     " + AnsiTerminal::color_reset());

        term_.print_at(19, 16, AnsiTerminal::bg_blue() + " [ Enter: Start Deployment ] " + AnsiTerminal::color_reset());
        term_.print_at(19, 52, " [ Space: Toggle Option ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Up) {
            cur_row = (cur_row + 2) % 3;
        } else if (ev.key == Key::Down || ev.key == Key::Tab) {
            cur_row = (cur_row + 1) % 3;
        } else if (ev.key == Key::Space) {
            if (cur_row == 0) {
                choice.edition_index = (choice.edition_index == 6) ? 4 : 6;
                choice.edition_name = (choice.edition_index == 6) ? "Windows 11 Pro" : "Windows 11 Home";
            } else if (cur_row == 1 && cached_esd_found) {
                choice.reuse_existing_esd = !choice.reuse_existing_esd;
            } else if (cur_row == 2) {
                lang_idx = (lang_idx + 1) % static_cast<int>(languages.size());
                choice.language = languages[lang_idx].first;
                if (lang_idx != 0 && cached_esd_found) {
                    choice.reuse_existing_esd = false; // Non-Korean language requires fresh ESD
                }
            }
        } else if (ev.key == Key::Enter) {
            return choice;
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
    term_.draw_box(1, 1, 78, 3, "Winstaller - Deployment Active");
    term_.print_at(2, 4, AnsiTerminal::color_bold() + "Windows 11 Zero-Touch Installation in Progress" + AnsiTerminal::color_reset());

    // Pipeline Box
    term_.draw_box(4, 1, 78, 9, "Pipeline Status (" + step_name + ")");
    std::string safe_msg = detail_msg.substr(0, 70);
    term_.print_at(5, 4, safe_msg);

    // Current Step Progress Bar
    int bar_width = 50;
    int filled_step = (step_pct * bar_width) / 100;
    std::string step_bar = "[" + std::string(filled_step, '=') + (filled_step < bar_width ? ">" : "") +
                           std::string(bar_width > filled_step ? bar_width - filled_step - 1 : 0, ' ') + "] " +
                           std::to_string(step_pct) + "%";
    term_.print_at(7, 4, "Current Step : " + step_bar);

    // Overall Progress Bar
    int filled_overall = (overall_pct * bar_width) / 100;
    std::string overall_bar = "[" + std::string(filled_overall, '=') + (filled_overall < bar_width ? ">" : "") +
                             std::string(bar_width > filled_overall ? bar_width - filled_overall - 1 : 0, ' ') + "] " +
                             std::to_string(overall_pct) + "%";
    term_.print_at(9, 4, "Total Progress: " + AnsiTerminal::color_cyan() + overall_bar + AnsiTerminal::color_reset());

    // Interactive Customization Box during deploy
    term_.draw_box(13, 1, 78, 10, "User Environment Customization (Optional, editable anytime)");
    term_.print_at(14, 4, AnsiTerminal::color_gray() + "* Defaults ('User', no password, random PC) apply if left unchanged." + AnsiTerminal::color_reset());

    auto field_style = [&](int f) {
        return (active_field == f) ? (AnsiTerminal::bg_blue() + AnsiTerminal::color_bold()) : "";
    };

    term_.print_at(16, 4, "1. Username      : " + field_style(0) + "[" + user_data.username + std::string(15 - user_data.username.size(), ' ') + "]" + AnsiTerminal::color_reset());
    std::string masked_pw(user_data.password.size(), '*');
    term_.print_at(17, 4, "2. Password      : " + field_style(1) + "[" + masked_pw + std::string(15 - masked_pw.size(), ' ') + "]" + AnsiTerminal::color_reset());
    term_.print_at(18, 4, "3. Computer Name : " + field_style(2) + "[" + user_data.computer_name + std::string(15 - user_data.computer_name.size(), ' ') + "]" + AnsiTerminal::color_reset());

    term_.print_at(21, 6, AnsiTerminal::color_gray() + "[Tab/Arrows: Switch Field]   [Type/Backspace: Edit]   [Enter: Save]" + AnsiTerminal::color_reset());
}

bool WinstallerTui::handle_user_data_input(TuiUserData& user_data, int& active_field) {
    auto ev = term_.read_key(0); // Non-blocking
    if (ev.key == Key::None) return false;

    if (ev.key == Key::Tab || ev.key == Key::Down) {
        active_field = (active_field + 1) % 3;
        return true;
    } else if (ev.key == Key::Up) {
        active_field = (active_field + 2) % 3;
        return true;
    } else if (ev.key == Key::Enter) {
        user_data.finalized = true;
        return true;
    } else if (ev.key == Key::Backspace) {
        std::string* target = (active_field == 0) ? &user_data.username :
                              (active_field == 1) ? &user_data.password : &user_data.computer_name;
        if (!target->empty()) target->pop_back();
        return true;
    } else if (ev.key == Key::Char || ev.key == Key::Space) {
        std::string* target = (active_field == 0) ? &user_data.username :
                              (active_field == 1) ? &user_data.password : &user_data.computer_name;
        if (target->size() < 15) {
            target->push_back(ev.ch);
        }
        return true;
    }
    return false;
}

bool WinstallerTui::prompt_reboot_countdown(int timeout_seconds) {
    for (int sec = timeout_seconds; sec > 0; --sec) {
        term_.fill_rect(9, 10, 58, 9, ' ');
        term_.draw_box(9, 10, 58, 9, "INSTALLATION PREPARATION COMPLETE");
        term_.print_at(11, 14, AnsiTerminal::color_bold() + "Windows deployment and configuration finished!" + AnsiTerminal::color_reset());
        term_.print_at(12, 14, "Rebooting in " + AnsiTerminal::color_yellow() + AnsiTerminal::color_bold() + std::to_string(sec) + " seconds" + AnsiTerminal::color_reset() + " into Windows Setup...");
        term_.print_at(14, 14, "Press ANY KEY to pause countdown and edit settings.");
        term_.print_at(16, 20, AnsiTerminal::bg_blue() + " [ Reboot Now (Enter) ] " + AnsiTerminal::color_reset());

        auto ev = term_.read_key(1000);
        if (ev.key == Key::Enter) {
            return true; // Immediate reboot
        } else if (ev.key != Key::None) {
            return false; // User aborted countdown to edit
        }
    }
    return true; // Timeout expired, auto-reboot
}

} // namespace litewin

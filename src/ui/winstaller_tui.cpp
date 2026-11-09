#include "ui/winstaller_tui.hpp"
#include "core/linux_catalog.hpp"
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
    term_.print_at(2, col + 3, AnsiTerminal::color_bold() + AnsiTerminal::color_teal() + "* Winstaller" + AnsiTerminal::color_reset() + " - Automated OS Deployment Tool");
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
            term_.print_at(row + 4, col + 4, "Winstaller is connected and ready to download Windows images.        ");

            int countdown = 5;
            bool cancel_auto = false;
            while (countdown > 0 && !cancel_auto) {
                term_.print_at(row + 6, col + 4, "Advancing to disk selection in " + AnsiTerminal::color_teal() + AnsiTerminal::color_bold() +
                    std::to_string(countdown) + "s" + AnsiTerminal::color_reset() + "... (or press Enter)          ");

                term_.print_at(row + 14, col + 4, AnsiTerminal::bg_teal() + " [ Continue (" + std::to_string(countdown) + "s / Enter) ] " + AnsiTerminal::color_reset());
                term_.print_at(row + 14, col + 34, " [ Wi-Fi Tool (W) ] ");
                term_.print_at(row + 14, col + 54, " [ Exit (Esc) ] ");

                auto ev = term_.read_key(1000);
                if (ev.key == Key::Enter) return ScreenAction::Next;
                if (ev.key == Key::Back) return ScreenAction::Back;
                if (ev.key == Key::Escape) return ScreenAction::Exit;
                if (ev.key == Key::Char && (ev.ch == 'w' || ev.ch == 'W')) {
                    term_.show_cursor();
                    term_.exit_raw_mode();
                    (void)open_network_settings(NetworkConfigUiMode::tui_only);
                    term_.enter_raw_mode();
                    term_.hide_cursor();
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
            auto ev = term_.read_key(2500); // Auto-detect hotplugged USB tethering or Ethernet cable
            if (ev.key == Key::None) {
                re_probe = true;
                break;
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
    term_.update_size();
    int m_w = 72;
    int m_h = 16;
    int m_col = std::max(2, (term_.cols() - m_w) / 2);
    int m_row = std::max(4, (term_.rows() - m_h) / 2);

    term_.fill_rect(m_row, m_col, m_w, m_h, ' ');
    term_.draw_box(m_row, m_col, m_w, m_h, "Winstaller - Terms of Use & Liability Waiver", true);
    term_.print_at(m_row + 2, m_col + 4, AnsiTerminal::color_gold() + AnsiTerminal::color_bold() + "[ Terms of Use & Liability Disclaimer ]" + AnsiTerminal::color_reset());
    term_.print_at(m_row + 4, m_col + 4, "1. Winstaller is an automated deployment utility provided 'AS-IS'.");
    term_.print_at(m_row + 5, m_col + 4, "2. ALL DATA on the selected target drive will be PERMANENTLY ERASED.");
    term_.print_at(m_row + 6, m_col + 4, "3. The authors assume NO LIABILITY for data loss, hardware damage, or");
    term_.print_at(m_row + 7, m_col + 4, "   licensing issues resulting from the use of this software.");
    term_.print_at(m_row + 8, m_col + 4, "4. Users are solely responsible for selecting drives and keeping backups.");
    term_.print_at(m_row + 10, m_col + 4, AnsiTerminal::color_slate() + "By continuing installation, you agree to all these terms." + AnsiTerminal::color_reset());

    term_.print_at(m_row + 13, m_col + 20, AnsiTerminal::bg_teal() + " [ Close / Return (Enter / Esc) ] " + AnsiTerminal::color_reset());

    while (true) {
        auto ev = term_.read_key(-1);
        if (ev.key == Key::Enter || ev.key == Key::Escape || (ev.key == Key::Char && (ev.ch == 'e' || ev.ch == 'E'))) {
            break;
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

    int selected_idx = 0;
    while (true) {
        term_.update_size();
        draw_header("Target Disk Selection");
        term_.draw_box(row, col, w, h, "Target Storage Disk Selection", true);
        term_.print_at(row + 2, col + 4, AnsiTerminal::color_bold() + "Select target drive for OS installation:" + AnsiTerminal::color_reset());
        term_.print_at(row + 3, col + 4, AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "[!] WARNING: All existing data on this drive will be WIPED!" + AnsiTerminal::color_reset());

        for (std::size_t i = 0; i < disks.size() && i < 4; ++i) {
            int d_row = row + 5 + static_cast<int>(i) * 2;
            const auto& d = disks[i];
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
                label += " [Partitions: " + std::to_string(d.children.size()) + "]";
            }
            if (label.size() > 60) label = label.substr(0, 57) + "...";

            if (static_cast<int>(i) == selected_idx) {
                term_.print_at(d_row, col + 4, AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + " > (*) " + label + AnsiTerminal::color_reset());
            } else {
                term_.print_at(d_row, col + 4, AnsiTerminal::color_slate() + "   ( ) " + label + AnsiTerminal::color_reset());
            }
        }

        term_.print_at(row + 13, col + 4, AnsiTerminal::color_slate() + "* Continuing (Enter) constitutes agreement to Terms & Liability Waiver." + AnsiTerminal::color_reset());

        term_.print_at(row + 15, col + 4, AnsiTerminal::bg_teal() + " [ Agree & Install (Enter) ] " + AnsiTerminal::color_reset());
        term_.print_at(row + 15, col + 35, " [ EULA (E) ] ");
        term_.print_at(row + 15, col + 50, " [ Back (B) ] ");
        term_.print_at(row + 15, col + 64, " [ Esc ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Up) {
            if (selected_idx > 0) --selected_idx;
        } else if (ev.key == Key::Down) {
            if (selected_idx + 1 < static_cast<int>(disks.size())) ++selected_idx;
        } else if (ev.key == Key::Char && (ev.ch == 'e' || ev.ch == 'E')) {
            show_eula_modal();
            return prompt_disk_selection(disks);
        } else if (ev.key == Key::Back) {
            return {ScreenAction::Back, ""};
        } else if (ev.key == Key::Escape) {
            return {ScreenAction::Exit, ""};
        } else if (ev.key == Key::Enter) {
            if (!disks[selected_idx].children.empty()) {
                int m_w = 68;
                int m_h = 10;
                int m_col = std::max(2, (term_.cols() - m_w) / 2);
                int m_row = std::max(6, (term_.rows() - m_h) / 2);

                term_.fill_rect(m_row, m_col, m_w, m_h, ' ');
                term_.draw_box(m_row, m_col, m_w, m_h, " DISK ERROR: EXISTING PARTITIONS ", true);
                term_.print_at(m_row + 2, m_col + 4, AnsiTerminal::color_red() + AnsiTerminal::color_bold() +
                    "[!] Error: Disk has " + std::to_string(disks[selected_idx].children.size()) +
                    " existing partition(s)!" + AnsiTerminal::color_reset());
                term_.print_at(m_row + 4, m_col + 4, "Winstaller requires a clean disk. Existing data will be wiped.");
                term_.print_at(m_row + 5, m_col + 4, "Do you want to FORCE WIPE all partitions and proceed?");
                term_.print_at(m_row + 7, m_col + 4, AnsiTerminal::bg_red() + " [ Force Wipe (Enter) ] " + AnsiTerminal::color_reset());
                term_.print_at(m_row + 7, m_col + 36, " [ Cancel (Esc / B) ] ");

                bool proceed = false;
                while (true) {
                    auto m_ev = term_.read_key(-1);
                    if (m_ev.key == Key::Enter) {
                        proceed = true;
                        break;
                    } else if (m_ev.key == Key::Escape || m_ev.key == Key::Back) {
                        proceed = false;
                        break;
                    }
                }
                if (!proceed) {
                    continue;
                }
            }
            break;
        }
    }

    std::string chosen_disk = disks[selected_idx].path;
    return {ScreenAction::Next, chosen_disk};
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
    term_.update_size();
    int m_w = 76;
    int m_h = 18;
    int m_col = std::max(2, (term_.cols() - m_w) / 2);
    int m_row = std::max(5, (term_.rows() - m_h) / 2);

    const auto tabs = get_os_tabs();
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

    auto items = get_tab_os_items(tabs[active_tab_idx]);
    int cur_item_idx = 0;
    if (tabs[active_tab_idx] == "Windows") {
        cur_item_idx = (choice.edition_index == 6) ? 1 : 0;
    } else {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (items[i].first == choice.linux_distro_id || items[i].second == choice.edition_name) {
                cur_item_idx = i;
                break;
            }
        }
    }

    while (true) {
        term_.fill_rect(m_row, m_col, m_w, m_h, ' ');
        term_.draw_box(m_row, m_col, m_w, m_h, "Select Target Operating System", true);

        // Render Top Horizontal Tab Bar:
        // [ Windows ] | [ Ubuntu ] | [ Debian ] | [ RHEL / Fedora ] | [ Arch ]
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

        // Divider
        std::string div_line;
        for (int c = 0; c < m_w - 6; ++c) div_line += "─";
        term_.print_at(m_row + 3, m_col + 3, AnsiTerminal::color_slate() + div_line + AnsiTerminal::color_reset());

        // Single list of OS versions for the active tab
        int max_display = 10;
        int top_idx = 0;
        if (cur_item_idx >= max_display) {
            top_idx = cur_item_idx - max_display + 1;
        }

        for (int i = 0; i < max_display && (top_idx + i) < static_cast<int>(items.size()); ++i) {
            int idx = top_idx + i;
            int line_row = m_row + 4 + i;
            const auto& item = items[idx];
            if (idx == cur_item_idx) {
                term_.print_at(line_row, m_col + 4, AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + " > (*) " + item.second + AnsiTerminal::color_reset());
            } else {
                term_.print_at(line_row, m_col + 4, AnsiTerminal::color_slate() + "   ( ) " + item.second + AnsiTerminal::color_reset());
            }
        }

        term_.print_at(m_row + 15, m_col + 4, AnsiTerminal::color_slate() + "Left/Right: Switch Tab  │  Up/Down: Select  │  Enter: Confirm" + AnsiTerminal::color_reset());
        term_.print_at(m_row + 16, m_col + 14, AnsiTerminal::bg_teal() + " [ Select (Enter) ] " + AnsiTerminal::color_reset());
        term_.print_at(m_row + 16, m_col + 42, " [ Cancel (Esc) ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Left) {
            active_tab_idx = (active_tab_idx + static_cast<int>(tabs.size()) - 1) % static_cast<int>(tabs.size());
            items = get_tab_os_items(tabs[active_tab_idx]);
            cur_item_idx = 0;
        } else if (ev.key == Key::Right) {
            active_tab_idx = (active_tab_idx + 1) % static_cast<int>(tabs.size());
            items = get_tab_os_items(tabs[active_tab_idx]);
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
    term_.update_size();
    int m_w = 60;
    int m_h = 13;
    int m_col = std::max(2, (term_.cols() - m_w) / 2);
    int m_row = std::max(5, (term_.rows() - m_h) / 2);

    int cur_sel = lang_idx;
    int n = static_cast<int>(languages.size());

    while (true) {
        term_.fill_rect(m_row, m_col, m_w, m_h, ' ');
        term_.draw_box(m_row, m_col, m_w, m_h, "Select Target Language", true);

        for (int i = 0; i < n && i < 7; ++i) {
            int line_row = m_row + 2 + i;
            if (i == cur_sel) {
                term_.print_at(line_row, m_col + 4, AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + " > (*) " + languages[i].second + AnsiTerminal::color_reset());
            } else {
                term_.print_at(line_row, m_col + 4, AnsiTerminal::color_slate() + "   ( ) " + languages[i].second + AnsiTerminal::color_reset());
            }
        }

        term_.print_at(m_row + 10, m_col + 10, AnsiTerminal::bg_teal() + " [ Select (Enter) ] " + AnsiTerminal::color_reset());
        term_.print_at(m_row + 10, m_col + 36, " [ Cancel (Esc) ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Up) {
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
    term_.update_size();

    int w = 76;
    int h = 18;
    int col = std::max(2, (term_.cols() - w) / 2);
    int row = std::max(5, (term_.rows() - h) / 2);

    TuiEditionChoice choice;
    // Default to Home (Index 4) for consumer OEM standard, upgrade to Pro only if detected
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
    int cur_row = 0; // 0 = Edition, 1 = Language, 2 = Install Source, 3 = Start Button

    while (true) {
        draw_header("Installation Options");
        term_.draw_box(row, col, w, h, "Installation Options", true);

        term_.print_at(row + 2, col + 4, "System Architecture: " + AnsiTerminal::color_bold() + "x86_64" + AnsiTerminal::color_reset());
        std::string oem_str = detected_oem_edition.empty() ? "None (Default: Home)" : (detected_oem_edition + " [Detected]");
        term_.print_at(row + 3, col + 4, "ACPI OEM License   : " + AnsiTerminal::color_green() + oem_str + AnsiTerminal::color_reset());

        auto row_active = [&](int r) {
            return cur_row == r ? (AnsiTerminal::color_teal() + AnsiTerminal::color_bold() + "> " + AnsiTerminal::color_reset()) : "  ";
        };

        // Row 0: Target OS / Edition
        std::string ed_field = "[ " + choice.edition_name + " ]";
        std::string os_label = "[1] Target OS       : ";
        term_.print_at(row + 5, col + 4, row_active(0) + AnsiTerminal::color_bold() + os_label + AnsiTerminal::color_reset() + AnsiTerminal::color_cyan() + ed_field + AnsiTerminal::color_reset());

        // Row 1: Target Language
        std::string lang_field = "[ " + languages[lang_idx].second + " ]";
        term_.print_at(row + 7, col + 4, row_active(1) + AnsiTerminal::color_bold() + "[2] Target Language : " + AnsiTerminal::color_reset() + AnsiTerminal::color_cyan() + lang_field + AnsiTerminal::color_reset());

        // Row 2: Install Source (renamed from ESD Source)
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

        // Action Buttons at bottom
        if (cur_row == 3) {
            term_.print_at(row + 14, col + 4, AnsiTerminal::bg_teal() + " [ Next: Select Disk (Enter) ] " + AnsiTerminal::color_reset());
        } else {
            term_.print_at(row + 14, col + 4, AnsiTerminal::color_bold() + " [ Next: Select Disk (Enter) ] " + AnsiTerminal::color_reset());
        }
        term_.print_at(row + 14, col + 38, " [ Back (B) ] ");
        term_.print_at(row + 14, col + 54, " [ Exit (Esc) ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Up) {
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
            } else if (cur_row == 3) {
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
    term_.update_size();
    int w = 76;
    int h = 18;
    int col = std::max(2, (term_.cols() - w) / 2);
    int row = std::max(5, (term_.rows() - h) / 2);

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
        draw_header(is_linux ? "Linux User Account Setup (Required)" : "User Account Setup");
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

        if (cur_field == 4) {
            term_.print_at(row + 15, col + 4, AnsiTerminal::bg_teal() + " [ Start Installation (Enter) ] " + AnsiTerminal::color_reset());
        } else {
            term_.print_at(row + 15, col + 4, AnsiTerminal::color_bold() + " [ Start Installation (Enter) ] " + AnsiTerminal::color_reset());
        }
        if (cur_field == 5) {
            term_.print_at(row + 15, col + 40, AnsiTerminal::bg_teal() + " [ Back ] " + AnsiTerminal::color_reset());
        } else {
            term_.print_at(row + 15, col + 40, " [ Back ] ");
        }
        term_.print_at(row + 15, col + 54, " [ Exit (Esc) ] ");

        auto ev = term_.read_key(-1);
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
        } else if (ev.key == Key::Backspace || ev.key == Key::Back) {
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
        } else if ((ev.key == Key::Char || ev.key == Key::Space) && cur_field <= 3) {
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
    term_.print_at(2, col + 3, AnsiTerminal::color_bold() + AnsiTerminal::color_teal() + "* OS Zero-Touch Installation in Progress" + AnsiTerminal::color_reset());

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
            if (ev.ch >= 32 && ev.ch <= 126) {
                const std::size_t max_len = (active_field == 0) ? 20 : ((active_field == 3) ? 15 : 64);
                bool valid_char = true;
                if (active_field == 0) {
                    const std::string_view forbidden = "/\\[]:;|=,+*?<>\"";
                    if (forbidden.find(ev.ch) != std::string_view::npos || (target->empty() && (ev.ch == ' ' || ev.ch == '.'))) {
                        valid_char = false;
                    }
                } else if (active_field == 3) {
                    bool is_alnum = (ev.ch >= 'A' && ev.ch <= 'Z') || (ev.ch >= 'a' && ev.ch <= 'z') || (ev.ch >= '0' && ev.ch <= '9');
                    if (!is_alnum && (ev.ch != '-' || target->empty())) {
                        valid_char = false;
                    }
                }
                if (valid_char && target->size() < max_len) {
                    target->push_back(ev.ch);
                    changed = true;
                }
            }
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
        term_.print_at(row + 2, col + 4, AnsiTerminal::color_bold() + AnsiTerminal::color_green() + "OS deployment and configuration finished!" + AnsiTerminal::color_reset());
        term_.print_at(row + 3, col + 4, "Rebooting in " + AnsiTerminal::color_gold() + AnsiTerminal::color_bold() + std::to_string(sec) + " seconds" + AnsiTerminal::color_reset() + " into installed OS...");
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
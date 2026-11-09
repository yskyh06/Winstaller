#pragma once

#include "ui/ansi_terminal.hpp"
#include "core/disk_inventory.hpp"

#include <string>
#include <vector>

namespace litewin {

enum class ScreenAction {
    Next,
    Back,
    Exit
};

struct TuiUserData {
    std::string username = "User";
    std::string password = "";
    std::string password_confirm = "";
    std::string computer_name = "";
    bool finalized = false;

    [[nodiscard]] bool password_matches() const noexcept {
        return password == password_confirm;
    }
};

struct TuiEditionChoice {
    int edition_index = 4; // Default Index 4: Home (OEM Consumer standard)
    std::string edition_name = "Windows 11 Home";
    std::string language = "ko-KR";
    bool reuse_existing_esd = false;
    bool is_linux = false;
    std::string os_tab = "Windows";
    std::string linux_distro_id = "";
};

class WinstallerTui {
public:
    explicit WinstallerTui(AnsiTerminal& term);

    // Multi-OS Catalog Accessors (v1.1)
    static std::vector<std::string> get_os_tabs();
    static std::vector<std::pair<std::string, std::string>> get_tab_os_items(const std::string& tab_name);

    // Modal: Full EULA & Terms of Use Popup
    void show_eula_modal();

    // Modal: Windows Edition Dropdown Selection Popup
    void show_edition_modal(TuiEditionChoice& choice);

    // Modal: Language Dropdown Selection Popup
    void show_language_modal(TuiEditionChoice& choice, int& lang_idx, const std::vector<std::pair<std::string, std::string>>& languages);

    // Screen 1: EULA & Disclaimer (Standalone legacy / direct prompt)
    ScreenAction prompt_eula();

    // Screen 2: Wi-Fi Setup
    ScreenAction prompt_wifi();

    // Screen 3: Target Disk Selection
    std::pair<ScreenAction, std::string> prompt_disk_selection(const std::vector<DiskInfo>& disks);

    // Screen 4: Windows Version & Edition Selection
    std::pair<ScreenAction, TuiEditionChoice> prompt_edition_selection(const std::string& detected_oem_edition, bool cached_esd_found);

    // Screen 4.5: Mandatory User Account & Password Setup (for Linux)
    ScreenAction prompt_user_account(TuiUserData& user_data, bool is_linux = true);

    // Screen 5: Live Deployment Dual-Pane View
    void render_progress(
        const std::string& step_name,
        int step_pct,
        int overall_pct,
        const std::string& detail_msg,
        TuiUserData& user_data,
        int active_field = 0
    );

    // Screen 5: Non-blocking key handler for user environment form
    bool handle_user_data_input(TuiUserData& user_data, int& active_field);

    // Screen 6: 10-Second Auto-Reboot Countdown (Returns true if time expired -> reboot now)
    bool prompt_reboot_countdown(int timeout_seconds = 10);

    // Utility: Draw centered header banner
    void draw_header(const std::string& subtitle = "");

private:
    AnsiTerminal& term_;
};

} // namespace litewin

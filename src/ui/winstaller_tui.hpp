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
    int edition_index = 6; // Default Index 6: Pro
    std::string edition_name = "Windows 11 Pro";
    std::string language = "ko-KR";
    bool reuse_existing_esd = false;
};

class WinstallerTui {
public:
    explicit WinstallerTui(AnsiTerminal& term);

    // Screen 1: EULA & Disclaimer
    ScreenAction prompt_eula();

    // Screen 2: Wi-Fi Setup
    ScreenAction prompt_wifi();

    // Screen 3: Target Disk Selection
    std::pair<ScreenAction, std::string> prompt_disk_selection(const std::vector<DiskInfo>& disks);

    // Screen 4: Windows Version & Edition Selection
    std::pair<ScreenAction, TuiEditionChoice> prompt_edition_selection(const std::string& detected_oem_edition, bool cached_esd_found);

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

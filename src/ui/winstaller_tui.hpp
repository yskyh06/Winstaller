#pragma once

#include "ui/ansi_terminal.hpp"
#include "ui/tui_layout.hpp"
#include "ui/mouse_controls.hpp"
#include "core/disk_inventory.hpp"
#include "core/download_status.hpp"
#include "core/windows_settings.hpp"
#include "platform/update_service.hpp"

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
    std::uint64_t catalog_revision=0;
    std::optional<RemoteTarget> remote_target;
    bool restart_requested = false;
    int edition_index = 4; // Default Index 4: Home (OEM Consumer standard)
    std::string edition_name = "Windows 11 Home";
    std::string language = "ko-KR";
    bool reuse_existing_esd = false;
    bool is_linux = false;
    std::string os_tab = "Windows";
    std::string linux_distro_id = "";
};

struct DetailsContent {
    std::string key;
    std::string title = "Details";
    std::string body = "Details coming soon.";
};

class WinstallerTui {
public:
    explicit WinstallerTui(AnsiTerminal& term);

    void configure_updates(UpdateService* service) { updates_ = service; }
    bool show_update_modal();
    static DetailsContent details_for(const std::string& key);
    const std::string& last_details_key() const noexcept { return last_details_key_; }

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

    void configure_progress(WindowsSettingsSession* settings = nullptr, std::string compatibility = {});
    void set_download_status(const DownloadStatus& status) { download_status_ = status; }

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

    // Responsive layout & UI helpers
    void draw_header(const std::string& subtitle = "");
    void draw_footer();
    bool show_recovery_prompt();
    bool handle_global_key(Key key);

    [[nodiscard]] MouseControls& mouse_controls() noexcept { return mouse_controls_; }
    [[nodiscard]] const MouseControls& mouse_controls() const noexcept { return mouse_controls_; }

private:
    UpdateService* updates_ = nullptr;
    bool boot_acknowledged_=false;
    std::shared_ptr<const RemoteCatalog> catalog_;
    std::vector<std::string> catalog_tabs() const;
    std::vector<std::pair<std::string,std::string>> catalog_items(const std::string& tab) const;
    void select_remote(TuiEditionChoice& choice, const RemoteTarget& target);
    KeyEvent read_control_key(int timeout_ms, int focused_id, const std::string& focused_key = {});
    bool confirm_disk_wipe(const DiskInfo& disk);
    void show_details(const DetailsContent& content);
    void draw_details_overlay(const DetailsContent& content);
    std::optional<DetailsContent> progress_details_;
    std::string last_details_key_;
    AnsiTerminal& term_;
    MouseControls mouse_controls_;
    bool progress_initialized_ = false;
    WindowsSettingsSession* windows_settings_ = nullptr;
    DownloadStatus download_status_;
    enum class SettingsTab { Account, Privacy };
    SettingsTab settings_tab_ = SettingsTab::Account;
    int settings_field_ = 0;
    std::vector<int> settings_controls() const;
    std::string compatibility_;
};

} // namespace litewin

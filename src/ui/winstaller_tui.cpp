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
    term_.print_at(2, 4, AnsiTerminal::color_bold() + "⚡ Winstaller - Automated Windows Deployment Tool" + AnsiTerminal::color_reset());
    if (!subtitle.empty()) {
        term_.print_at(2, 54, AnsiTerminal::color_gray() + subtitle + AnsiTerminal::color_reset());
    }
}

bool WinstallerTui::prompt_eula() {
    draw_header("Disclaimer & EULA");

    term_.draw_box(4, 1, 78, 18, "면책 조항 및 이용 동의 (Disclaimer)");

    term_.print_at(6, 4, AnsiTerminal::color_bold() + "[ 이용 약관 및 법적 책임 고지 ]" + AnsiTerminal::color_reset());
    term_.print_at(8, 4, "1. 본 프로그램(Winstaller)은 사용자의 편의를 위한 자동화 도구입니다.");
    term_.print_at(9, 4, "2. 디스크 파티션 분할, 포맷 및 Windows 설치 과정에서 발생하는 모든");
    term_.print_at(10, 4, "   데이터 영구 삭제, 하드웨어 손상 및 라이선스 책임은 전적으로 사용자");
    term_.print_at(11, 4, "   본인에게 있으며, 개발자는 어떠한 법적·기술적 책임도 지지 않습니다.");
    term_.print_at(13, 4, "3. 설치를 진행하기 전 중요한 모든 데이터를 사전 백업하였음을 확인합니다.");

    term_.print_at(16, 4, AnsiTerminal::color_yellow() + "위 내용을 충분히 이해하였으며, 진행에 동의하십니까?" + AnsiTerminal::color_reset());

    int selected = 0; // 0 = Agree, 1 = Exit
    while (true) {
        if (selected == 0) {
            term_.print_at(18, 12, AnsiTerminal::bg_blue() + AnsiTerminal::color_bold() + " [  동의함 (Agree & Enter)  ] " + AnsiTerminal::color_reset());
            term_.print_at(18, 44, " [  종료 (Exit & Esc)  ] ");
        } else {
            term_.print_at(18, 12, " [  동의함 (Agree & Enter)  ] ");
            term_.print_at(18, 44, AnsiTerminal::bg_red() + AnsiTerminal::color_bold() + " [  종료 (Exit & Esc)  ] " + AnsiTerminal::color_reset());
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

    // Check if network is already reachable
    term_.print_at(5, 4, "네트워크 연결 상태를 확인하고 있습니다...");
    auto probe = probe_internet({}, true);
    if (probe.is_online) {
        term_.print_at(7, 4, AnsiTerminal::color_green() + "✔ 네트워크가 정상 연결되어 인터넷 통신이 가능합니다." + AnsiTerminal::color_reset());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    }

    term_.draw_box(5, 1, 78, 17, "Wi-Fi 네트워크 설정");
    term_.print_at(7, 4, AnsiTerminal::color_yellow() + "[!] 유선 네트워크가 감지되지 않았거나 인터넷에 연결되지 않았습니다." + AnsiTerminal::color_reset());
    term_.print_at(9, 4, "무선 네트워크(Wi-Fi) 도구를 실행하여 연결을 설정할 수 있습니다.");
    term_.print_at(11, 4, "[1] nmtui / iwctl 네트워크 관리자 실행");
    term_.print_at(12, 4, "[2] 로컬 캐시(ESD)를 사용하여 오프라인으로 진행");

    term_.print_at(16, 14, AnsiTerminal::bg_blue() + " [ Enter: 네트워크 도구 열기 ] " + AnsiTerminal::color_reset());
    term_.print_at(16, 48, " [ Esc: 건너뛰기 ] ");

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
    term_.draw_box(4, 1, 78, 18, "설치 대상 디스크 선택");

    term_.print_at(6, 4, AnsiTerminal::color_bold() + "Windows를 설치할 대상 디스크를 선택하세요:" + AnsiTerminal::color_reset());
    term_.print_at(7, 4, AnsiTerminal::color_red() + "⚠️  선택한 디스크의 모든 기존 데이터와 파티션은 영구 삭제됩니다." + AnsiTerminal::color_reset());

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
                term_.print_at(row, 4, AnsiTerminal::color_cyan() + AnsiTerminal::color_bold() + " ▶ (*) " + label + AnsiTerminal::color_reset());
            } else {
                term_.print_at(row, 4, "   ( ) " + label + "    ");
            }
        }

        term_.print_at(19, 14, AnsiTerminal::bg_blue() + " [ Enter: 디스크 선택 완료 ] " + AnsiTerminal::color_reset());
        term_.print_at(19, 48, " [ Esc: 취소/종료 ] ");

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
    term_.draw_box(8, 8, 62, 10, "⚠️  DANGER: FINAL CONFIRMATION");
    term_.print_at(10, 11, AnsiTerminal::color_red() + AnsiTerminal::color_bold() + "정말 다음 드라이브를 포맷하고 설치하시겠습니까?" + AnsiTerminal::color_reset());
    term_.print_at(11, 11, "대상 드라이브: " + AnsiTerminal::color_bold() + chosen_disk + AnsiTerminal::color_reset());
    term_.print_at(12, 11, "모든 기존 파일, OS, 복구 파티션이 영구 소멸됩니다.");

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
    term_.draw_box(4, 1, 78, 18, "Windows 에디션 및 설치 옵션");

    term_.print_at(6, 4, "시스템 아키텍처: " + AnsiTerminal::color_bold() + "x86_64 (64-bit UEFI)" + AnsiTerminal::color_reset());
    std::string oem_str = detected_oem_edition.empty() ? "미감지 (기본 Windows 11 Pro 선택)" : (detected_oem_edition + " (자동 감지)");
    term_.print_at(7, 4, "메인보드 OEM 정품 라이선스: " + AnsiTerminal::color_green() + oem_str + AnsiTerminal::color_reset());

    TuiEditionChoice choice;
    choice.edition_index = 6;
    choice.edition_name = "Windows 11 Pro";
    choice.language = "ko-KR";
    choice.reuse_existing_esd = cached_esd_found;

    int cur_row = 0;
    while (true) {
        term_.print_at(9, 4, AnsiTerminal::color_bold() + "[1] 에디션 선택:" + AnsiTerminal::color_reset());
        if (choice.edition_index == 6) {
            term_.print_at(10, 6, AnsiTerminal::color_cyan() + "(*) Windows 11 Pro (인덱스 6) [권장]" + AnsiTerminal::color_reset());
            term_.print_at(11, 6, "( ) Windows 11 Home (인덱스 4)       ");
        } else {
            term_.print_at(10, 6, "( ) Windows 11 Pro (인덱스 6)       ");
            term_.print_at(11, 6, AnsiTerminal::color_cyan() + "(*) Windows 11 Home (인덱스 4) [선택]" + AnsiTerminal::color_reset());
        }

        term_.print_at(13, 4, AnsiTerminal::color_bold() + "[2] 로컬 ESD 캐시 활용:" + AnsiTerminal::color_reset());
        if (cached_esd_found) {
            std::string esd_check = choice.reuse_existing_esd ? "[X] 기존 P5 캐시 재사용 (다운로드 생략, 4.3 GB)" : "[ ] 새로 다운로드 (클린 다운로드)";
            term_.print_at(14, 6, AnsiTerminal::color_green() + esd_check + AnsiTerminal::color_reset());
        } else {
            term_.print_at(14, 6, AnsiTerminal::color_gray() + "[!] 로컬 캐시 없음 (MS 공식 CDN에서 자동 다운로드)" + AnsiTerminal::color_reset());
        }

        term_.print_at(16, 4, AnsiTerminal::color_bold() + "[3] 표시 언어: 한국어 (ko-KR)" + AnsiTerminal::color_reset());

        term_.print_at(19, 16, AnsiTerminal::bg_blue() + " [ Enter: 설치 파이프라인 시작 ] " + AnsiTerminal::color_reset());
        term_.print_at(19, 52, " [ Space: 옵션 변경 ] ");

        auto ev = term_.read_key(-1);
        if (ev.key == Key::Up || ev.key == Key::Down) {
            cur_row = 1 - cur_row;
        } else if (ev.key == Key::Space) {
            if (cur_row == 0) {
                choice.edition_index = (choice.edition_index == 6) ? 4 : 6;
                choice.edition_name = (choice.edition_index == 6) ? "Windows 11 Pro" : "Windows 11 Home";
            } else if (cached_esd_found) {
                choice.reuse_existing_esd = !choice.reuse_existing_esd;
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
    term_.draw_box(1, 1, 78, 3, "Winstaller - Pipeline Active");
    term_.print_at(2, 4, AnsiTerminal::color_bold() + "⚡ Windows 11 자동 설치 진행 중" + AnsiTerminal::color_reset());

    // Pipeline Box
    term_.draw_box(4, 1, 78, 9, "진행 상황 (" + step_name + ")");
    std::string safe_msg = detail_msg.substr(0, 70);
    term_.print_at(5, 4, safe_msg);

    // Current Step Progress Bar
    int bar_width = 50;
    int filled_step = (step_pct * bar_width) / 100;
    std::string step_bar = "[" + std::string(filled_step, '=') + (filled_step < bar_width ? ">" : "") +
                           std::string(bar_width > filled_step ? bar_width - filled_step - 1 : 0, ' ') + "] " +
                           std::to_string(step_pct) + "%";
    term_.print_at(7, 4, "현재 작업: " + step_bar);

    // Overall Progress Bar
    int filled_overall = (overall_pct * bar_width) / 100;
    std::string overall_bar = "[" + std::string(filled_overall, '=') + (filled_overall < bar_width ? ">" : "") +
                             std::string(bar_width > filled_overall ? bar_width - filled_overall - 1 : 0, ' ') + "] " +
                             std::to_string(overall_pct) + "%";
    term_.print_at(9, 4, "전체 진행: " + AnsiTerminal::color_cyan() + overall_bar + AnsiTerminal::color_reset());

    // Interactive Customization Box during deploy
    term_.draw_box(13, 1, 78, 10, "실시간 환경 설정 (작업 진행 중 자유롭게 수정 가능)");
    term_.print_at(14, 4, AnsiTerminal::color_gray() + "※ 미입력 시 기본값('User', 무암호, 랜덤PC)으로 자동 적용됩니다." + AnsiTerminal::color_reset());

    auto field_style = [&](int f) {
        return (active_field == f) ? (AnsiTerminal::bg_blue() + AnsiTerminal::color_bold()) : "";
    };

    term_.print_at(16, 4, "1. 사용자 이름 (Username)   : " + field_style(0) + "[" + user_data.username + std::string(15 - user_data.username.size(), ' ') + "]" + AnsiTerminal::color_reset());
    std::string masked_pw(user_data.password.size(), '*');
    term_.print_at(17, 4, "2. 로그인 암호 (Password)   : " + field_style(1) + "[" + masked_pw + std::string(15 - masked_pw.size(), ' ') + "]" + AnsiTerminal::color_reset());
    term_.print_at(18, 4, "3. 컴퓨터 이름 (PC Name)    : " + field_style(2) + "[" + user_data.computer_name + std::string(15 - user_data.computer_name.size(), ' ') + "]" + AnsiTerminal::color_reset());

    term_.print_at(21, 6, AnsiTerminal::color_gray() + "[Tab / 방향키: 항목 이동]   [글자 입력 / 백스페이스: 수정]   [Enter: 저장]" + AnsiTerminal::color_reset());
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
        term_.draw_box(9, 10, 58, 9, "🎉 설치 준비 완료");
        term_.print_at(11, 14, AnsiTerminal::color_bold() + "모든 이미지 배포 및 무인 구성이 완료되었습니다!" + AnsiTerminal::color_reset());
        term_.print_at(12, 14, "시스템이 " + AnsiTerminal::color_yellow() + AnsiTerminal::color_bold() + std::to_string(sec) + "초" + AnsiTerminal::color_reset() + " 후 자동으로 재부팅됩니다.");
        term_.print_at(14, 14, "설정을 다시 수정하시려면 【아무 키나 누르세요】");
        term_.print_at(16, 20, AnsiTerminal::bg_blue() + " [ 지금 바로 재부팅 (Enter) ] " + AnsiTerminal::color_reset());

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

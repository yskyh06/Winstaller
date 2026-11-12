#include "ui/winstaller_tui.hpp"
#include "platform/configuration_writer.hpp"
#include "platform/http_downloader.hpp"
#include "core/hash_verifier.hpp"
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
using namespace litewin;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
class InputTerminal : public AnsiTerminal {
public:
    std::deque<KeyEvent> keys;
    KeyEvent read_key(int = -1) override {
        if(keys.empty()) return {Key::None};
        auto e=keys.front();keys.pop_front();return e;
    }
};
void press(InputTerminal& t, WinstallerTui& ui, int id, MouseKind kind) {
    const auto* area=ui.mouse_controls().find_area(id); CHECK(area);
    KeyEvent e{Key::Mouse};e.mouse={kind,area->bounds.row,area->bounds.col,0};t.keys.push_back(e);
}
void test_ui() {
    InputTerminal term;WinstallerTui ui(term);TuiUserData user;user.username="Frozen";int field=0;
    ui.configure_progress();ui.render_progress("AcquireImage",45,43,"downloading",user);
    for (const auto& [seconds, expected] : {
             std::pair{59.0, "0h 00m 59s"}, std::pair{60.0, "0h 01m 00s"},
             std::pair{3600.0, "1h 00m 00s"}}) {
        DownloadStatus status;
        status.eta_seconds = seconds;
        status.host = "releases.ubuntu.com";
        status.origin = DownloadOrigin::Official;
        ui.set_download_status(status);
        std::ostringstream frame;
        auto* previous = std::cout.rdbuf(frame.rdbuf());
        ui.render_progress("AcquireImage", 45, 43, "downloading", user);
        std::cout.rdbuf(previous);
        CHECK(frame.str().find(expected) != std::string::npos);
        CHECK(frame.str().find("Server: releases.ubuntu.com (Official host)") != std::string::npos);
        CHECK(frame.str().find("█") != std::string::npos);
        CHECK(frame.str().find("░") != std::string::npos);
    }
    CHECK(!ui.mouse_controls().find_area(500));CHECK(!ui.mouse_controls().find_area(6000));
    term.keys={{Key::Char,'x'},{Key::Tab},{Key::Enter},{Key::Backspace}};
    ui.handle_user_data_input(user,field);CHECK(user.username=="Frozen");CHECK(!user.finalized);
    WindowsSettingsSession state;ui.configure_progress(&state);
    auto draw=[&]{ui.render_progress("AcquireImage",45,43,"downloading",user);};
    draw();press(term,ui,6000,MouseKind::Move);ui.handle_user_data_input(user,field);CHECK(!state.is_open());
    draw();press(term,ui,6000,MouseKind::LeftPress);press(term,ui,6000,MouseKind::LeftRelease);
    ui.handle_user_data_input(user,field);CHECK(state.is_open());
    draw();term.keys={{Key::Char,'x'}};ui.handle_user_data_input(user,field);
    CHECK(state.draft().options.username=="Userx");CHECK(state.effective().username=="User");
    term.keys = {{Key::Right}};
    ui.handle_user_data_input(user, field);
    draw();
    CHECK(ui.mouse_controls().find_area(6104));
    CHECK(!ui.mouse_controls().find_area(6100));
    term.keys = {{Key::Space, ' '}, {Key::Left}};
    ui.handle_user_data_input(user, field);
    draw();
    CHECK(ui.mouse_controls().find_area(6100));
    CHECK(state.draft().options.username == "Userx");
    CHECK(!state.draft().options.disable_telemetry);
    // Updating progress never closes or commits the draft.
    draw();CHECK(state.is_open());CHECK(state.effective().username=="User");
    press(term,ui,6107,MouseKind::LeftPress);press(term,ui,6107,MouseKind::LeftRelease);
    ui.handle_user_data_input(user,field);CHECK(!state.is_open());CHECK(state.effective().username=="Userx");
    draw();term.keys={{Key::Char,'s'}};ui.handle_user_data_input(user,field);draw();
    state.draft().options.password="secret";state.draft().password_confirm="different";
    press(term,ui,6107,MouseKind::LeftPress);press(term,ui,6107,MouseKind::LeftRelease);
    ui.handle_user_data_input(user,field);CHECK(state.is_open());CHECK(state.effective().password.empty());
    const auto now=WindowsSettingsSession::Clock::now();state.begin_completion(now,10);
    CHECK(state.poll_completion(now+std::chrono::seconds(10))==SettingsDecision::Discarded);
    CHECK(state.effective().username=="Userx");CHECK(state.effective().password.empty());
    // A fresh session can switch modes with a mismatched draft password.
    WindowsSettingsSession oobe;
    ui.configure_progress(&oobe);
    draw();
    term.keys = {{Key::Char, 's'}};
    ui.handle_user_data_input(user, field);
    draw();
    oobe.draft().options.password = "unconfirmed";
    press(term, ui, 6110, MouseKind::LeftPress);
    press(term, ui, 6110, MouseKind::LeftRelease);
    ui.handle_user_data_input(user, field);
    draw();
    CHECK(!ui.mouse_controls().find_area(6101));
    CHECK(ui.mouse_controls().find_area(6103));
    press(term, ui, 6107, MouseKind::LeftPress);
    press(term, ui, 6107, MouseKind::LeftRelease);
    ui.handle_user_data_input(user, field);
    CHECK(!oobe.is_open());
    CHECK(!oobe.effective().create_local_account);
    CHECK(oobe.effective().password.empty());
}
void test_xml_and_commit(const std::filesystem::path& root) {
    UnattendOptions o; o.bypass_tpm=true;o.username="Alice";o.password="<secret>&";
    auto xml=UnattendGenerator::generate_xml(o);
    CHECK(xml.find("BypassTPMCheck")!=std::string::npos);
    for(const char* absent:{"BypassSecureBootCheck","BypassRAMCheck","BypassCPUCheck","BypassStorageCheck","DiagTrack","UnsupportedHardwareNotificationCache"}) CHECK(xml.find(absent)==std::string::npos);
    CHECK(xml.find("&lt;secret&gt;&amp;")!=std::string::npos);
    CHECK(xml.find("LocalAccount")!=std::string::npos);
    auto oobe = o;
    oobe.create_local_account = false;
    const auto oobe_xml = UnattendGenerator::generate_xml(oobe);
    for (const char* absent : {"<LocalAccounts>", "<AutoLogon>", "BypassNRO",
                              "HideOnlineAccountScreens", "HideWirelessSetupInOOBE", "secret"}) {
        CHECK(oobe_xml.find(absent) == std::string::npos);
    }
    CHECK(write_final_unattend(root,o));
    std::ifstream in(root/"Windows/Panther/unattend.xml");std::string saved((std::istreambuf_iterator<char>(in)),{});
    // Random default computer names differ between generations; check material settings.
    CHECK(saved.find("<Name>Alice</Name>")!=std::string::npos);
    CHECK(saved.find("&lt;secret&gt;&amp;")!=std::string::npos);
    CHECK(!write_final_unattend(root/"missing",o));
    auto unsafe=root/"unsafe";std::filesystem::create_directory(unsafe);
    std::filesystem::create_directory_symlink(root/"Windows",unsafe/"Windows");
    CHECK(!write_final_unattend(unsafe,o));
    o.disable_telemetry=false;o.disable_advertising_id=false;o.disable_tailored_experiences=false;
    xml=UnattendGenerator::generate_xml(o);
    CHECK(xml.find("AllowTelemetry")==std::string::npos);CHECK(xml.find("DisabledByGroupPolicy")==std::string::npos);
    CHECK(xml.find("TailoredExperiencesWithDiagnosticDataEnabled")==std::string::npos);
}
void test_download(const std::filesystem::path& root) {
    const auto source=root/"source.bin";std::ofstream(source)<<std::string(20000,'a');
    DownloadOptions opts;opts.url="file://"+source.string();opts.destination_path=(root/"target.bin").string();opts.minimum_expected_bytes=1;
    opts.expected_sha256=compute_file_sha256(source.string());
    std::vector<DownloadStatus> seen;opts.status_callback=[&](const DownloadStatus& s){seen.push_back(s);};
    auto result=HttpDownloader::download(opts);CHECK(result.success);CHECK(!seen.empty());
    CHECK(result.status.phase==DownloadPhase::Complete);CHECK(result.status.hash==DownloadHash::Matched);
    CHECK(result.status.transport==DownloadTransport::Local);CHECK(result.status.origin==DownloadOrigin::Unknown);
    CHECK(result.status.downloaded_bytes==20000);
    opts.expected_sha256=std::string(64,'0');result=HttpDownloader::download(opts);
    CHECK(!result.success);CHECK(result.status.hash==DownloadHash::Mismatch);CHECK(result.status.phase==DownloadPhase::Failed);
    opts.expected_sha256="";result=HttpDownloader::download(opts);CHECK(result.success);CHECK(result.status.hash==DownloadHash::NotProvided);
    // Resume bytes must not inflate rate or the received count.
    std::ofstream(opts.destination_path+".part")<<std::string(1000,'a');
    result=HttpDownloader::download(opts);CHECK(result.success);CHECK(result.status.downloaded_bytes==20000);CHECK(result.status.resumed_bytes==1000);
}
int main(int argc, char** argv){
    if(argc>=4 && std::string(argv[1])=="--probe") {
        DownloadOptions o;o.url=argv[2];o.destination_path=argv[3];o.minimum_expected_bytes=1;
        if(argc>=5) o.expected_sha256=argv[4];
        int count=0; bool verified=false;
        o.status_callback=[&](const DownloadStatus& s){++count;verified=verified||s.transport==DownloadTransport::HttpsVerified;};
        auto r=HttpDownloader::download(o);
        std::cout<<"{\"success\":"<<(r.success?"true":"false")<<",\"phase\":"<<static_cast<int>(r.status.phase)
          <<",\"hash\":"<<static_cast<int>(r.status.hash)<<",\"transport\":"<<static_cast<int>(r.status.transport)
          <<",\"host\":\""<<r.status.host<<"\",\"bytes\":"<<r.status.downloaded_bytes
          <<",\"resume\":"<<r.status.resumed_bytes<<",\"verified_seen\":"<<(verified?"true":"false")<<",\"count\":"<<count<<"}\n";
        return 0;
    }

    auto root=std::filesystem::temp_directory_path()/("litewin-settings-"+std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {test_ui();test_xml_and_commit(root);test_download(root);std::filesystem::remove_all(root);return 0;}
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<"\n";std::filesystem::remove_all(root);return 1;}
}

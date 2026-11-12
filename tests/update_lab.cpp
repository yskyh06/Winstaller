// Local VM harness. Never installed in the Live ISO and never uses production keys.
#include "platform/update_service.hpp"
#include "platform/update_media.hpp"
#include "ui/winstaller_tui.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace litewin;
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--guard") {
            const auto state=boot_target_status(argv[2]);
            std::cout<<"BOOT_TARGET_STATUS="<<static_cast<int>(state)<<'\n';
            return state==BootTargetStatus::Protected?0:1;
        }
        if(argc!=2)throw std::runtime_error("Usage: update_lab fixture-directory");
        const std::filesystem::path root=argv[1];auto config=UpdateConfig::load((root/"config.json").string());
        UpdateService service(config,[&](const std::string& url,std::stop_token){std::ifstream f(root/(url==config.catalog_url?"catalog.signed.json":"release.signed.json"));return std::string((std::istreambuf_iterator<char>(f)),{});},
          [&](const UpdateComponent& component,int fd,std::stop_token stop,const UpdateProgress& progress){
              std::ifstream f(root/("payload-"+component.role),std::ios::binary);if(!f)throw std::runtime_error("Missing lab payload");
              auto offset=lseek(fd,0,SEEK_END);f.seekg(offset);char bytes[65536];std::uint64_t done=offset;
              while(f){if(stop.stop_requested())throw std::runtime_error("Update cancelled");f.read(bytes,sizeof(bytes));size_t size=f.gcount(),n=0;while(n<size){auto w=write(fd,bytes+n,size-n);if(w<=0)throw std::runtime_error("Lab payload write failed");n+=w;}done+=size;if(progress)progress(done,component.size);}
          });
        service.check();
        while(service.snapshot().checking)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(!service.snapshot().release||!service.snapshot().catalog)throw std::runtime_error("Lab signatures rejected");
        AnsiTerminal term;WinstallerTui ui(term);ui.configure_updates(&service);
        auto result=ui.prompt_edition_selection("",false);
        std::ofstream("/run/winstaller-lab-result")<<static_cast<int>(service.snapshot().state)<<'\n'<<service.snapshot().error<<'\n';
        return result.second.restart_requested?2:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

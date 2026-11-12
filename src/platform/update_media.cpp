#include "platform/update_media.hpp"
#include "platform/process_runner.hpp"
#include <nlohmann/json.hpp>
#include <linux/magic.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <stdexcept>
namespace litewin {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
std::string read(const fs::path& path) {
    check(!fs::is_symlink(path),"Unsafe USB identity record");
    std::ifstream in(path);check(bool(in),"This boot medium does not support in-place updates");
    std::string s(65537,'\0');in.read(s.data(),s.size());s.resize(in.gcount());check(s.size()<=65536,"Invalid USB identity record");
    while(!s.empty()&&(s.back()=='\n'||s.back()=='\r'))s.pop_back();return s;
}
std::string disk_sequence(const std::string& device) {
    auto sys=fs::canonical(fs::path("/sys/class/block")/fs::path(device).filename());
    check(fs::exists(sys/"partition"),"Update source is not a partition");return read(sys.parent_path()/"diskseq");
}
Json inventory() {
    auto r=ProcessRunner::run("lsblk",{"--json","--list","--paths","-o","PATH,TYPE,PARTUUID,UUID,FSTYPE,MOUNTPOINTS"});
    check(r.success(),"Could not identify update USB");return Json::parse(r.stdout_output).at("blockdevices");
}
}
BootTargetStatus boot_target_status(const std::string& target) {
    try {
        const auto cmd=" "+read("/proc/cmdline")+" ";
        if(cmd.find(" winstaller.slot=A ")==std::string::npos && cmd.find(" winstaller.slot=B ")==std::string::npos)
            return BootTargetStatus::Allowed;
        const auto id=read("/run/winstaller-media/partuuid");
        check(id.size()==36 && id.find_first_not_of("0123456789abcdef-")==std::string::npos,"Invalid boot partition identity");
        const auto partition=fs::canonical(fs::path("/dev/disk/by-partuuid")/id);
        check(disk_sequence(partition.string())==read("/run/winstaller-media/diskseq"),"Boot USB identity changed");
        const auto sys=fs::canonical(fs::path("/sys/class/block")/partition.filename());
        const auto boot_disk=fs::path("/dev")/sys.parent_path().filename();
        struct stat source{},destination{};
        check(stat(boot_disk.c_str(),&source)==0 && S_ISBLK(source.st_mode) &&
              stat(target.c_str(),&destination)==0 && S_ISBLK(destination.st_mode),"Could not identify boot and target disks");
        return source.st_rdev==destination.st_rdev ? BootTargetStatus::Protected : BootTargetStatus::Allowed;
    }catch(const std::exception&){return BootTargetStatus::Unknown;}
}
UpdateMedia::UpdateMedia(bool writable) {
    try {
        auto cmd=read("/proc/cmdline");
        const bool a=(" "+cmd+" ").find(" winstaller.slot=A ")!=std::string::npos;
        const bool b=(" "+cmd+" ").find(" winstaller.slot=B ")!=std::string::npos;
        check(a!=b,"This boot medium requires a newly created update-capable USB");slot_=a?"A":"B";
        struct statfs ram{};check(statfs("/run/archiso/copytoram",&ram)==0&&ram.f_type==TMPFS_MAGIC,"Updates require booting entirely from RAM");
        check(fs::is_regular_file("/run/archiso/copytoram/airootfs.sfs"),"RAM root filesystem was not found");
        partuuid_=read("/run/winstaller-media/partuuid");fsuuid_=read("/run/winstaller-media/fsuuid");
        sequence_=read("/run/winstaller-media/diskseq");instance_=read("/run/winstaller-media/instance");
        check(!partuuid_.empty()&&!fsuuid_.empty()&&!sequence_.empty()&&!instance_.empty(),"Incomplete boot USB identity");
        int matches=0;
        for(const auto& d:inventory()) if(d.value("partuuid",Json()).is_string()&&d["partuuid"]==partuuid_) {
            ++matches;device_=d.at("path").get<std::string>();
            check(d.value("type","")=="part"&&d.at("uuid")==fsuuid_&&d.value("fstype","")=="vfat","USB identity or filesystem changed");
            for(const auto& m:d.at("mountpoints"))check(m.is_null(),"USB product partition is already mounted");
        }
        check(matches==1,"Boot USB is missing or its identity is duplicated");
        struct stat st{};check(stat(device_.c_str(),&st)==0&&S_ISBLK(st.st_mode),"Update device is unavailable");device_number_=st.st_rdev;
        check(disk_sequence(device_)==sequence_,"USB was reconnected; reboot before updating");
        fs::create_directories("/run/winstaller-update");check(!fs::is_symlink("/run/winstaller-update"),"Unsafe update mount path");
        lock_=open("/run/winstaller-update/device.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        check(lock_>=0&&flock(lock_,LOCK_EX|LOCK_NB)==0,"Another update operation is active");
        check(!fs::is_symlink(root_),"Unsafe update mount path");fs::create_directories(root_);
        check(mount(device_.c_str(),root_.c_str(),"vfat",MS_NODEV|MS_NOSUID|MS_NOEXEC|(writable?0:MS_RDONLY),"utf8=1,umask=0077")==0,"Could not mount the product partition");mounted_=true;
        auto identity=Json::parse(read(root_/"winstaller/device.json"));
        check(identity.at("layout_version")==2&&identity.at("instance_uuid")==instance_&&identity.at("partuuid")==partuuid_&&identity.at("fsuuid")==fsuuid_&&identity.at("boot_abi")=="winstaller-slots-v1","Unrecognized product partition");
        revalidate();
    } catch(...) {if(mounted_)umount(root_.c_str());if(lock_>=0)close(lock_);throw;}
}
UpdateMedia::~UpdateMedia(){if(mounted_)umount(root_.c_str());if(lock_>=0)close(lock_);}
void UpdateMedia::revalidate()const {
    struct stat st{};check(stat(device_.c_str(),&st)==0&&S_ISBLK(st.st_mode)&&st.st_rdev==device_number_&&disk_sequence(device_)==sequence_,"Update USB was removed or replaced");
    int matches=0;for(const auto& d:inventory())if(d.value("partuuid",Json()).is_string()&&d["partuuid"]==partuuid_)++matches;
    check(matches==1,"USB identity became ambiguous");
}
std::uint64_t UpdateMedia::available()const{return fs::space(root_).available;}
void UpdateMedia::finish(){revalidate();check(umount(root_.c_str())==0,"Update is prepared but USB could not be safely unmounted; do not remove it");mounted_=false;}
} // namespace litewin

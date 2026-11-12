#include "platform/update_store.hpp"
#include "core/hash_verifier.hpp"
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace litewin {
namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
void ensure(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
void safe(const fs::path& p) {
    fs::path current;
    for(const auto& part:fs::absolute(p).lexically_normal()) {
        current/=part;
        ensure(!fs::is_symlink(fs::symlink_status(current)),"Symlink in update storage");
    }
}
void sync_dir(const fs::path& path) {
    int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    ensure(fd>=0,"Could not open update directory");int result=fsync(fd);close(fd);
    ensure(result==0,"Could not persist update directory");
}
std::string read(const fs::path& path,size_t limit=65536) {
    safe(path);std::ifstream in(path,std::ios::binary);ensure(bool(in),"Missing update record");
    std::string data(limit+1,'\0');in.read(data.data(),data.size());data.resize(in.gcount());
    ensure(data.size()<=limit,"Oversized update record");return data;
}
void atomic(const fs::path& path,const std::string& body) {
    safe(path);safe(path.string()+".tmp");
    int fd=open((path.string()+".tmp").c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
    ensure(fd>=0,"Could not create update record");
    size_t n=0;while(n<body.size()) {auto r=write(fd,body.data()+n,body.size()-n);if(r<0&&errno==EINTR)continue;if(r<=0){close(fd);throw std::runtime_error("Could not write update record");}n+=r;}
    int result=fsync(fd);int closed=close(fd);ensure(result==0&&closed==0,"Could not persist update record");
    fs::rename(path.string()+".tmp",path);sync_dir(path.parent_path());
}
fs::path slot_path(const fs::path& root,const std::string& slot) {
    ensure(slot=="A" || slot=="B","Invalid update slot");auto p=root/"winstaller/slots"/slot;safe(p);return p;
}
void cancelled(std::stop_token stop){ensure(!stop.stop_requested(),"Update cancelled");}
}
SlotStatus read_update_slot(const fs::path& root,const std::string& slot) {
    auto p=slot_path(root,slot);
    try {
        if(!verify_file_sha256((p/"ready.env").string(),read(p/"ready.sha256",128).substr(0,64)))return {};
        auto env=read(p/"ready.env",1024);ensure(env.size()==1024,"Invalid slot environment");
        auto manifest=Json::parse(read(p/"manifest.json"));
        const auto generation=manifest.at("generation").get<std::uint64_t>();
        ensure(generation>0&&generation<=2147483647,"Invalid slot generation");
        ensure(env.find("\nwinstaller_generation="+std::to_string(generation)+"\n")!=std::string::npos,"Slot generation mismatch");
        ensure(manifest.at("slot")==slot,"Slot identity mismatch");
        for(const auto& role:{"kernel","initramfs","rootfs"}) {
            auto file=p/update_component_path(role);safe(file);
            ensure(fs::is_regular_file(file),"Missing slot component");
            ensure(fs::file_size(file)==manifest.at("components").at(role).at("size_bytes").get<std::uint64_t>(),"Slot component size mismatch");
        }
        return {slot,manifest.at("release_id").get<std::string>(),generation};
    } catch(const std::exception&) {return {};}
}
void acknowledge_update_slot(const fs::path& root,const std::string& slot) {
    auto status=read_update_slot(root,slot);ensure(status.generation>0,"Cannot acknowledge an invalid boot slot");
    auto p=slot_path(root,slot);atomic(p/"boot-success",status.release+"\n"+std::to_string(status.generation)+"\n");
}
void stage_update(const fs::path& root,const std::string& active,const AppRelease& release,std::stop_token stop,
                  const ComponentFetch& fetch,const UpdateProgress& progress,const std::function<void()>& before_commit) {
    ensure(release.compatible&&release.components.size()==3,"Incompatible update release");
    auto current=read_update_slot(root,active);ensure(current.generation>0,"Active update slot is invalid");
    auto source=slot_path(root,active);const auto inactive=active=="A"?"B":"A";auto dest=slot_path(root,inactive);
    auto previous=read_update_slot(root,inactive);
    ensure(previous.generation<=current.generation,"Restart into the prepared release before another update");
    ensure(read(source/"boot-success",256)==current.release+"\n"+std::to_string(current.generation)+"\n","Current release has not reached the installation screen");
    ensure(current.generation<2147483647,"Slot generation exhausted");
    const auto free=fs::space(root).available;
    std::uint64_t reusable=0;
    if(fs::exists(dest))for(const auto& entry:fs::recursive_directory_iterator(dest)) {
        ensure(!entry.is_symlink(),"Symlink in inactive slot");
        ensure(entry.is_directory()||entry.is_regular_file(),"Unexpected inactive slot object");
        if(entry.is_regular_file())reusable+=entry.file_size();
    }
    ensure(free+reusable>=release.total_bytes()+64ULL*1024*1024,"Insufficient USB space for a complete release");
    cancelled(stop);
    // Invalidate the inactive set before changing any component. The active set is never modified.
    if(fs::exists(dest/"ready.sha256"))fs::remove(dest/"ready.sha256");
    if(fs::exists(dest))sync_dir(dest);
    Json descriptor={{"release_id",release.id},{"generation",current.generation+1},{"components",Json::object()}};
    for(const auto& c:release.components)descriptor["components"][c.role]={{"size_bytes",c.size},{"sha256",c.sha256}};
    bool resume=false;
    try{resume=Json::parse(read(dest/"staging.json"))==descriptor;}catch(const std::exception&){}
    // Only resume files bound to this exact signed component set and active generation.
    if(!resume && fs::exists(dest))fs::remove_all(dest);
    fs::create_directories(dest);sync_dir(dest.parent_path());
    atomic(dest/"staging.json",descriptor.dump());
    Json manifest={{"slot",inactive},{"release_id",release.id},{"generation",current.generation+1},{"components",descriptor["components"]}};
    std::uint64_t done=0;
    for(const auto& component:release.components) {
        cancelled(stop);auto relative=update_component_path(component.role);auto file=dest/relative;
        safe(file);fs::create_directories(file.parent_path());
        if(fs::is_regular_file(file)&&fs::file_size(file)==component.size&&verify_file_sha256(file.string(),component.sha256)) {
            done+=component.size;if(progress)progress(done,release.total_bytes());continue;
        }
        if(fs::exists(file))fs::remove(file);
        auto part=file.string()+".part";
        if(fs::exists(part)&&fs::file_size(part)>component.size)fs::remove(part);
        int fd=open(part.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);ensure(fd>=0,"Could not create inactive component");
        try {
            auto old=source/relative;safe(old);
            if(fs::is_regular_file(old)&&fs::file_size(old)==component.size&&verify_file_sha256(old.string(),component.sha256)) {
                ensure(ftruncate(fd,0)==0&&lseek(fd,0,SEEK_SET)==0,"Could not reset reusable component");
                std::ifstream in(old,std::ios::binary);char data[65536];
                while(in) {cancelled(stop);in.read(data,sizeof(data));size_t n=0,count=in.gcount();while(n<count){auto w=write(fd,data+n,count-n);ensure(w>0,"Could not copy reusable component");n+=w;}}
                ensure(in.eof(),"Could not read reusable component");
            } else fetch(component,fd,stop,[&](auto bytes,auto){if(progress)progress(done+std::min(bytes,component.size),release.total_bytes());});
            ensure(fsync(fd)==0,"Could not persist update component");const int closed=close(fd);fd=-1;ensure(closed==0,"Could not close update component safely");
            if(fs::file_size(part)!=component.size||!verify_file_sha256(part,component.sha256)) {
                fs::remove(part);throw std::runtime_error("Update component checksum mismatch");
            }
            fs::rename(part,file);sync_dir(file.parent_path());
        } catch(...) {if(fd>=0)close(fd);throw;}
        done+=component.size;if(progress)progress(done,release.total_bytes());
        manifest["components"][component.role]={{"size_bytes",component.size},{"sha256",component.sha256}};
    }
    atomic(dest/"manifest.json",manifest.dump());cancelled(stop);
    if(before_commit)before_commit();
    // No cancellation after this point. The checksum record is the last commit marker.
    std::string env="# GRUB Environment Block\nwinstaller_generation="+std::to_string(current.generation+1)+"\n";
    env.resize(1024,'#');atomic(dest/"ready.env",env);
    atomic(dest/"ready.sha256",compute_file_sha256((dest/"ready.env").string())+"  ready.env\n");
    ensure(read_update_slot(root,inactive).generation==current.generation+1,"Committed slot failed validation");
}
} // namespace litewin

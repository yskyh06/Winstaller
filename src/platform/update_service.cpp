#include "platform/update_service.hpp"
#include "platform/update_media.hpp"
#include <ctime>
#include <fstream>
namespace litewin {
UpdateService::UpdateService(UpdateConfig config,MetadataFetch fetch,ComponentFetch download)
    :config_(std::move(config)),fetch_(std::move(fetch)),download_(std::move(download)){}
UpdateService::~UpdateService(){check_worker_.request_stop();update_worker_.request_stop();ack_worker_.request_stop();}
UpdateSnapshot UpdateService::snapshot()const {
    std::lock_guard lock(mutex_);auto result=snapshot_;const auto now=std::time(nullptr);
    if(result.catalog&&(now<=0||result.catalog->published_at>static_cast<std::uint64_t>(now)||result.catalog->expires_at<=static_cast<std::uint64_t>(now)))result.catalog.reset();
    if(result.release&&(now<=0||result.release->published_at>static_cast<std::uint64_t>(now)||result.release->expires_at<=static_cast<std::uint64_t>(now)))result.release.reset();
    return result;
}
void UpdateService::check() {
    if(!configured())return;
    {std::lock_guard lock(mutex_);if(snapshot_.checking||snapshot_.state==UpdateState::Downloading||snapshot_.state==UpdateState::Committing)return;snapshot_.checking=true;snapshot_.check_error.clear();}
    check_worker_=std::jthread([this](std::stop_token stop){
        bool failed=false;
        try {
            auto catalog=std::make_shared<RemoteCatalog>(verify_catalog(fetch_(config_.catalog_url,stop),config_,std::time(nullptr)));
            std::lock_guard lock(mutex_);
            if(!snapshot_.catalog||catalog->revision>snapshot_.catalog->revision)snapshot_.catalog=std::move(catalog);
        }catch(const std::exception&){failed=true;}
        try {
            auto release=std::make_shared<AppRelease>(verify_release(fetch_(config_.release_url,stop),config_,std::time(nullptr)));
            std::lock_guard lock(mutex_);
            auto seen=seen_releases_.find(release->id);
            if(seen!=seen_releases_.end()&&seen->second!=release->document_hash)throw std::runtime_error("Release identity changed");
            seen_releases_[release->id]=release->document_hash;
            if(release->sequence>config_.release_sequence && (!snapshot_.release ||
               (release->revision>snapshot_.release->revision && (release->sequence>snapshot_.release->sequence || (release->id==snapshot_.release->id && release->sequence==snapshot_.release->sequence)))))snapshot_.release=std::move(release);
        }catch(const std::exception&){failed=true;}
        std::lock_guard lock(mutex_);snapshot_.checking=false;
        if(failed)snapshot_.check_error="Update check unavailable. The last verified list remains available.";
    });
}
void UpdateService::acknowledge_boot() {
    ack_worker_=std::jthread([]{
        try{UpdateMedia media(true);acknowledge_update_slot(media.root(),media.active_slot());media.finish();}catch(const std::exception&){}
    });
}
std::string UpdateService::current_version()const {
    std::ifstream file("/etc/winstaller-version");std::string version;std::getline(file,version);
    if(version.empty()||version.size()>40)return "build "+std::to_string(config_.release_sequence);
    for(unsigned char c:version)if(c<32||c>126)return "build "+std::to_string(config_.release_sequence);
    return version;
}
UpdateMediaInfo UpdateService::inspect_media()const {
    try {
        UpdateMedia media(false);auto current=read_update_slot(media.root(),media.active_slot());
        if(!current.generation)return {false,0,"The current USB slot is invalid; create a new USB."};
        auto other=read_update_slot(media.root(),media.active_slot()=="A"?"B":"A");
        if(other.generation>current.generation)return {false,0,"A release is already prepared. Restart to use it."};
        const auto available=media.available();media.finish();
        return {true,available,"Update USB: "+std::to_string(available/(1024*1024))+" MiB free"};
    }catch(const std::exception& e){return {false,0,e.what()};}
}
bool UpdateService::begin_update(const std::string& expected_release_id) {
    std::shared_ptr<const AppRelease> release;
    {std::lock_guard lock(mutex_);
        if(!snapshot_.release||!snapshot_.release->compatible||snapshot_.state==UpdateState::Downloading||snapshot_.state==UpdateState::Committing||snapshot_.state==UpdateState::Ready)return false;
        if(snapshot_.release->expires_at<=static_cast<std::uint64_t>(std::time(nullptr))){snapshot_.state=UpdateState::Failed;snapshot_.error="Release metadata expired. Check for updates again.";return false;}
        if(snapshot_.release->id!=expected_release_id){snapshot_.state=UpdateState::Failed;snapshot_.error="A newer release was found. Close and reopen Update to review it.";return false;}
        release=snapshot_.release;snapshot_.state=UpdateState::Downloading;snapshot_.bytes=0;snapshot_.total=release->total_bytes();snapshot_.error.clear();
    }
    update_worker_=std::jthread([this,release](std::stop_token stop){
        try {
            UpdateMedia media(true);media.revalidate();
            auto last_check=std::chrono::steady_clock::now();
            stage_update(media.root(),media.active_slot(),*release,stop,download_,[&](auto bytes,auto total){
                if(std::chrono::steady_clock::now()-last_check>=std::chrono::seconds(1)){media.revalidate();last_check=std::chrono::steady_clock::now();}
                std::lock_guard lock(mutex_);snapshot_.bytes=bytes;snapshot_.total=total;
            },[&]{media.revalidate();std::lock_guard lock(mutex_);if(stop.stop_requested())throw std::runtime_error("Update cancelled");snapshot_.state=UpdateState::Committing;});
            media.finish();std::lock_guard lock(mutex_);snapshot_.state=UpdateState::Ready;
        }catch(const std::exception& e){std::lock_guard lock(mutex_);snapshot_.state=stop.stop_requested()?UpdateState::Cancelled:UpdateState::Failed;snapshot_.error=e.what();}
    });return true;
}
void UpdateService::cancel(){std::lock_guard lock(mutex_);if(snapshot_.state==UpdateState::Downloading)update_worker_.request_stop();}
} // namespace litewin

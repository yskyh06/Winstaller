#pragma once
#include "platform/update_store.hpp"
#include <memory>
#include <map>
#include <mutex>
#include <thread>
namespace litewin {
using MetadataFetch=std::function<std::string(const std::string&,std::stop_token)>;
enum class UpdateState { Idle, Downloading, Committing, Ready, Cancelled, Failed };
struct UpdateSnapshot {
    std::shared_ptr<const RemoteCatalog> catalog;
    std::shared_ptr<const AppRelease> release;
    bool checking=false;
    UpdateState state=UpdateState::Idle;
    std::uint64_t bytes=0,total=0;
    std::string error, check_error;
};
struct UpdateMediaInfo {bool eligible=false;std::uint64_t available=0;std::string detail;};
class UpdateService {
public:
    explicit UpdateService(UpdateConfig config,MetadataFetch fetch=fetch_update_metadata,
                           ComponentFetch download=fetch_update_component);
    ~UpdateService();
    std::uint64_t current_sequence()const{return config_.release_sequence;}
    bool configured()const{return config_.configured();}
    void check();
    void acknowledge_boot();
    UpdateSnapshot snapshot()const;
    std::string current_version()const;
    UpdateMediaInfo inspect_media()const;
    std::string media_description()const{return inspect_media().detail;}
    bool begin_update(const std::string& expected_release_id);
    void cancel();
private:
    UpdateConfig config_;
    MetadataFetch fetch_;ComponentFetch download_;
    mutable std::mutex mutex_;
    UpdateSnapshot snapshot_;
    std::map<std::string,std::string> seen_releases_;
    std::jthread check_worker_,update_worker_,ack_worker_;
};
} // namespace litewin

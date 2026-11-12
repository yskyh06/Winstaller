#pragma once
#include "platform/update_transport.hpp"
#include <filesystem>
#include <functional>
namespace litewin {
using ComponentFetch = std::function<void(const UpdateComponent&,int,std::stop_token,const UpdateProgress&)>;
struct SlotStatus {
    std::string slot, release;
    std::uint64_t generation=0;
};
// The caller must own an exclusively locked, identity-checked product partition.
// Never pass the user data partition here. Test callers use temporary directories.
SlotStatus read_update_slot(const std::filesystem::path& root,const std::string& slot);
void acknowledge_update_slot(const std::filesystem::path& root,const std::string& slot);
void stage_update(const std::filesystem::path& root,const std::string& active,const AppRelease& release,
                  std::stop_token stop,const ComponentFetch& fetch,const UpdateProgress& progress,
                  const std::function<void()>& before_commit = {});
} // namespace litewin

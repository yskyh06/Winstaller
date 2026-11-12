#pragma once
#include <filesystem>
#include <string>
#include <cstdint>
namespace litewin {
enum class BootTargetStatus { Allowed, Protected, Unknown };
// Preserve the boot disk even after copytoram unmounts it, including SATA VM disks.
BootTargetStatus boot_target_status(const std::string& target);
class UpdateMedia {
public:
    explicit UpdateMedia(bool writable);
    ~UpdateMedia();
    UpdateMedia(const UpdateMedia&)=delete;
    UpdateMedia& operator=(const UpdateMedia&)=delete;
    const std::filesystem::path& root()const{return root_;}
    const std::string& active_slot()const{return slot_;}
    void revalidate()const;
    void finish();
    std::uint64_t available()const;
private:
    std::filesystem::path root_="/run/winstaller-update/media";
    std::string device_,slot_,sequence_,partuuid_,fsuuid_,instance_;
    std::uint64_t device_number_=0;
    int lock_=-1;
    bool mounted_=false;
};
} // namespace litewin

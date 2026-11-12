#pragma once
#include "core/esd_catalog.hpp"
#include "core/linux_catalog.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace litewin {
struct UpdateKey { std::string id, public_hex; std::vector<std::string> roles; };
struct UpdateConfig {
    std::string catalog_url, release_url;
    std::vector<UpdateKey> keys;
    std::uint64_t release_sequence = 1;
    bool configured() const;
    static UpdateConfig load(const std::string& path = "/etc/winstaller/updates.json");
};
struct RemoteTarget {
    std::string id, product, name, tab, language, language_name, strategy, image_name;
    bool windows = false, supported = true;
    std::string unsupported_reason;
    EsdTarget esd;
    LinuxTarget linux_target;
};
struct RemoteCatalog {
    std::uint64_t revision = 0, published_at=0, expires_at=0;
    std::vector<RemoteTarget> targets;
    std::vector<std::string> tabs() const;
    std::vector<std::pair<std::string,std::string>> products(const std::string& tab) const;
    std::vector<std::pair<std::string,std::string>> languages(const std::string& product) const;
    std::optional<RemoteTarget> find(const std::string& product, const std::string& language) const;
};
struct UpdateComponent { std::string role, url, sha256; std::uint64_t size = 0; };
struct AppRelease {
    std::string id, version, notes, boot_abi, document_hash;
    std::uint64_t revision = 0, sequence = 0, published_at=0, expires_at=0;
    std::vector<UpdateComponent> components;
    bool compatible = true;
    std::string incompatible_reason;
    std::uint64_t total_bytes() const;
};
// Strict parsing and signature validation throw std::runtime_error on invalid input.
RemoteCatalog verify_catalog(const std::string& envelope, const UpdateConfig& config, std::int64_t now);
AppRelease verify_release(const std::string& envelope, const UpdateConfig& config, std::int64_t now);
bool valid_update_url(const std::string& url, bool image);
std::string update_component_path(const std::string& role);
} // namespace litewin

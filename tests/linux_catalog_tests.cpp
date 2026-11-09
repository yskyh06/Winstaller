#include "core/linux_catalog.hpp"

#include <cassert>
#include <cctype>
#include <iostream>
#include <string_view>
#include <unordered_map>

namespace {

void test_all_targets_basic_properties() {
    const auto& targets = litewin::get_all_linux_targets();
    assert(!targets.empty());
    assert(targets.size() == 28);

    for (const auto& target : targets) {
        assert(!target.id.empty());
        assert(!target.display_name.empty());
        assert(!target.version.empty());
        assert(!target.download_url.empty());
        assert(target.download_url.rfind("https://", 0) == 0 ||
               target.download_url.rfind("http://", 0) == 0);
        assert(target.sha256_hash.size() == 64);
        for (char c : target.sha256_hash) {
            assert(std::isxdigit(static_cast<unsigned char>(c)));
        }
        assert(target.size_bytes > 0);
        assert(!target.image_layers.empty());
        for (const auto& layer : target.image_layers) {
            assert(!layer.empty());
        }
    }
    std::cout << "[PASS] All 28 targets basic properties validated\n";
}

void test_expired_and_badge_rules() {
    const auto& targets = litewin::get_all_linux_targets();
    std::size_t expired_count = 0;

    for (const auto& target : targets) {
        // No badges allowed in display_name
        assert(target.display_name.find("[최신]") == std::string::npos);
        assert(target.display_name.find("[레거시]") == std::string::npos);
        assert(target.display_name.find('[') == std::string::npos);
        assert(target.display_name.find(']') == std::string::npos);

        if (target.is_expired) {
            ++expired_count;
            constexpr std::string_view kSuffix = " (Expired)";
            assert(target.display_name.size() >= kSuffix.size());
            assert(target.display_name.ends_with(kSuffix));
        } else {
            assert(target.display_name.find("(Expired)") == std::string::npos);
        }
    }

    // Expected expired: Ubuntu 20.04 (2), Ubuntu 18.04 (2), Debian 11 (2), Rocky 8 (1), Alma 8 (1) -> total 8
    assert(expired_count == 8);
    std::cout << "[PASS] Expired and badge rules verified (expired_count=" << expired_count << ")\n";
}

void test_shared_url_hash_and_size_consistency() {
    const auto& targets = litewin::get_all_linux_targets();

    struct UrlMeta {
        std::string sha256_hash;
        std::uint64_t size_bytes = 0;
        std::string first_id;
    };
    std::unordered_map<std::string, UrlMeta> seen_urls;

    for (const auto& target : targets) {
        auto it = seen_urls.find(target.download_url);
        if (it != seen_urls.end()) {
            assert(it->second.sha256_hash == target.sha256_hash);
            assert(it->second.size_bytes == target.size_bytes);
        } else {
            seen_urls[target.download_url] = UrlMeta{
                .sha256_hash = target.sha256_hash,
                .size_bytes = target.size_bytes,
                .first_id = target.id,
            };
        }
    }

    // Explicitly verify Arch desktop & base match
    auto arch_desktop = litewin::find_linux_target_by_id("arch-2026.11-desktop");
    auto arch_base = litewin::find_linux_target_by_id("arch-2026.11-base");
    assert(arch_desktop.has_value() && arch_base.has_value());
    assert(arch_desktop->download_url == arch_base->download_url);
    assert(arch_desktop->sha256_hash == arch_base->sha256_hash);
    assert(arch_desktop->size_bytes == arch_base->size_bytes);

    std::cout << "[PASS] Shared download_url hash and size consistency verified\n";
}

void test_kernel_path_field() {
    const auto& targets = litewin::get_all_linux_targets();

    for (const auto& target : targets) {
        if (target.family == litewin::LinuxDistroFamily::Arch) {
            assert(target.kernel_path == "arch/boot/x86_64/vmlinuz-linux");
        } else {
            assert(target.kernel_path.empty());
        }
    }
    std::cout << "[PASS] kernel_path field verified for all targets\n";
}

void test_layered_squashfs_and_erofs() {
    const auto& targets = litewin::get_all_linux_targets();

    for (const auto& target : targets) {
        if (target.tab_category == litewin::OsTabCategory::Ubuntu) {
            if (target.version == "26.04" || target.version == "24.04") {
                assert(target.extractor_mode == litewin::LinuxExtractorMode::LayeredSquashFS);
                assert(target.image_layers.size() >= 2);
            } else if (target.version == "22.04" || target.version == "20.04" || target.version == "18.04") {
                assert(target.extractor_mode == litewin::LinuxExtractorMode::SingleSquashFS);
                assert(target.image_layers.size() == 1);
                assert(target.image_layers[0] == "casper/filesystem.squashfs");
            }
        } else if (target.tab_category == litewin::OsTabCategory::RhelFedora) {
            if (target.version == "45" || target.version == "44" || target.version == "10") {
                assert(target.extractor_mode == litewin::LinuxExtractorMode::Erofs);
                assert(target.image_layers.size() == 1);
                assert(target.image_layers[0] == "LiveOS/rootfs.erofs");
            } else if (target.version == "9" || target.version == "8") {
                assert(target.extractor_mode == litewin::LinuxExtractorMode::SingleSquashFS);
                assert(target.image_layers.size() == 1);
                assert(target.image_layers[0] == "LiveOS/squashfs.img");
            }
        } else if (target.tab_category == litewin::OsTabCategory::Debian) {
            assert(target.extractor_mode == litewin::LinuxExtractorMode::SingleSquashFS);
            assert(target.image_layers.size() == 1);
            assert(target.image_layers[0] == "live/filesystem.squashfs");
        } else if (target.tab_category == litewin::OsTabCategory::Arch) {
            assert(target.extractor_mode == litewin::LinuxExtractorMode::SingleSquashFS);
            assert(target.image_layers.size() == 1);
            assert(target.image_layers[0] == "arch/x86_64/airootfs.sfs");
        }
    }
    std::cout << "[PASS] LayeredSquashFS and Erofs rules verified\n";
}

void test_tab_lookups() {
    auto win_targets = litewin::get_linux_targets_by_tab(litewin::OsTabCategory::Windows);
    assert(win_targets.empty());

    auto ubuntu_targets = litewin::get_linux_targets_by_tab(litewin::OsTabCategory::Ubuntu);
    assert(ubuntu_targets.size() == 10);
    for (const auto& t : ubuntu_targets) {
        assert(t.tab_category == litewin::OsTabCategory::Ubuntu);
        assert(t.family == litewin::LinuxDistroFamily::Debian);
    }

    auto debian_targets = litewin::get_linux_targets_by_tab(litewin::OsTabCategory::Debian);
    assert(debian_targets.size() == 6);
    for (const auto& t : debian_targets) {
        assert(t.tab_category == litewin::OsTabCategory::Debian);
        assert(t.family == litewin::LinuxDistroFamily::Debian);
    }

    auto rhel_targets = litewin::get_linux_targets_by_tab(litewin::OsTabCategory::RhelFedora);
    assert(rhel_targets.size() == 10);
    for (const auto& t : rhel_targets) {
        assert(t.tab_category == litewin::OsTabCategory::RhelFedora);
        assert(t.family == litewin::LinuxDistroFamily::RedHat);
    }

    auto arch_targets = litewin::get_linux_targets_by_tab(litewin::OsTabCategory::Arch);
    assert(arch_targets.size() == 2);
    for (const auto& t : arch_targets) {
        assert(t.tab_category == litewin::OsTabCategory::Arch);
        assert(t.family == litewin::LinuxDistroFamily::Arch);
    }

    std::size_t total = win_targets.size() + ubuntu_targets.size() +
                        debian_targets.size() + rhel_targets.size() +
                        arch_targets.size();
    assert(total == litewin::get_all_linux_targets().size());
    assert(total == 28);
    std::cout << "[PASS] Tab lookups verified\n";
}

void test_id_lookups_and_legacy_aliases() {
    const auto& targets = litewin::get_all_linux_targets();
    for (const auto& target : targets) {
        auto found = litewin::find_linux_target_by_id(target.id);
        assert(found.has_value());
        assert(found->id == target.id);
        assert(found->display_name == target.display_name);

        // Test case-insensitivity
        std::string upper_id = target.id;
        for (char& c : upper_id) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        auto found_upper = litewin::find_linux_target_by_id(upper_id);
        assert(found_upper.has_value());
        assert(found_upper->id == target.id);
    }

    // Test legacy aliases
    auto deb13 = litewin::find_linux_target_by_id("debian-13-cli");
    assert(deb13.has_value() && deb13->id == "debian-13-standard");

    auto deb12 = litewin::find_linux_target_by_id("debian-12-cli");
    assert(deb12.has_value() && deb12->id == "debian-12-standard");

    auto deb11 = litewin::find_linux_target_by_id("debian-11-cli");
    assert(deb11.has_value() && deb11->id == "debian-11-standard");

    auto fed45 = litewin::find_linux_target_by_id("fedora-45-desktop");
    assert(fed45.has_value() && fed45->id == "fedora-45-workstation");

    auto fed44 = litewin::find_linux_target_by_id("fedora-44-desktop");
    assert(fed44.has_value() && fed44->id == "fedora-44-workstation");

    auto not_found = litewin::find_linux_target_by_id("non-existent-os-id");
    assert(!not_found.has_value());

    std::cout << "[PASS] ID lookups and legacy aliases verified\n";
}

void test_to_string_helpers() {
    assert(litewin::to_string(litewin::OsTabCategory::Windows) == "Windows");
    assert(litewin::to_string(litewin::OsTabCategory::Ubuntu) == "Ubuntu");
    assert(litewin::to_string(litewin::OsTabCategory::Debian) == "Debian");
    assert(litewin::to_string(litewin::OsTabCategory::RhelFedora) == "RHEL / Fedora");
    assert(litewin::to_string(litewin::OsTabCategory::Arch) == "Arch");
    assert(litewin::to_string(static_cast<litewin::OsTabCategory>(999)) == "Unknown");

    assert(litewin::to_string(litewin::LinuxDistroFamily::Debian) == "Debian");
    assert(litewin::to_string(litewin::LinuxDistroFamily::RedHat) == "RedHat");
    assert(litewin::to_string(litewin::LinuxDistroFamily::Arch) == "Arch");
    assert(litewin::to_string(static_cast<litewin::LinuxDistroFamily>(999)) == "Unknown");

    assert(litewin::to_string(litewin::LinuxExtractorMode::SingleSquashFS) == "SingleSquashFS");
    assert(litewin::to_string(litewin::LinuxExtractorMode::LayeredSquashFS) == "LayeredSquashFS");
    assert(litewin::to_string(litewin::LinuxExtractorMode::Erofs) == "Erofs");
    assert(litewin::to_string(static_cast<litewin::LinuxExtractorMode>(999)) == "Unknown");

    assert(litewin::to_string(litewin::LinuxEnvironment::DesktopGUI) == "Desktop GUI");
    assert(litewin::to_string(litewin::LinuxEnvironment::ServerCLI) == "Server CLI");

    std::cout << "[PASS] to_string helpers verified\n";
}

} // namespace

int main() {
    std::cout << "Running linux catalog tests...\n";
    test_all_targets_basic_properties();
    test_expired_and_badge_rules();
    test_shared_url_hash_and_size_consistency();
    test_kernel_path_field();
    test_layered_squashfs_and_erofs();
    test_tab_lookups();
    test_id_lookups_and_legacy_aliases();
    test_to_string_helpers();
    std::cout << "All linux catalog tests passed successfully!\n";
    return 0;
}

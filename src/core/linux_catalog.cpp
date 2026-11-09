#include "core/linux_catalog.hpp"

#include <algorithm>
#include <cctype>

namespace litewin {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

const std::vector<LinuxTarget> g_linux_targets = {
    // Ubuntu LTS (10 items)
    {
        .id = "ubuntu-26.04-desktop",
        .display_name = "Ubuntu 26.04 LTS (Desktop GUI)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::LayeredSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "26.04",
        .is_expired = false,
        .image_layers = {"casper/minimal.squashfs", "casper/minimal.standard.squashfs"},
        .download_url = "https://releases.ubuntu.com/26.04/ubuntu-26.04-desktop-amd64.iso",
        .sha256_hash = "487f87faaf547ea30e0aba4d5b53346292571256b25333a978db1692bcee9dd2",
        .size_bytes = 5800000000ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-26.04-server",
        .display_name = "Ubuntu 26.04 LTS (Server CLI)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::LayeredSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "26.04",
        .is_expired = false,
        .image_layers = {"casper/ubuntu-server-minimal.squashfs", "casper/ubuntu-server-minimal.ubuntu-server.squashfs"},
        .download_url = "https://releases.ubuntu.com/26.04/ubuntu-26.04-live-server-amd64.iso",
        .sha256_hash = "dec49008a71f6098d0bcfc822021f4d042d5f2db279e4d75bdd981304f1ca5d9",
        .size_bytes = 2918598656ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-24.04-desktop",
        .display_name = "Ubuntu 24.04 LTS (Desktop GUI)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::LayeredSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "24.04",
        .is_expired = false,
        .image_layers = {"casper/minimal.squashfs", "casper/minimal.standard.squashfs"},
        .download_url = "https://releases.ubuntu.com/24.04/ubuntu-24.04.5.1-desktop-amd64.iso",
        .sha256_hash = "4da4a0c9035da8e68a59a838674f403f0a54472c78a83b4fb7f78d03588f85a7",
        .size_bytes = 6094000000ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-24.04-server",
        .display_name = "Ubuntu 24.04 LTS (Server CLI)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::LayeredSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "24.04",
        .is_expired = false,
        .image_layers = {"casper/ubuntu-server-minimal.squashfs", "casper/ubuntu-server-minimal.ubuntu-server.squashfs"},
        .download_url = "https://releases.ubuntu.com/24.04/ubuntu-24.04.5-live-server-amd64.iso",
        .sha256_hash = "97f3d7ffb032c3eb3b23d2c8be9cc76e60c2c1f2c0146ba5ba9fe01cafae0fd8",
        .size_bytes = 4080486400ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-22.04-desktop",
        .display_name = "Ubuntu 22.04 LTS (Desktop GUI)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "22.04",
        .is_expired = false,
        .image_layers = {"casper/filesystem.squashfs"},
        .download_url = "https://releases.ubuntu.com/22.04/ubuntu-22.04.5-desktop-amd64.iso",
        .sha256_hash = "bfd1cee02bc4f35db939e69b934ba49a39a378797ce9aee20f6e3e3e728fefbf",
        .size_bytes = 5000000000ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-22.04-server",
        .display_name = "Ubuntu 22.04 LTS (Server CLI)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "22.04",
        .is_expired = false,
        .image_layers = {"casper/filesystem.squashfs"},
        .download_url = "https://releases.ubuntu.com/22.04/ubuntu-22.04.5-live-server-amd64.iso",
        .sha256_hash = "9bc6028870aef3f74f4e16b900008179e78b130e6b0b9a140635434a46aa98b0",
        .size_bytes = 2100000000ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-20.04-desktop",
        .display_name = "Ubuntu 20.04 LTS (Desktop GUI) (Expired)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "20.04",
        .is_expired = true,
        .image_layers = {"casper/filesystem.squashfs"},
        .download_url = "https://releases.ubuntu.com/20.04/ubuntu-20.04.6-desktop-amd64.iso",
        .sha256_hash = "510ce77afcb9537f198bc7daa0e5b503b6e67aaed68146943c231baeaab94df1",
        .size_bytes = 4400000000ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-20.04-server",
        .display_name = "Ubuntu 20.04 LTS (Server CLI) (Expired)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "20.04",
        .is_expired = true,
        .image_layers = {"casper/filesystem.squashfs"},
        .download_url = "https://releases.ubuntu.com/20.04/ubuntu-20.04.6-live-server-amd64.iso",
        .sha256_hash = "b8f31413336b9393ad5d8ef0282717b2ab19f007df2e9ed5196c13d8f9153c8b",
        .size_bytes = 1500000000ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-18.04-desktop",
        .display_name = "Ubuntu 18.04 LTS (Desktop GUI) (Expired)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "18.04",
        .is_expired = true,
        .image_layers = {"casper/filesystem.squashfs"},
        .download_url = "https://releases.ubuntu.com/18.04/ubuntu-18.04.6-desktop-amd64.iso",
        .sha256_hash = "f730be589aa1ba923ebe6eca573fa66d09ba14c4c104da2c329df652d42aff11",
        .size_bytes = 2500000000ULL,
        .kernel_path = "",
    },
    {
        .id = "ubuntu-18.04-server",
        .display_name = "Ubuntu 18.04 LTS (Server CLI) (Expired)",
        .tab_category = OsTabCategory::Ubuntu,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "18.04",
        .is_expired = true,
        .image_layers = {"casper/filesystem.squashfs"},
        .download_url = "https://releases.ubuntu.com/18.04/ubuntu-18.04.6-live-server-amd64.iso",
        .sha256_hash = "6c647b1ab4318e8c560d5748f908e108be654bad1e165f7cf4f3c1fc43995934",
        .size_bytes = 900000000ULL,
        .kernel_path = "",
    },

    // Debian (6 items)
    {
        .id = "debian-13-desktop",
        .display_name = "Debian 13 Trixie (Desktop GUI)",
        .tab_category = OsTabCategory::Debian,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "13",
        .is_expired = false,
        .image_layers = {"live/filesystem.squashfs"},
        .download_url = "https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.7.0-amd64-gnome.iso",
        .sha256_hash = "e94859a83b305cae5125dc9c6080ced040c59d099ea7381e7f300fd1dba87a17",
        .size_bytes = 3200000000ULL,
        .kernel_path = "",
    },
    {
        .id = "debian-13-standard",
        .display_name = "Debian 13 Trixie (Standard CLI)",
        .tab_category = OsTabCategory::Debian,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "13",
        .is_expired = false,
        .image_layers = {"live/filesystem.squashfs"},
        .download_url = "https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.7.0-amd64-standard.iso",
        .sha256_hash = "040a44f35186321eb6cdaab52a8a2d06224fe6b77f6fbb9f1861ad89c14e17ec",
        .size_bytes = 1200000000ULL,
        .kernel_path = "",
    },
    {
        .id = "debian-12-desktop",
        .display_name = "Debian 12 Bookworm (Desktop GUI)",
        .tab_category = OsTabCategory::Debian,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "12",
        .is_expired = false,
        .image_layers = {"live/filesystem.squashfs"},
        .download_url = "https://cdimage.debian.org/mirror/cdimage/archive/12.9.0-live/amd64/iso-hybrid/debian-live-12.9.0-amd64-gnome.iso",
        .sha256_hash = "2b992d95a86906cf4560074b38bd6894b12ac17a385a65734d67a5ab947d45b3",
        .size_bytes = 3100000000ULL,
        .kernel_path = "",
    },
    {
        .id = "debian-12-standard",
        .display_name = "Debian 12 Bookworm (Standard CLI)",
        .tab_category = OsTabCategory::Debian,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "12",
        .is_expired = false,
        .image_layers = {"live/filesystem.squashfs"},
        .download_url = "https://cdimage.debian.org/mirror/cdimage/archive/12.9.0-live/amd64/iso-hybrid/debian-live-12.9.0-amd64-standard.iso",
        .sha256_hash = "279320202a53deae1895acbb984a5a262dbcccef874372f066e2c91163161c5d",
        .size_bytes = 1100000000ULL,
        .kernel_path = "",
    },
    {
        .id = "debian-11-desktop",
        .display_name = "Debian 11 Bullseye (Desktop GUI) (Expired)",
        .tab_category = OsTabCategory::Debian,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "11",
        .is_expired = true,
        .image_layers = {"live/filesystem.squashfs"},
        .download_url = "https://cdimage.debian.org/mirror/cdimage/archive/11.11.0-live/amd64/iso-hybrid/debian-live-11.11.0-amd64-gnome.iso",
        .sha256_hash = "097ba00a2453cbb4c483c9d40af7c2e9f8fbfd02ff32d82d929f075cb1c995cc",
        .size_bytes = 2900000000ULL,
        .kernel_path = "",
    },
    {
        .id = "debian-11-standard",
        .display_name = "Debian 11 Bullseye (Standard CLI) (Expired)",
        .tab_category = OsTabCategory::Debian,
        .family = LinuxDistroFamily::Debian,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "11",
        .is_expired = true,
        .image_layers = {"live/filesystem.squashfs"},
        .download_url = "https://cdimage.debian.org/mirror/cdimage/archive/11.11.0-live/amd64/iso-hybrid/debian-live-11.11.0-amd64-standard.iso",
        .sha256_hash = "da16e530793dfad72ffc6b570e8ba82adbd2540c7aef356079d0aac14bfd5b91",
        .size_bytes = 1000000000ULL,
        .kernel_path = "",
    },

    // RHEL / Fedora (10 items)
    {
        .id = "fedora-45-workstation",
        .display_name = "Fedora 45 (Workstation GUI)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::Erofs,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "45",
        .is_expired = false,
        .image_layers = {"LiveOS/rootfs.erofs"},
        .download_url = "https://download.fedoraproject.org/pub/fedora/linux/releases/45/Workstation/x86_64/iso/Fedora-Workstation-Live-45-1.4.x86_64.iso",
        .sha256_hash = "68338c6bfe4f0d64151f2e47efe9f44a8855ad05b460e56b844fb3fa2c9a3beb",
        .size_bytes = 2925092864ULL,
        .kernel_path = "",
    },
    {
        .id = "fedora-45-server",
        .display_name = "Fedora 45 (Server CLI)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::Erofs,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "45",
        .is_expired = false,
        .image_layers = {"LiveOS/rootfs.erofs"},
        .download_url = "https://download.fedoraproject.org/pub/fedora/linux/releases/45/Server/x86_64/iso/Fedora-Server-dvd-x86_64-45-1.4.iso",
        .sha256_hash = "fcc0f623708ddbd7656df152510ba018bad618bfc99879e73809255fb11a48ff",
        .size_bytes = 4100390912ULL,
        .kernel_path = "",
    },
    {
        .id = "fedora-44-workstation",
        .display_name = "Fedora 44 (Workstation GUI)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::Erofs,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "44",
        .is_expired = false,
        .image_layers = {"LiveOS/rootfs.erofs"},
        .download_url = "https://download.fedoraproject.org/pub/fedora/linux/releases/44/Workstation/x86_64/iso/Fedora-Workstation-Live-44-1.7.x86_64.iso",
        .sha256_hash = "1620295f6a00c27c3208f0c00b8ece4eab1ec69b9002152d97488bf26a426ddf",
        .size_bytes = 2851612672ULL,
        .kernel_path = "",
    },
    {
        .id = "rocky-10-workstation",
        .display_name = "Rocky Linux 10 (Workstation GUI)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::Erofs,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "10",
        .is_expired = false,
        .image_layers = {"LiveOS/rootfs.erofs"},
        .download_url = "https://download.rockylinux.org/pub/rocky/10/live/x86_64/Rocky-10-Workstation-x86_64-latest.iso",
        .sha256_hash = "9a8241ce991398841ab15a1af2e7d66d51a7c675cab639d9b4463e94ec8aa11a",
        .size_bytes = 1815683072ULL,
        .kernel_path = "",
    },
    {
        .id = "rocky-10-server",
        .display_name = "Rocky Linux 10 (Server CLI)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::Erofs,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "10",
        .is_expired = false,
        .image_layers = {"LiveOS/rootfs.erofs"},
        .download_url = "https://download.rockylinux.org/pub/rocky/10/isos/x86_64/Rocky-10-latest-x86_64-minimal.iso",
        .sha256_hash = "aac6ac3ce781b91a91ce78463405f66c611a5dca4b3840c79e5e01d97302f6c8",
        .size_bytes = 2072444928ULL,
        .kernel_path = "",
    },
    {
        .id = "alma-10-server",
        .display_name = "AlmaLinux 10 (Server CLI)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::Erofs,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "10",
        .is_expired = false,
        .image_layers = {"LiveOS/rootfs.erofs"},
        .download_url = "https://repo.almalinux.org/almalinux/10/isos/x86_64/AlmaLinux-10-latest-x86_64-minimal.iso",
        .sha256_hash = "1b532f534231da0d1cd0ccae622bea6cd588d8a0d7b259f1f131501a6eed41a4",
        .size_bytes = 1651376128ULL,
        .kernel_path = "",
    },
    {
        .id = "rocky-9-server",
        .display_name = "Rocky Linux 9 (Server CLI)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "9",
        .is_expired = false,
        .image_layers = {"LiveOS/squashfs.img"},
        .download_url = "https://download.rockylinux.org/pub/rocky/9/isos/x86_64/Rocky-9-latest-x86_64-minimal.iso",
        .sha256_hash = "d338032cd1cdd41c67139f2f71b4c832c8e4a21943106519db9c7137df7a63d4",
        .size_bytes = 1480048640ULL,
        .kernel_path = "",
    },
    {
        .id = "alma-9-server",
        .display_name = "AlmaLinux 9 (Server CLI)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "9",
        .is_expired = false,
        .image_layers = {"LiveOS/squashfs.img"},
        .download_url = "https://repo.almalinux.org/almalinux/9/isos/x86_64/AlmaLinux-9-latest-x86_64-minimal.iso",
        .sha256_hash = "7762a4b45a66235726db145a573658964bf77bf7b9bc1c018afe86a4cf37cc2e",
        .size_bytes = 2838495232ULL,
        .kernel_path = "",
    },
    {
        .id = "rocky-8-server",
        .display_name = "Rocky Linux 8 (Server CLI) (Expired)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "8",
        .is_expired = true,
        .image_layers = {"LiveOS/squashfs.img"},
        .download_url = "https://download.rockylinux.org/pub/rocky/8/isos/x86_64/Rocky-8-latest-x86_64-minimal.iso",
        .sha256_hash = "231d21db029e900b305d8e23ff7a61d06bb4e873cb170bd4a85f4e6dd896748f",
        .size_bytes = 2000000000ULL,
        .kernel_path = "",
    },
    {
        .id = "alma-8-server",
        .display_name = "AlmaLinux 8 (Server CLI) (Expired)",
        .tab_category = OsTabCategory::RhelFedora,
        .family = LinuxDistroFamily::RedHat,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "8",
        .is_expired = true,
        .image_layers = {"LiveOS/squashfs.img"},
        .download_url = "https://repo.almalinux.org/almalinux/8/isos/x86_64/AlmaLinux-8-latest-x86_64-minimal.iso",
        .sha256_hash = "e524329700abe47ce1f509bed7e2d3c68b336a54c712daa1b492b2429a64d419",
        .size_bytes = 2140143616ULL,
        .kernel_path = "",
    },

    // Arch Linux (2 items)
    {
        .id = "arch-2026.11-desktop",
        .display_name = "Arch Linux 2026.11 (Desktop Preset)",
        .tab_category = OsTabCategory::Arch,
        .family = LinuxDistroFamily::Arch,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::DesktopGUI,
        .version = "2026.11",
        .is_expired = false,
        .image_layers = {"arch/x86_64/airootfs.sfs"},
        .download_url = "https://geo.mirror.pkgbuild.com/iso/2026.11.01/archlinux-2026.11.01-x86_64.iso",
        .sha256_hash = "01bcdcf01af2f7d58f2794899c834770d25c9bfc945bd092b32bb66672a4a216",
        .size_bytes = 1317568512ULL,
        .kernel_path = "arch/boot/x86_64/vmlinuz-linux",
    },
    {
        .id = "arch-2026.11-base",
        .display_name = "Arch Linux 2026.11 (Base CLI)",
        .tab_category = OsTabCategory::Arch,
        .family = LinuxDistroFamily::Arch,
        .extractor_mode = LinuxExtractorMode::SingleSquashFS,
        .environment = LinuxEnvironment::ServerCLI,
        .version = "2026.11",
        .is_expired = false,
        .image_layers = {"arch/x86_64/airootfs.sfs"},
        .download_url = "https://geo.mirror.pkgbuild.com/iso/2026.11.01/archlinux-2026.11.01-x86_64.iso",
        .sha256_hash = "01bcdcf01af2f7d58f2794899c834770d25c9bfc945bd092b32bb66672a4a216",
        .size_bytes = 1317568512ULL,
        .kernel_path = "arch/boot/x86_64/vmlinuz-linux",
    },
};

} // namespace

const std::vector<LinuxTarget>& get_all_linux_targets() {
    return g_linux_targets;
}

std::vector<LinuxTarget> get_linux_targets_by_tab(OsTabCategory tab) {
    std::vector<LinuxTarget> result;
    for (const auto& target : g_linux_targets) {
        if (target.tab_category == tab) {
            result.push_back(target);
        }
    }
    return result;
}

std::optional<LinuxTarget> find_linux_target_by_id(std::string_view id) {
    auto resolve_alias = [](std::string_view key) -> std::string_view {
        if (iequals(key, "debian-13-cli")) return "debian-13-standard";
        if (iequals(key, "debian-12-cli")) return "debian-12-standard";
        if (iequals(key, "debian-11-cli")) return "debian-11-standard";
        if (iequals(key, "fedora-45-desktop")) return "fedora-45-workstation";
        if (iequals(key, "fedora-44-desktop")) return "fedora-44-workstation";
        return key;
    };

    const std::string_view resolved = resolve_alias(id);
    for (const auto& target : g_linux_targets) {
        if (iequals(target.id, resolved)) {
            return target;
        }
    }
    return std::nullopt;
}

std::string_view to_string(OsTabCategory tab) {
    switch (tab) {
    case OsTabCategory::Windows:
        return "Windows";
    case OsTabCategory::Ubuntu:
        return "Ubuntu";
    case OsTabCategory::Debian:
        return "Debian";
    case OsTabCategory::RhelFedora:
        return "RHEL / Fedora";
    case OsTabCategory::Arch:
        return "Arch";
    }
    return "Unknown";
}

std::string_view to_string(LinuxDistroFamily family) {
    switch (family) {
    case LinuxDistroFamily::Debian:
        return "Debian";
    case LinuxDistroFamily::RedHat:
        return "RedHat";
    case LinuxDistroFamily::Arch:
        return "Arch";
    }
    return "Unknown";
}

std::string_view to_string(LinuxExtractorMode mode) {
    switch (mode) {
    case LinuxExtractorMode::SingleSquashFS:
        return "SingleSquashFS";
    case LinuxExtractorMode::LayeredSquashFS:
        return "LayeredSquashFS";
    case LinuxExtractorMode::Erofs:
        return "Erofs";
    }
    return "Unknown";
}

std::string_view to_string(LinuxEnvironment env) {
    switch (env) {
    case LinuxEnvironment::DesktopGUI:
        return "Desktop GUI";
    case LinuxEnvironment::ServerCLI:
        return "Server CLI";
    }
    return "Unknown";
}

} // namespace litewin

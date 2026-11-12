#include "platform/linux_installer.hpp"
#include "platform/update_media.hpp"
#include "platform/http_downloader.hpp"
#include "platform/process_runner.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace litewin {

namespace fs = std::filesystem;

std::string_view to_string(LinuxBootFamily family) noexcept {
    switch (family) {
        case LinuxBootFamily::Debian: return "Debian";
        case LinuxBootFamily::RedHat: return "RedHat";
        case LinuxBootFamily::Arch:   return "Arch";
    }
    return "Unknown";
}

std::string_view to_string(LinuxImageFormat format) noexcept {
    switch (format) {
        case LinuxImageFormat::SingleSquashFS:  return "SingleSquashFS";
        case LinuxImageFormat::LayeredSquashFS: return "LayeredSquashFS";
        case LinuxImageFormat::Erofs:           return "Erofs";
    }
    return "Unknown";
}

std::string_view to_string(LinuxPipelineStep step) noexcept {
    switch (step) {
        case LinuxPipelineStep::None:              return "None";
        case LinuxPipelineStep::PreCheck:          return "PreCheck";
        case LinuxPipelineStep::PartitionDisk:     return "PartitionDisk";
        case LinuxPipelineStep::MountVolumes:      return "MountVolumes";
        case LinuxPipelineStep::AcquireImage:      return "AcquireImage";
        case LinuxPipelineStep::ExtractRootFS:     return "ExtractRootFS";
        case LinuxPipelineStep::ConfigureChroot:   return "ConfigureChroot";
        case LinuxPipelineStep::InstallBootloader: return "InstallBootloader";
        case LinuxPipelineStep::UnmountVolumes:    return "UnmountVolumes";
        case LinuxPipelineStep::Complete:          return "Complete";
    }
    return "Unknown";
}

std::string LinuxInstaller::shell_quote(std::string_view s) {
    std::string res;
    res.reserve(s.size() + 2);
    res.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            res.append("'\\''");
        } else {
            res.push_back(c);
        }
    }
    res.push_back('\'');
    return res;
}

bool LinuxInstaller::is_valid_unix_identifier(std::string_view s) {
    if (s.empty() || s.size() > 64) {
        return false;
    }
    const char first = s.front();
    if (!std::isalnum(static_cast<unsigned char>(first)) && first != '_') {
        return false;
    }
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '_' && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

static std::string format_partition_path(std::string_view disk_path, int part_num) {
    if (disk_path.empty()) {
        return "";
    }
    const char last_char = disk_path.back();
    if (std::isdigit(static_cast<unsigned char>(last_char))) {
        return std::string(disk_path) + "p" + std::to_string(part_num);
    }
    return std::string(disk_path) + std::to_string(part_num);
}

std::optional<LinuxPartitionPlan> LinuxInstaller::build_partition_plan(
    std::string_view disk_path,
    std::uint64_t disk_size_bytes
) {
    if (disk_path.empty()) {
        return std::nullopt;
    }

    constexpr std::uint64_t kSectorSize = 512;
    constexpr std::uint64_t kEspSizeBytes = 512ULL * 1024 * 1024;          // 512 MiB
    constexpr std::uint64_t kMinRootSizeBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB
    constexpr std::uint64_t kAlignmentSectors = 2048;                      // 1 MiB alignment
    constexpr std::uint64_t kGptBackupSectors = 34;

    const std::uint64_t total_sectors = disk_size_bytes / kSectorSize;
    constexpr std::uint64_t kStartSectorP1 = kAlignmentSectors;
    constexpr std::uint64_t kSizeSectorP1 = kEspSizeBytes / kSectorSize; // 1,048,576 sectors
    constexpr std::uint64_t kStartSectorP2 = kStartSectorP1 + kSizeSectorP1; // 1,050,624

    if (total_sectors <= kStartSectorP2 + kGptBackupSectors) {
        return std::nullopt;
    }

    const std::uint64_t usable_sectors_end = total_sectors - kGptBackupSectors;
    std::uint64_t size_sector_p2 = usable_sectors_end - kStartSectorP2;
    size_sector_p2 -= (size_sector_p2 % kAlignmentSectors);

    if (size_sector_p2 < (kMinRootSizeBytes / kSectorSize)) {
        return std::nullopt;
    }

    std::string esp_name = format_partition_path(disk_path, 1);
    std::string root_name = format_partition_path(disk_path, 2);

    std::string script = "label: gpt\nunit: sectors\nsector-size: 512\n\n";
    script += "1 : start=" + std::to_string(kStartSectorP1) +
              ", size=" + std::to_string(kSizeSectorP1) +
              ", type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"EFI System Partition\"\n";
    script += "2 : start=" + std::to_string(kStartSectorP2) +
              ", size=" + std::to_string(size_sector_p2) +
              ", type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=\"Linux Root\"\n";

    return LinuxPartitionPlan{
        .esp_part = std::move(esp_name),
        .root_part = std::move(root_name),
        .sfdisk_script = std::move(script)
    };
}

std::vector<std::string> LinuxInstaller::build_extraction_commands(
    LinuxImageFormat format,
    const std::vector<std::string>& layer_paths,
    std::string_view target_mount
) {
    std::vector<std::string> commands;
    if (layer_paths.empty()) {
        return commands;
    }

    const std::string target_str(target_mount);
    const std::string quoted_target = shell_quote(target_str);

    switch (format) {
        case LinuxImageFormat::SingleSquashFS: {
            commands.push_back("unsquashfs -f -ignore-errors -no-exit-code -d " + quoted_target + " " + shell_quote(layer_paths.front()));
            std::string_view front_path = layer_paths.front();
            if (front_path.ends_with("LiveOS/squashfs.img") || front_path.ends_with("squashfs.img")) {
                std::string nested_cmd = "if [ -f " + shell_quote(target_str + "/LiveOS/rootfs.img") +
                    " ]; then mkdir -p " + shell_quote(target_str + "_rootfs") +
                    " && mount -o loop,ro " + shell_quote(target_str + "/LiveOS/rootfs.img") + " " + shell_quote(target_str + "_rootfs") +
                    " && cp -a " + shell_quote(target_str + "_rootfs/.") + " " + shell_quote(target_str + "/") +
                    " && umount -f " + shell_quote(target_str + "_rootfs") +
                    " && rm -rf " + shell_quote(target_str + "_rootfs") + " " + shell_quote(target_str + "/LiveOS") +
                    "; fi";
                commands.push_back(std::move(nested_cmd));
            }
            break;
        }

        case LinuxImageFormat::LayeredSquashFS: {
            commands.reserve(layer_paths.size() + 1);
            commands.push_back("unsquashfs -f -ignore-errors -no-exit-code -d " + quoted_target + " " + shell_quote(layer_paths.front()));
            const std::string stage_dir = target_str + "/.litewin_stage";
            const std::string quoted_stage = shell_quote(stage_dir);
            for (std::size_t i = 1; i < layer_paths.size(); ++i) {
                std::string stage_cmd =
                    "rm -rf " + quoted_stage +
                    " && mkdir -p " + quoted_stage +
                    " && unsquashfs -f -ignore-errors -no-exit-code -d " + quoted_stage + " " + shell_quote(layer_paths[i]) +
                    " && (cd " + quoted_stage + " && find . -type c -size 0 | while IFS= read -r w; do rm -rf " + quoted_target + "/\"$w\" \"./$w\"; done)" +
                    " && (cd " + quoted_stage + " && find . ! -type d | while IFS= read -r f; do if [ -d " + quoted_target + "/\"$f\" ] && [ ! -L " + quoted_target + "/\"$f\" ]; then rm -rf " + quoted_target + "/\"$f\"; fi; done)" +
                    " && (cd " + quoted_stage + " && find . -type d | while IFS= read -r d; do if [ \"$d\" != \".\" ] && { [ -e " + quoted_target + "/\"$d\" ] || [ -L " + quoted_target + "/\"$d\" ]; } && { [ ! -d " + quoted_target + "/\"$d\" ] || [ -L " + quoted_target + "/\"$d\" ]; }; then rm -rf " + quoted_target + "/\"$d\"; fi; done)" +
                    " && cp -af " + quoted_stage + "/. " + quoted_target + "/" +
                    " && rm -rf " + quoted_stage;
                commands.push_back(std::move(stage_cmd));
            }
            commands.push_back("find " + quoted_target + " -xdev -type c -size 0 -delete 2>/dev/null || true");
            break;
        }

        case LinuxImageFormat::Erofs:
            commands.push_back("fsck.erofs --extract=" + quoted_target + " " + shell_quote(layer_paths.front()));
            break;
    }

    return commands;
}

std::vector<std::string> LinuxInstaller::build_chroot_commands(
    const LinuxInstallOptions& opts
) {
    std::vector<std::string> commands;

    std::string esp_part;
    std::string root_part;
    if (!opts.target_disk_path.empty()) {
        esp_part = format_partition_path(opts.target_disk_path, 1);
        root_part = format_partition_path(opts.target_disk_path, 2);
    } else {
        esp_part = "/dev/sda1";
        root_part = "/dev/sda2";
    }

    std::string safe_user = opts.username;
    std::transform(safe_user.begin(), safe_user.end(), safe_user.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (safe_user.empty() || !is_valid_unix_identifier(safe_user)) {
        safe_user = "user";
    }

    // Common 1: generate /etc/fstab using blkid UUID fallback to partition device path
    std::string fstab_cmd1 = "ROOT_UUID=$(blkid -s UUID -o value " + shell_quote(root_part) +
        " 2>/dev/null); [ -n \"$ROOT_UUID\" ] && echo \"UUID=$ROOT_UUID / ext4 defaults 0 1\" > /etc/fstab || echo " +
        shell_quote(root_part + " / ext4 defaults 0 1") + " > /etc/fstab";
    std::string fstab_cmd2 = "ESP_UUID=$(blkid -s UUID -o value " + shell_quote(esp_part) +
        " 2>/dev/null); [ -n \"$ESP_UUID\" ] && echo \"UUID=$ESP_UUID /boot/efi vfat umask=0077 0 2\" >> /etc/fstab || echo " +
        shell_quote(esp_part + " /boot/efi vfat umask=0077 0 2") + " >> /etc/fstab";
    commands.push_back(std::move(fstab_cmd1));
    commands.push_back(std::move(fstab_cmd2));

    // Common 2: set /etc/hostname
    commands.push_back("echo " + shell_quote(opts.hostname) + " > /etc/hostname");

    // Common 3: set /etc/locale.conf (LANG=<locale>)
    commands.push_back("echo " + shell_quote("LANG=" + opts.locale) + " > /etc/locale.conf");

    // Common 4: create user safely (id -u check, fallback to --badname, ensure bash shell & home dir)
    std::string user_cmd = "(id -u " + shell_quote(safe_user) + " >/dev/null 2>&1 || "
        "useradd -m -s /bin/bash " + shell_quote(safe_user) + " 2>/dev/null || "
        "useradd --badname -m -s /bin/bash " + shell_quote(safe_user) + " 2>/dev/null || "
        "useradd --badnames -m -s /bin/bash " + shell_quote(safe_user) + " 2>/dev/null || true) && "
        "usermod -s /bin/bash " + shell_quote(safe_user) + " 2>/dev/null || true && "
        "mkdir -p /home/" + safe_user + " && chown -R " + shell_quote(safe_user + ":" + safe_user) + " /home/" + safe_user + " 2>/dev/null || true";
    commands.push_back(std::move(user_cmd));

    // Common 5: Password configuration (usermod -p with SHA-512 & chpasswd -c SHA512 to bypass PAM in chroot)
    if (!opts.password.empty()) {
        std::string pw_hash;
        auto hash_res = ProcessRunner::run("openssl", {"passwd", "-6", opts.password});
        if (hash_res.success()) {
            pw_hash = hash_res.stdout_output;
            while (!pw_hash.empty() && (pw_hash.back() == '\n' || pw_hash.back() == '\r' || pw_hash.back() == ' ')) {
                pw_hash.pop_back();
            }
        }
        if (pw_hash.empty()) {
            pw_hash = "$6$litewin$placeholder";
        }
        std::string pass_cmd =
            "usermod -p " + shell_quote(pw_hash) + " " + shell_quote(safe_user) + " 2>/dev/null || true && "
            "usermod -p " + shell_quote(pw_hash) + " root 2>/dev/null || true && "
            "usermod -U " + shell_quote(safe_user) + " 2>/dev/null || true && "
            "echo " + shell_quote(safe_user + ":" + opts.password) + " | chpasswd -c SHA512 2>/dev/null || true && "
            "echo " + shell_quote("root:" + opts.password) + " | chpasswd -c SHA512 2>/dev/null || true && "
            "chmod 0640 /etc/shadow 2>/dev/null || true";
        commands.push_back(std::move(pass_cmd));
    } else {
        commands.push_back("passwd -d " + shell_quote(safe_user) + " 2>/dev/null || usermod -p '' " + shell_quote(safe_user) + " 2>/dev/null || true");
        commands.push_back("if [ -f /etc/pam.d/common-auth ] && ! grep -q nullok /etc/pam.d/common-auth; then sed -i 's/pam_unix.so/pam_unix.so nullok/g' /etc/pam.d/common-auth || true; fi");
    }

    // Distro family adapters
    switch (opts.boot_family) {
        case LinuxBootFamily::Debian:
            commands.push_back("apt-get purge -y --ignore-missing casper 'live-boot*' 'calamares*' || true");
            commands.push_back("dpkg --purge --force-all casper lupin-casper live-boot live-boot-initramfs-tools 2>/dev/null || true");
            commands.push_back("rm -rf /usr/share/initramfs-tools/conf.d/*casper* /usr/share/initramfs-tools/conf.d/*live* /etc/initramfs-tools/conf.d/*casper* /etc/initramfs-tools/conf.d/*live* /usr/share/initramfs-tools/hooks/casper* /usr/share/initramfs-tools/scripts/casper* /usr/share/initramfs-tools/scripts/live*");
            commands.push_back("mkdir -p /etc/initramfs-tools/conf.d /etc/cloud && echo 'BOOT=local' > /etc/initramfs-tools/conf.d/litewin-local-boot.conf && touch /etc/cloud/cloud-init.disabled");
            commands.push_back("systemctl mask systemd-networkd-wait-online.service NetworkManager-wait-online.service 2>/dev/null || true");
            commands.push_back("rm -f /etc/systemd/system/network-online.target.wants/systemd-networkd-wait-online.service /etc/systemd/system/network-online.target.wants/NetworkManager-wait-online.service 2>/dev/null || true");
            commands.push_back("ln -sf /dev/null /etc/systemd/system/systemd-networkd-wait-online.service 2>/dev/null || true");
            commands.push_back("usermod -aG sudo " + shell_quote(safe_user) + " 2>/dev/null || true");
            commands.push_back("mkdir -p /etc/sudoers.d && echo " + shell_quote(safe_user + " ALL=(ALL) NOPASSWD:ALL") + " > /etc/sudoers.d/99-litewin-nopasswd && chmod 0440 /etc/sudoers.d/99-litewin-nopasswd");
            commands.push_back("for k in /lib/modules/* /usr/lib/modules/*; do if [ -d \"$k\" ]; then kver=$(basename \"$k\"); update-initramfs -c -k \"$kver\" || update-initramfs -u -k \"$kver\" || true; fi; done; update-initramfs -c -k all || update-initramfs -u -k all || true");
            commands.push_back("grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=ubuntu --recheck --no-floppy");
            commands.push_back("grub-install --target=x86_64-efi --efi-directory=/boot/efi --removable --recheck --no-floppy");
            commands.push_back("update-grub");
            break;

        case LinuxBootFamily::RedHat:
            commands.push_back("dnf remove -y 'anaconda*' livesys-scripts || true");
            commands.push_back("usermod -aG wheel " + shell_quote(safe_user) + " 2>/dev/null || true");
            commands.push_back("mkdir -p /etc/sudoers.d && echo " + shell_quote(safe_user + " ALL=(ALL) NOPASSWD:ALL") + " > /etc/sudoers.d/99-litewin-nopasswd && chmod 0440 /etc/sudoers.d/99-litewin-nopasswd");
            commands.push_back("touch /.autorelabel");
            commands.push_back("dracut --regenerate-all --force");
            commands.push_back("grub2-mkconfig -o /boot/grub2/grub.cfg");
            break;

        case LinuxBootFamily::Arch:
            commands.push_back("rm -f /etc/systemd/system/multi-user.target.wants/pacman-init.service /etc/systemd/system/choose-mirror.service");
            commands.push_back("if [ ! -f /boot/vmlinuz-linux ] && [ -f /usr/lib/modules/*/vmlinuz ]; then cp -f /usr/lib/modules/*/vmlinuz /boot/vmlinuz-linux; fi");
            commands.push_back("usermod -aG wheel " + shell_quote(safe_user) + " 2>/dev/null || true");
            commands.push_back("mkdir -p /etc/sudoers.d && echo " + shell_quote(safe_user + " ALL=(ALL) NOPASSWD:ALL") + " > /etc/sudoers.d/99-litewin-nopasswd && chmod 0440 /etc/sudoers.d/99-litewin-nopasswd");
            commands.push_back("mkinitcpio -P");
            commands.push_back("grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=Arch");
            commands.push_back("grub-mkconfig -o /boot/grub/grub.cfg");
            break;
    }

    return commands;
}

std::string LinuxInstaller::build_grub_default_config(bool is_dual_boot) {
    std::string cfg;
    cfg += "GRUB_DEFAULT=0\n";
    if (is_dual_boot) {
        cfg += "GRUB_TIMEOUT=5\n";
        cfg += "GRUB_TIMEOUT_STYLE=menu\n";
    } else {
        cfg += "GRUB_TIMEOUT=0\n";
        cfg += "GRUB_TIMEOUT_STYLE=hidden\n";
    }
    cfg += "GRUB_DISTRIBUTOR=`lsb_release -i -s 2> /dev/null || echo Debian`\n";
    cfg += "GRUB_CMDLINE_LINUX_DEFAULT=\"quiet splash\"\n";
    cfg += "GRUB_CMDLINE_LINUX=\"boot=local\"\n";
    return cfg;
}

std::string LinuxInstaller::build_grub_custom_entries(const std::vector<ForeignOsEntry>& foreign_os_entries) {
    std::string s;
    s += "#!/bin/sh\n";
    s += "exec tail -n +3 $0\n";
    s += "# This file provides an easy way to add custom menu entries.\n\n";

    for (const auto& entry : foreign_os_entries) {
        std::string label = entry.os_name;
        if (label.empty()) {
            if (entry.type == ForeignOsType::Windows) {
                label = "Windows Boot Manager (on " + entry.esp_device_path + ")";
            } else {
                label = "Ubuntu (on " + entry.esp_device_path + ")";
            }
        }
        std::string class_str = (entry.type == ForeignOsType::Windows)
            ? "--class windows --class os"
            : "--class ubuntu --class gnu-linux --class os";
        std::string loader = entry.efi_loader_path;
        if (loader.empty()) {
            loader = (entry.type == ForeignOsType::Windows)
                ? "/EFI/Microsoft/Boot/bootmgfw.efi"
                : "/EFI/ubuntu/grubx64.efi";
        }

        s += "menuentry '" + label + "' " + class_str + " {\n";
        s += "    insmod part_gpt\n";
        s += "    insmod fat\n";
        s += "    search --no-floppy --fs-uuid --set=root " + entry.esp_uuid + "\n";
        s += "    chainloader " + loader + "\n";
        s += "}\n\n";
    }
    return s;
}

std::string LinuxInstaller::build_forwarding_grub_cfg(
    std::string_view root_uuid,
    std::string_view kernel_filename,
    std::string_view initrd_filename
) {
    std::string kfile = kernel_filename.empty() ? "vmlinuz-generic" : std::string(kernel_filename);
    std::string ifile = initrd_filename.empty() ? "initrd.img-generic" : std::string(initrd_filename);

    std::string s;
    s += "insmod part_gpt\n";
    s += "insmod part_msdos\n";
    s += "insmod ext2\n";
    s += "insmod fat\n";
    s += "insmod search\n";
    s += "insmod search_fs_uuid\n";
    s += "insmod search_label\n";
    s += "insmod search_fs_file\n";
    s += "insmod configfile\n";
    s += "insmod normal\n";
    s += "insmod linux\n";
    s += "insmod gzio\n";
    s += "insmod xzio\n";
    s += "insmod zstd\n";
    s += "insmod test\n\n";

    if (!root_uuid.empty() && root_uuid != "ROOT-UUID-PLACEHOLDER") {
        s += "search --no-floppy --fs-uuid --set=root " + std::string(root_uuid) + "\n";
    }
    s += "if [ -z \"$root\" ]; then\n";
    s += "    search --no-floppy --label --set=root ROOT\n";
    s += "fi\n";
    s += "if [ -z \"$root\" ]; then\n";
    s += "    set root=(hd0,gpt2)\n";
    s += "fi\n\n";

    s += "set prefix=($root)'/boot/grub'\n";
    s += "if [ -f $prefix/grub.cfg ]; then\n";
    s += "    configfile $prefix/grub.cfg\n";
    s += "fi\n\n";

    std::string uuid_clause = (!root_uuid.empty() && root_uuid != "ROOT-UUID-PLACEHOLDER")
        ? ("root=UUID=" + std::string(root_uuid))
        : "root=LABEL=ROOT";

    s += "set timeout_style=menu\n";
    s += "set timeout=3\n";
    s += "menuentry 'Ubuntu Linux (Direct Boot)' --class ubuntu --class gnu-linux --class os {\n";
    s += "    insmod gzio\n";
    s += "    insmod zstd\n";
    s += "    insmod part_gpt\n";
    s += "    insmod ext2\n";
    if (!root_uuid.empty() && root_uuid != "ROOT-UUID-PLACEHOLDER") {
        s += "    search --no-floppy --fs-uuid --set=root " + std::string(root_uuid) + "\n";
    } else {
        s += "    search --no-floppy --label --set=root ROOT\n";
    }
    s += "    linux /boot/" + kfile + " " + uuid_clause + " ro boot=local\n";
    s += "    initrd /boot/" + ifile + "\n";
    s += "}\n";
    return s;
}

std::string LinuxInstaller::build_main_grub_cfg(
    std::string_view root_uuid,
    bool is_dual_boot,
    const std::vector<ForeignOsEntry>& foreign_os_entries,
    std::string_view kernel_filename,
    std::string_view initrd_filename
) {
    std::string kfile = kernel_filename.empty() ? "vmlinuz-generic" : std::string(kernel_filename);
    std::string ifile = initrd_filename.empty() ? "initrd.img-generic" : std::string(initrd_filename);
    std::string uuid_clause = (!root_uuid.empty() && root_uuid != "ROOT-UUID-PLACEHOLDER")
        ? ("root=UUID=" + std::string(root_uuid))
        : "root=LABEL=ROOT";

    std::string s;
    s += "# Automatically generated by LiteWin LinuxInstaller\n";
    s += "insmod part_gpt\n";
    s += "insmod part_msdos\n";
    s += "insmod ext2\n";
    s += "insmod fat\n";
    s += "insmod gzio\n";
    s += "insmod xzio\n";
    s += "insmod zstd\n";
    s += "insmod search\n";
    s += "insmod search_fs_uuid\n";
    s += "insmod search_label\n";
    s += "insmod search_fs_file\n";
    s += "insmod test\n";
    s += "insmod linux\n";
    s += "insmod normal\n";
    s += "insmod all_video\n";
    s += "insmod gfxterm\n\n";
    s += "set default=0\n";
    if (is_dual_boot) {
        s += "set timeout_style=menu\n";
        s += "set timeout=5\n\n";
    } else {
        s += "set timeout_style=menu\n";
        s += "set timeout=2\n\n";
    }

    s += "menuentry 'Ubuntu' --class ubuntu --class gnu-linux --class gnu --class os {\n";
    s += "    insmod gzio\n";
    s += "    insmod zstd\n";
    s += "    insmod part_gpt\n";
    s += "    insmod ext2\n";
    if (!root_uuid.empty() && root_uuid != "ROOT-UUID-PLACEHOLDER") {
        s += "    search --no-floppy --fs-uuid --set=root " + std::string(root_uuid) + "\n";
    } else {
        s += "    search --no-floppy --label --set=root ROOT\n";
    }
    s += "    linux /boot/" + kfile + " " + uuid_clause + " ro boot=local\n";
    s += "    initrd /boot/" + ifile + "\n";
    s += "}\n\n";

    s += "menuentry 'Ubuntu (Fallback Kernel)' --class ubuntu --class gnu-linux --class gnu --class os {\n";
    s += "    insmod gzio\n";
    s += "    insmod zstd\n";
    s += "    insmod part_gpt\n";
    s += "    insmod ext2\n";
    if (!root_uuid.empty() && root_uuid != "ROOT-UUID-PLACEHOLDER") {
        s += "    search --no-floppy --fs-uuid --set=root " + std::string(root_uuid) + "\n";
    } else {
        s += "    search --no-floppy --label --set=root ROOT\n";
    }
    s += "    linux /boot/vmlinuz-generic " + uuid_clause + " ro boot=local\n";
    s += "    initrd /boot/initrd.img-generic\n";
    s += "}\n\n";

    if (!foreign_os_entries.empty()) {
        std::string custom = build_grub_custom_entries(foreign_os_entries);
        auto pos = custom.find("menuentry ");
        if (pos != std::string::npos) {
            s += custom.substr(pos);
        }
    }

    return s;
}

std::vector<ForeignOsEntry> LinuxInstaller::discover_foreign_os_targets(std::string_view target_disk_path) {
    std::vector<ForeignOsEntry> entries;
    if (target_disk_path.empty()) {
        return entries;
    }

    std::string target(target_disk_path);
    std::error_code ec;
    if (!fs::exists("/sys/block", ec)) {
        return entries;
    }

    for (const auto& dir_entry : fs::directory_iterator("/sys/block", ec)) {
        std::string dev_name = dir_entry.path().filename().string();
        if (dev_name.starts_with("loop") || dev_name.starts_with("zram") ||
            dev_name.starts_with("ram") || dev_name.starts_with("sr") ||
            dev_name.starts_with("dm-")) {
            continue;
        }

        std::string full_dev_path = "/dev/" + dev_name;
        if (full_dev_path == target) {
            continue;
        }

        for (const auto& part_entry : fs::directory_iterator(dir_entry.path(), ec)) {
            std::string part_name = part_entry.path().filename().string();
            if (!part_name.starts_with(dev_name) || part_name == dev_name) {
                continue;
            }

            std::string part_path = "/dev/" + part_name;
            auto blkid_type = ProcessRunner::run("blkid", {"-s", "TYPE", "-o", "value", part_path});
            std::string fstype = blkid_type.stdout_output;
            while (!fstype.empty() && (fstype.back() == '\n' || fstype.back() == '\r' || fstype.back() == ' ')) {
                fstype.pop_back();
            }

            if (fstype != "vfat") {
                continue;
            }

            auto blkid_uuid = ProcessRunner::run("blkid", {"-s", "UUID", "-o", "value", part_path});
            std::string uuid = blkid_uuid.stdout_output;
            while (!uuid.empty() && (uuid.back() == '\n' || uuid.back() == '\r' || uuid.back() == ' ')) {
                uuid.pop_back();
            }
            if (uuid.empty()) {
                continue;
            }

            std::string tmp_mount = "/tmp/litewin_probe_" + part_name;
            fs::create_directories(tmp_mount, ec);
            auto mount_res = ProcessRunner::run("mount", {"-o", "ro", part_path, tmp_mount});
            if (!mount_res.success()) {
                fs::remove(tmp_mount, ec);
                continue;
            }

            if (fs::exists(tmp_mount + "/EFI/Microsoft/Boot/bootmgfw.efi", ec)) {
                ForeignOsEntry e;
                e.type = ForeignOsType::Windows;
                e.os_name = "Windows Boot Manager (on " + part_path + ")";
                e.esp_device_path = part_path;
                e.esp_uuid = uuid;
                e.efi_loader_path = "/EFI/Microsoft/Boot/bootmgfw.efi";
                entries.push_back(std::move(e));
            } else if (fs::exists(tmp_mount + "/EFI/ubuntu/grubx64.efi", ec) ||
                       fs::exists(tmp_mount + "/EFI/ubuntu/shimx64.efi", ec)) {
                ForeignOsEntry e;
                e.type = ForeignOsType::Linux;
                e.os_name = "Ubuntu (on " + part_path + ")";
                e.esp_device_path = part_path;
                e.esp_uuid = uuid;
                e.efi_loader_path = fs::exists(tmp_mount + "/EFI/ubuntu/grubx64.efi", ec)
                    ? "/EFI/ubuntu/grubx64.efi"
                    : "/EFI/ubuntu/shimx64.efi";
                entries.push_back(std::move(e));
            } else if (fs::exists(tmp_mount + "/EFI/BOOT/BOOTX64.EFI", ec)) {
                ForeignOsEntry e;
                e.type = ForeignOsType::Linux;
                e.os_name = "Linux (on " + part_path + ")";
                e.esp_device_path = part_path;
                e.esp_uuid = uuid;
                e.efi_loader_path = "/EFI/BOOT/BOOTX64.EFI";
                entries.push_back(std::move(e));
            }

            (void)ProcessRunner::run("umount", {"-l", tmp_mount});
            fs::remove(tmp_mount, ec);
        }
    }

    return entries;
}

static bool is_bootloader_command(const std::string& cmd) {
    return (cmd.find("grub") != std::string::npos || cmd.find("update-grub") != std::string::npos);
}

static void ensure_unix_user_and_shadow(const std::string& mnt, const std::string& safe_user, const std::string& password) {
    std::error_code ec;
    if (!fs::exists(mnt + "/etc", ec)) {
        return;
    }

    std::string pw_hash;
    if (!password.empty()) {
        auto hash_res = ProcessRunner::run("openssl", {"passwd", "-6", password});
        if (hash_res.success()) {
            pw_hash = hash_res.stdout_output;
            while (!pw_hash.empty() && (pw_hash.back() == '\n' || pw_hash.back() == '\r' || pw_hash.back() == ' ')) {
                pw_hash.pop_back();
            }
        }
    }

    // 1. Ensure user exists in /etc/passwd with /bin/bash shell
    int user_uid = 1000;
    const std::string passwd_path = mnt + "/etc/passwd";
    if (fs::exists(passwd_path, ec)) {
        std::ifstream in(passwd_path);
        std::vector<std::string> lines;
        std::string line;
        bool found_user = false;
        while (std::getline(in, line)) {
            if (line.starts_with(safe_user + ":")) {
                found_user = true;
                // Extract UID (3rd field) and ensure shell is /bin/bash
                auto p1 = line.find(':');
                auto p2 = (p1 != std::string::npos) ? line.find(':', p1 + 1) : std::string::npos;
                auto p3 = (p2 != std::string::npos) ? line.find(':', p2 + 1) : std::string::npos;
                if (p2 != std::string::npos && p3 != std::string::npos) {
                    try { user_uid = std::stoi(line.substr(p2 + 1, p3 - p2 - 1)); } catch (...) {}
                }
                auto last_colon = line.rfind(':');
                if (last_colon != std::string::npos) {
                    line = line.substr(0, last_colon + 1) + "/bin/bash";
                }
            } else {
                auto p1 = line.find(':');
                auto p2 = (p1 != std::string::npos) ? line.find(':', p1 + 1) : std::string::npos;
                auto p3 = (p2 != std::string::npos) ? line.find(':', p2 + 1) : std::string::npos;
                if (p2 != std::string::npos && p3 != std::string::npos) {
                    try {
                        int uid = std::stoi(line.substr(p2 + 1, p3 - p2 - 1));
                        if (uid >= 1000 && uid < 60000 && uid >= user_uid && !found_user) {
                            user_uid = uid + 1;
                        }
                    } catch (...) {}
                }
            }
            lines.push_back(line);
        }
        in.close();
        if (!found_user) {
            std::string uid_str = std::to_string(user_uid);
            lines.push_back(safe_user + ":x:" + uid_str + ":" + uid_str + ":" + safe_user + ":/home/" + safe_user + ":/bin/bash");
        }
        std::ofstream out(passwd_path, std::ios::trunc);
        for (const auto& l : lines) {
            out << l << "\n";
        }
    }

    // 2. Ensure group exists in /etc/group and user is in sudo/wheel/adm
    const std::string group_path = mnt + "/etc/group";
    if (fs::exists(group_path, ec)) {
        std::ifstream in(group_path);
        std::vector<std::string> lines;
        std::string line;
        bool found_group = false;
        while (std::getline(in, line)) {
            if (line.starts_with(safe_user + ":")) {
                found_group = true;
            }
            for (std::string_view gname : {"sudo:", "wheel:", "adm:", "users:"}) {
                if (line.starts_with(gname)) {
                    auto last_colon = line.rfind(':');
                    if (last_colon != std::string::npos) {
                        std::string members = line.substr(last_colon + 1);
                        if (members.find(safe_user) == std::string::npos) {
                            if (!members.empty()) line += ",";
                            line += safe_user;
                        }
                    }
                }
            }
            lines.push_back(line);
        }
        in.close();
        if (!found_group) {
            lines.push_back(safe_user + ":x:" + std::to_string(user_uid) + ":");
        }
        std::ofstream out(group_path, std::ios::trunc);
        for (const auto& l : lines) {
            out << l << "\n";
        }
    }

    // 3. Directly update /etc/shadow for both safe_user and root
    const std::string shadow_path = mnt + "/etc/shadow";
    if (fs::exists(shadow_path, ec)) {
        std::ifstream in(shadow_path);
        std::vector<std::string> lines;
        std::string line;
        bool found_user = false;
        while (std::getline(in, line)) {
            if (line.starts_with(safe_user + ":") || (!pw_hash.empty() && line.starts_with("root:"))) {
                if (line.starts_with(safe_user + ":")) {
                    found_user = true;
                }
                auto p1 = line.find(':');
                auto p2 = (p1 != std::string::npos) ? line.find(':', p1 + 1) : std::string::npos;
                if (p1 != std::string::npos && p2 != std::string::npos) {
                    line = line.substr(0, p1 + 1) + pw_hash + line.substr(p2);
                }
            }
            lines.push_back(line);
        }
        in.close();
        if (!found_user) {
            lines.push_back(safe_user + ":" + pw_hash + ":19800:0:99999:7:::");
        }
        std::ofstream out(shadow_path, std::ios::trunc);
        for (const auto& l : lines) {
            out << l << "\n";
        }
        out.close();
        (void)ProcessRunner::run("chmod", {"0640", shadow_path});
    }

    // 4. Ensure home directory and sudoers exist
    std::string home_dir = mnt + "/home/" + safe_user;
    fs::create_directories(home_dir, ec);
    std::string uid_gid = std::to_string(user_uid) + ":" + std::to_string(user_uid);
    (void)ProcessRunner::run("chown", {"-R", uid_gid, home_dir});
    fs::create_directories(mnt + "/etc/sudoers.d", ec);
    std::ofstream sudo_out(mnt + "/etc/sudoers.d/99-litewin-nopasswd", std::ios::trunc);
    sudo_out << safe_user << " ALL=(ALL) NOPASSWD:ALL\n";
    sudo_out.close();
    (void)ProcessRunner::run("chmod", {"0440", mnt + "/etc/sudoers.d/99-litewin-nopasswd"});
}

bool LinuxInstaller::update_installed_user_credentials(const LinuxInstallOptions& opts) {
    std::string safe_user = opts.username;
    std::transform(safe_user.begin(), safe_user.end(), safe_user.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (!is_valid_unix_identifier(safe_user) || !is_valid_unix_identifier(opts.hostname)) {
        return false;
    }

    if (opts.dry_run) {
        return true;
    }

    std::error_code ec;
    bool needs_unmount = false;
    std::string mnt = opts.mount_root;
    if (!fs::exists(mnt + "/etc", ec) && !opts.target_disk_path.empty()) {
        std::string root_part = format_partition_path(opts.target_disk_path, 2);
        fs::create_directories(mnt, ec);
        auto m_res = ProcessRunner::run("mount", {"-t", "ext4", root_part, mnt});
        if (m_res.success()) {
            needs_unmount = true;
        } else {
            return false;
        }
    }

    if (!fs::exists(mnt + "/etc", ec)) {
        if (needs_unmount) (void)ProcessRunner::run("umount", {"-f", mnt});
        return false;
    }

    fs::create_directories(mnt + "/dev", ec);
    fs::create_directories(mnt + "/proc", ec);
    bool dev_mounted = ProcessRunner::run("mount", {"--rbind", "/dev", mnt + "/dev"}).success();
    bool proc_mounted = ProcessRunner::run("mount", {"--bind", "/proc", mnt + "/proc"}).success();

    const std::string chroot_path_prefix = "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; ";

    // Update /etc/hostname
    auto host_cmd = chroot_path_prefix + "echo " + shell_quote(opts.hostname) + " > /etc/hostname";
    (void)ProcessRunner::run("chroot", {mnt, "/bin/sh", "-c", host_cmd});

    // Check if user exists; create if missing (safe fallback)
    std::string user_cmd = chroot_path_prefix + "(id -u " + shell_quote(safe_user) + " >/dev/null 2>&1 || "
        "useradd -m -s /bin/bash " + shell_quote(safe_user) + " 2>/dev/null || "
        "useradd --badname -m -s /bin/bash " + shell_quote(safe_user) + " 2>/dev/null || "
        "useradd --badnames -m -s /bin/bash " + shell_quote(safe_user) + " 2>/dev/null || true) && "
        "usermod -s /bin/bash " + shell_quote(safe_user) + " 2>/dev/null || true && "
        "mkdir -p /home/" + safe_user + " && chown -R " + shell_quote(safe_user + ":" + safe_user) + " /home/" + safe_user + " 2>/dev/null || true";
    (void)ProcessRunner::run("chroot", {mnt, "/bin/sh", "-c", user_cmd});

    // Update password if provided, or unlock for passwordless login
    if (!opts.password.empty()) {
        std::string pw_hash;
        auto hash_res = ProcessRunner::run("openssl", {"passwd", "-6", opts.password});
        if (hash_res.success()) {
            pw_hash = hash_res.stdout_output;
            while (!pw_hash.empty() && (pw_hash.back() == '\n' || pw_hash.back() == '\r' || pw_hash.back() == ' ')) {
                pw_hash.pop_back();
            }
        }
        if (pw_hash.empty()) {
            pw_hash = "$6$litewin$placeholder";
        }
        auto pass_cmd = chroot_path_prefix +
                        "usermod -p " + shell_quote(pw_hash) + " " + shell_quote(safe_user) + " 2>/dev/null || true && "
                        "usermod -p " + shell_quote(pw_hash) + " root 2>/dev/null || true && "
                        "usermod -U " + shell_quote(safe_user) + " 2>/dev/null || true && "
                        "echo " + shell_quote(safe_user + ":" + opts.password) + " | chpasswd -c SHA512 2>/dev/null || true && "
                        "echo " + shell_quote("root:" + opts.password) + " | chpasswd -c SHA512 2>/dev/null || true && "
                        "chmod 0640 /etc/shadow 2>/dev/null || true";
        (void)ProcessRunner::run("chroot", {mnt, "/bin/sh", "-c", pass_cmd});
    } else {
        auto unlock_cmd = chroot_path_prefix + "passwd -d " + shell_quote(safe_user) + " 2>/dev/null || usermod -p '' " + shell_quote(safe_user) + " 2>/dev/null || true";
        (void)ProcessRunner::run("chroot", {mnt, "/bin/sh", "-c", unlock_cmd});
    }

    (void)ProcessRunner::run("chroot", {mnt, "/bin/sh", "-c", chroot_path_prefix + "usermod -aG sudo " + shell_quote(safe_user) + " 2>/dev/null || usermod -aG wheel " + shell_quote(safe_user) + " 2>/dev/null || true"});
    (void)ProcessRunner::run("chroot", {mnt, "/bin/sh", "-c", chroot_path_prefix + "mkdir -p /etc/sudoers.d && echo " + shell_quote(safe_user + " ALL=(ALL) NOPASSWD:ALL") + " > /etc/sudoers.d/99-litewin-nopasswd && chmod 0440 /etc/sudoers.d/99-litewin-nopasswd"});

    ensure_unix_user_and_shadow(mnt, safe_user, opts.password);

    if (proc_mounted) {
        (void)ProcessRunner::run("umount", {"-f", mnt + "/proc"});
    }
    if (dev_mounted) {
        (void)ProcessRunner::run("umount", {"-R", "-f", mnt + "/dev"});
    }
    if (needs_unmount) {
        (void)ProcessRunner::run("umount", {"-f", mnt});
    }

    return true;
}

namespace {

struct ScopeUnmountGuard {
    bool active = true;
    bool dry_run = false;
    std::vector<std::string> mounts; // in mounting order

    void record_mount(std::string path) {
        mounts.push_back(std::move(path));
    }

    void release_mount(const std::string& path) {
        auto it = std::find(mounts.begin(), mounts.end(), path);
        if (it != mounts.end()) {
            mounts.erase(it);
        }
    }

    void dismiss() noexcept {
        active = false;
    }

    ~ScopeUnmountGuard() {
        if (!active || dry_run) {
            return;
        }
        for (auto it = mounts.rbegin(); it != mounts.rend(); ++it) {
            (void)ProcessRunner::run("umount", {"-l", *it});
        }
    }
};

} // namespace

LinuxInstallResult LinuxInstaller::execute(const LinuxInstallOptions& opts) {
    LinuxInstallResult result;

    auto report = [&](LinuxPipelineStep step, int pct, const std::string& msg) {
        result.step_logs.push_back("[" + std::string(to_string(step)) + "] " + msg);
        if (opts.progress_callback) {
            opts.progress_callback(step, pct, msg);
        }
    };

    auto fail = [&](LinuxPipelineStep step, const std::string& error_msg) -> LinuxInstallResult {
        result.success = false;
        result.failed_step = step;
        result.error_message = error_msg;
        result.step_logs.push_back("[Error at " + std::string(to_string(step)) + "] " + error_msg);
        return result;
    };

    if(!opts.dry_run && boot_target_status(opts.target_disk_path)!=BootTargetStatus::Allowed)
        return fail(LinuxPipelineStep::PreCheck,"The boot medium is protected, or its identity is unavailable. Reboot with the original medium connected.");
    // 1. PreCheck
    if (opts.target_disk_path.empty()) {
        return fail(LinuxPipelineStep::PreCheck, "Target disk path must not be empty");
    }
    if (!is_valid_unix_identifier(opts.username)) {
        return fail(LinuxPipelineStep::PreCheck, "Invalid username: must be 1-64 chars [a-zA-Z0-9_.-] and start with [a-zA-Z0-9_]");
    }
    if (!is_valid_unix_identifier(opts.hostname)) {
        return fail(LinuxPipelineStep::PreCheck, "Invalid hostname: must be 1-64 chars [a-zA-Z0-9_.-] and start with [a-zA-Z0-9_]");
    }
    if (opts.mount_root.empty()) {
        return fail(LinuxPipelineStep::PreCheck, "Mount root must not be empty");
    }
    if (opts.image_layer_paths.empty() && opts.download_url.empty()) {
        return fail(LinuxPipelineStep::PreCheck, "No image layers or download URL specified");
    }

    auto plan_opt = build_partition_plan(opts.target_disk_path, opts.target_disk_size);
    if (!plan_opt.has_value()) {
        return fail(LinuxPipelineStep::PreCheck, "Insufficient target disk size or invalid partition plan");
    }
    const auto& plan = *plan_opt;

    report(LinuxPipelineStep::PreCheck, 10, "Pre-check validation passed for " + opts.target_disk_path);
    result.last_completed_step = LinuxPipelineStep::PreCheck;

    // 2. PartitionDisk
    if (opts.dry_run) {
        result.executed_commands.push_back("sfdisk --force " + opts.target_disk_path);
        result.executed_commands.push_back("mkfs.fat -F32 -n EFI " + plan.esp_part);
        result.executed_commands.push_back("mkfs.ext4 -F -L ROOT " + plan.root_part);
        report(LinuxPipelineStep::PartitionDisk, 25, "Disk partitioned and formatted (dry-run)");
    } else {
        auto sfdisk_res = ProcessRunner::run("sfdisk", {"--force", opts.target_disk_path}, plan.sfdisk_script);
        if (!sfdisk_res.success()) {
            return fail(LinuxPipelineStep::PartitionDisk, "sfdisk partition application failed: " + sfdisk_res.error_message);
        }
        (void)ProcessRunner::run("partprobe", {opts.target_disk_path});
        (void)ProcessRunner::run("udevadm", {"settle"});

        auto fat_res = ProcessRunner::run("mkfs.fat", {"-F32", "-n", "EFI", plan.esp_part});
        if (!fat_res.success()) {
            return fail(LinuxPipelineStep::PartitionDisk, "ESP FAT32 formatting failed: " + fat_res.error_message);
        }

        auto ext4_res = ProcessRunner::run("mkfs.ext4", {"-F", "-L", "ROOT", plan.root_part});
        if (!ext4_res.success()) {
            return fail(LinuxPipelineStep::PartitionDisk, "Root ext4 formatting failed: " + ext4_res.error_message);
        }
        report(LinuxPipelineStep::PartitionDisk, 25, "Disk partitioned and formatted");
    }
    result.last_completed_step = LinuxPipelineStep::PartitionDisk;

    // RAII guard for cleanup on error in subsequent steps
    ScopeUnmountGuard unmount_guard;
    unmount_guard.dry_run = opts.dry_run;

    // 3. MountVolumes
    const bool has_efivars = opts.dry_run || fs::exists("/sys/firmware/efi/efivars");

    if (opts.dry_run) {
        result.executed_commands.push_back("mount -t ext4 " + plan.root_part + " " + opts.mount_root);
        report(LinuxPipelineStep::MountVolumes, 40, "Mounted target root partition (dry-run)");
    } else {
        std::error_code ec;
        fs::create_directories(opts.mount_root, ec);
        auto mount_root_res = ProcessRunner::run("mount", {"-t", "ext4", plan.root_part, opts.mount_root});
        if (!mount_root_res.success()) {
            return fail(LinuxPipelineStep::MountVolumes, "Failed to mount root partition: " + mount_root_res.error_message);
        }
        unmount_guard.record_mount(opts.mount_root);
        report(LinuxPipelineStep::MountVolumes, 40, "Mounted target root partition");
    }
    result.last_completed_step = LinuxPipelineStep::MountVolumes;

    // 4. AcquireImage
    const std::string iso_path = opts.mount_root + "/.litewin_source.iso";
    const std::string iso_mount = opts.mount_root + "_iso";
    bool iso_loop_mounted = false;
    std::vector<std::string> resolved_layers = opts.image_layer_paths;

    if (!opts.download_url.empty()) {
        report(LinuxPipelineStep::AcquireImage, 42, "Starting download of Linux ISO image...");
        if (opts.dry_run) {
            result.executed_commands.push_back("curl -f -L -o " + shell_quote(iso_path) + " " + shell_quote(opts.download_url));
            if (!opts.expected_sha256.empty()) {
                result.executed_commands.push_back("echo " + shell_quote(opts.expected_sha256 + "  " + iso_path) + " | sha256sum -c -");
            }
            result.executed_commands.push_back("mkdir -p " + shell_quote(iso_mount));
            result.executed_commands.push_back("mount -o loop,ro " + shell_quote(iso_path) + " " + shell_quote(iso_mount));
            iso_loop_mounted = true;
            report(LinuxPipelineStep::AcquireImage, 55, "Downloaded and loop-mounted ISO image (dry-run)");
        } else {
            DownloadOptions dl_opt;
            dl_opt.url = opts.download_url;
            dl_opt.destination_path = iso_path;
            dl_opt.expected_sha256 = opts.expected_sha256;
            dl_opt.strict_official_source=opts.strict_official_source;
            dl_opt.expected_size=opts.expected_size;
            dl_opt.status_callback = opts.download_status_callback;

            int last_reported_pct = -1;
            dl_opt.progress_callback = [&](std::uint64_t dlnow, std::uint64_t dltotal) -> bool {
                if (dltotal > 0) {
                    int pct = static_cast<int>((dlnow * 100ULL) / dltotal);
                    if (pct >= last_reported_pct + 2 || (pct == 100 && last_reported_pct < 100)) {
                        last_reported_pct = pct;
                        std::uint64_t dl_mb = dlnow / (1024 * 1024);
                        std::uint64_t total_mb = dltotal / (1024 * 1024);
                        int overall_pct = 40 + (pct * 15) / 100;
                        report(LinuxPipelineStep::AcquireImage, overall_pct,
                               "Downloading ISO: " + std::to_string(dl_mb) + " / " +
                               std::to_string(total_mb) + " MB (" + std::to_string(pct) + "%)");
                    }
                }
                return true;
            };

            auto dl_res = HttpDownloader::download(dl_opt);
            if (!dl_res.success) {
                return fail(LinuxPipelineStep::AcquireImage, "Failed to download ISO: " + dl_res.error_message);
            }

            std::error_code ec;
            fs::create_directories(iso_mount, ec);
            auto loop_proc = ProcessRunner::run("mount", {"-o", "loop,ro", iso_path, iso_mount});
            if (!loop_proc.success()) {
                return fail(LinuxPipelineStep::AcquireImage, "Failed to loop-mount ISO: " + loop_proc.error_message);
            }
            unmount_guard.record_mount(iso_mount);
            iso_loop_mounted = true;
            report(LinuxPipelineStep::AcquireImage, 55, "Downloaded and loop-mounted ISO image");
        }
    } else {
        if (opts.download_status_callback) {
            DownloadStatus local;
            local.phase = DownloadPhase::Cached;
            local.transport = DownloadTransport::Local;
            local.hash = DownloadHash::NotProvided;
            opts.download_status_callback(local);
        }
        report(LinuxPipelineStep::AcquireImage, 55, "Using local image layer(s)");
    }

    // Resolve relative layer paths against iso_mount if ISO was loop mounted
    if (iso_loop_mounted) {
        if (resolved_layers.empty()) {
            std::string default_layer = (opts.image_format == LinuxImageFormat::Erofs)
                                            ? "LiveOS/rootfs.erofs"
                                            : "casper/filesystem.squashfs";
            resolved_layers.push_back(iso_mount + "/" + default_layer);
        } else {
            for (auto& l : resolved_layers) {
                if (!l.empty() && l.front() != '/') {
                    l = iso_mount + "/" + l;
                }
            }
        }
        if (!opts.dry_run) {
            std::error_code ec;
            std::vector<std::string> valid_layers;
            for (const auto& l : resolved_layers) {
                if (fs::exists(l, ec)) {
                    valid_layers.push_back(l);
                }
            }
            if (!valid_layers.empty() && valid_layers.back().ends_with(".squashfs")) {
                std::string stem = valid_layers.back().substr(0, valid_layers.back().size() - 9);
                std::string generic_layer = stem + ".generic.squashfs";
                if (fs::exists(generic_layer, ec) &&
                    std::find(valid_layers.begin(), valid_layers.end(), generic_layer) == valid_layers.end()) {
                    valid_layers.push_back(generic_layer);
                }
            }
            if (valid_layers.empty()) {
                const std::vector<std::string> fallbacks = {
                    iso_mount + "/casper/filesystem.squashfs",
                    iso_mount + "/casper/minimal.squashfs",
                    iso_mount + "/casper/minimal.standard.squashfs",
                    iso_mount + "/casper/minimal.standard.generic.squashfs",
                    iso_mount + "/casper/ubuntu-server-minimal.squashfs",
                    iso_mount + "/casper/ubuntu-server-minimal.ubuntu-server.squashfs",
                    iso_mount + "/casper/ubuntu-server-minimal.ubuntu-server.generic.squashfs"
                };
                for (const auto& fb : fallbacks) {
                    if (fs::exists(fb, ec)) {
                        valid_layers.push_back(fb);
                    }
                }
            }
            if (!valid_layers.empty()) {
                resolved_layers = std::move(valid_layers);
            }
        }
    }
    result.last_completed_step = LinuxPipelineStep::AcquireImage;

    // 5. ExtractRootFS
    auto extract_cmds = build_extraction_commands(opts.image_format, resolved_layers, opts.mount_root);
    if (opts.dry_run) {
        for (const auto& cmd : extract_cmds) {
            result.executed_commands.push_back(cmd);
        }
        if (!opts.kernel_source_path.empty()) {
            std::string src_k = (iso_loop_mounted && !opts.kernel_source_path.starts_with("/"))
                                    ? (iso_mount + "/" + opts.kernel_source_path)
                                    : opts.kernel_source_path;
            result.executed_commands.push_back("mkdir -p " + shell_quote(opts.mount_root + "/boot"));
            result.executed_commands.push_back("cp -f " + shell_quote(src_k) + " " + shell_quote(opts.mount_root + "/boot/vmlinuz-linux"));
        } else if (iso_loop_mounted) {
            result.executed_commands.push_back("mkdir -p " + shell_quote(opts.mount_root + "/boot"));
            result.executed_commands.push_back("cp -f " + shell_quote(iso_mount + "/casper/vmlinuz") + " " + shell_quote(opts.mount_root + "/boot/vmlinuz-generic"));
            result.executed_commands.push_back("cp -f " + shell_quote(iso_mount + "/casper/initrd") + " " + shell_quote(opts.mount_root + "/boot/initrd.img-generic"));
        }
        if (iso_loop_mounted) {
            result.executed_commands.push_back("umount -f " + iso_mount);
            result.executed_commands.push_back("rm -f " + shell_quote(iso_path));
            iso_loop_mounted = false;
        }
        report(LinuxPipelineStep::ExtractRootFS, 70, "Extracted " + std::to_string(extract_cmds.size()) + " layer(s) (dry-run)");
    } else {
        bool extracted_via_overlay = false;
        if ((opts.image_format == LinuxImageFormat::LayeredSquashFS ||
             (opts.image_format == LinuxImageFormat::SingleSquashFS &&
              !resolved_layers.empty() && !resolved_layers.front().ends_with("squashfs.img"))) &&
            !resolved_layers.empty()) {
            std::error_code ec;
            const std::string sq_base = "/tmp/litewin_sq";
            fs::remove_all(sq_base, ec);
            fs::create_directories(sq_base, ec);
            std::vector<std::string> layer_mounts;
            bool all_mounted = true;
            for (std::size_t i = 0; i < resolved_layers.size(); ++i) {
                std::string lm = sq_base + "/layer_" + std::to_string(i);
                fs::create_directories(lm, ec);
                auto m_res = ProcessRunner::run("mount", {"-t", "squashfs", "-o", "loop,ro", resolved_layers[i], lm});
                if (!m_res.success()) {
                    all_mounted = false;
                    break;
                }
                unmount_guard.record_mount(lm);
                layer_mounts.push_back(lm);
            }

            std::string copy_src;
            std::string merged_mount = sq_base + "/merged";
            bool overlay_mounted = false;
            if (all_mounted) {
                if (layer_mounts.size() == 1) {
                    copy_src = layer_mounts.front();
                } else {
                    std::string lowerdir;
                    for (auto it = layer_mounts.rbegin(); it != layer_mounts.rend(); ++it) {
                        if (!lowerdir.empty()) lowerdir += ":";
                        lowerdir += *it;
                    }
                    fs::create_directories(merged_mount, ec);
                    auto ov_res = ProcessRunner::run("mount", {"-t", "overlay", "overlay", "-o", "ro,lowerdir=" + lowerdir, merged_mount});
                    if (ov_res.success()) {
                        unmount_guard.record_mount(merged_mount);
                        overlay_mounted = true;
                        copy_src = merged_mount;
                    }
                }
            }

            if (!copy_src.empty()) {
                auto cp_res = ProcessRunner::run("cp", {"-a", copy_src + "/.", opts.mount_root + "/"});
                if (cp_res.success()) {
                    extracted_via_overlay = true;
                }
            }

            if (overlay_mounted) {
                (void)ProcessRunner::run("umount", {"-f", merged_mount});
                unmount_guard.release_mount(merged_mount);
            }
            for (auto it = layer_mounts.rbegin(); it != layer_mounts.rend(); ++it) {
                (void)ProcessRunner::run("umount", {"-f", *it});
                unmount_guard.release_mount(*it);
            }
            fs::remove_all(sq_base, ec);
        }

        if (!extracted_via_overlay) {
            for (const auto& cmd : extract_cmds) {
                auto proc = ProcessRunner::run("sh", {"-c", cmd});
                if (!proc.success() && proc.exit_code != 2) {
                    std::string err = !proc.stderr_output.empty() ? proc.stderr_output :
                                      (!proc.error_message.empty() ? proc.error_message : ("exit code " + std::to_string(proc.exit_code)));
                    while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) {
                        err.pop_back();
                    }
                    return fail(LinuxPipelineStep::ExtractRootFS, "Image extraction failed: " + err);
                }
            }
        }
        if (!opts.kernel_source_path.empty()) {
            std::string src_k = (iso_loop_mounted && !opts.kernel_source_path.starts_with("/"))
                                    ? (iso_mount + "/" + opts.kernel_source_path)
                                    : opts.kernel_source_path;
            std::error_code ec;
            fs::create_directories(opts.mount_root + "/boot", ec);
            auto cp_proc = ProcessRunner::run("cp", {"-f", src_k, opts.mount_root + "/boot/vmlinuz-linux"});
            if (!cp_proc.success()) {
                return fail(LinuxPipelineStep::ExtractRootFS, "Kernel copy failed: " + cp_proc.error_message);
            }
        } else if (iso_loop_mounted) {
            std::error_code ec;
            std::string casper_vmlinuz;
            for (const std::string& cand : {std::string("/casper/vmlinuz"), std::string("/casper/hwe-vmlinuz")}) {
                if (fs::exists(iso_mount + cand, ec)) {
                    casper_vmlinuz = iso_mount + cand;
                    break;
                }
            }
            if (!casper_vmlinuz.empty()) {
                std::string kver = "generic";
                for (const std::string& mod_base : {opts.mount_root + "/lib/modules", opts.mount_root + "/usr/lib/modules"}) {
                    if (fs::exists(mod_base, ec)) {
                        for (const auto& mod_dir : fs::directory_iterator(mod_base, ec)) {
                            if (mod_dir.is_directory(ec)) {
                                kver = mod_dir.path().filename().string();
                                break;
                            }
                        }
                        if (kver != "generic") break;
                    }
                }
                fs::create_directories(opts.mount_root + "/boot", ec);
                std::string dst_vmlinuz = opts.mount_root + "/boot/vmlinuz-" + kver;
                if (!fs::exists(dst_vmlinuz, ec) || fs::file_size(dst_vmlinuz, ec) == 0) {
                    fs::remove(dst_vmlinuz, ec);
                    (void)ProcessRunner::run("cp", {"-f", casper_vmlinuz, dst_vmlinuz});
                }
                if (kver != "generic") {
                    std::string gen_vmlinuz = opts.mount_root + "/boot/vmlinuz-generic";
                    if (!fs::exists(gen_vmlinuz, ec)) {
                        fs::create_symlink("vmlinuz-" + kver, gen_vmlinuz, ec);
                    }
                }
                std::string link_vmlinuz = opts.mount_root + "/boot/vmlinuz";
                if (!fs::exists(link_vmlinuz, ec)) {
                    fs::create_symlink("vmlinuz-" + kver, link_vmlinuz, ec);
                }

                std::string dst_initrd = opts.mount_root + "/boot/initrd.img-" + kver;
                if (!fs::exists(dst_initrd, ec) || fs::file_size(dst_initrd, ec) == 0) {
                    for (const std::string& initrd_name : {std::string("/casper/initrd"), std::string("/casper/hwe-initrd"), std::string("/casper/initrd.lz"), std::string("/casper/initrd.gz")}) {
                        if (fs::exists(iso_mount + initrd_name, ec)) {
                            fs::remove(dst_initrd, ec);
                            (void)ProcessRunner::run("cp", {"-f", iso_mount + initrd_name, dst_initrd});
                            break;
                        }
                    }
                }
                if (fs::exists(dst_initrd, ec)) {
                    if (kver != "generic") {
                        std::string gen_initrd = opts.mount_root + "/boot/initrd.img-generic";
                        if (!fs::exists(gen_initrd, ec)) {
                            fs::create_symlink("initrd.img-" + kver, gen_initrd, ec);
                        }
                    }
                    std::string link_initrd = opts.mount_root + "/boot/initrd.img";
                    if (!fs::exists(link_initrd, ec)) {
                        fs::create_symlink("initrd.img-" + kver, link_initrd, ec);
                    }
                }
            }
        }
        if (iso_loop_mounted) {
            (void)ProcessRunner::run("umount", {"-f", iso_mount});
            unmount_guard.release_mount(iso_mount);
            std::error_code ec;
            fs::remove(iso_path, ec);
            fs::remove(iso_mount, ec);
            iso_loop_mounted = false;
        }
        report(LinuxPipelineStep::ExtractRootFS, 70, "Extracted " + std::to_string(resolved_layers.size()) + " layer(s)");
    }
    result.last_completed_step = LinuxPipelineStep::ExtractRootFS;

    // 6. ConfigureChroot
    if (opts.dry_run) {
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/boot/efi");
        result.executed_commands.push_back("mount -t vfat " + plan.esp_part + " " + opts.mount_root + "/boot/efi");
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/dev");
        result.executed_commands.push_back("mount --rbind /dev " + opts.mount_root + "/dev");
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/dev/pts");
        result.executed_commands.push_back("mount --bind /dev/pts " + opts.mount_root + "/dev/pts");
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/proc");
        result.executed_commands.push_back("mount --bind /proc " + opts.mount_root + "/proc");
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/sys");
        result.executed_commands.push_back("mount --rbind /sys " + opts.mount_root + "/sys");
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/run");
        result.executed_commands.push_back("mount --bind /run " + opts.mount_root + "/run");
        if (has_efivars) {
            result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/sys/firmware/efi/efivars");
            result.executed_commands.push_back("mount -t efivarfs efivarfs " + opts.mount_root + "/sys/firmware/efi/efivars");
        }
    } else {
        std::error_code ec;
        fs::create_directories(opts.mount_root + "/boot/efi", ec);
        auto mount_esp_res = ProcessRunner::run("mount", {"-t", "vfat", plan.esp_part, opts.mount_root + "/boot/efi"});
        if (!mount_esp_res.success()) {
            return fail(LinuxPipelineStep::ConfigureChroot, "Failed to mount EFI partition: " + mount_esp_res.error_message);
        }
        unmount_guard.record_mount(opts.mount_root + "/boot/efi");

        fs::create_directories(opts.mount_root + "/dev", ec);
        (void)ProcessRunner::run("mount", {"--rbind", "/dev", opts.mount_root + "/dev"});
        unmount_guard.record_mount(opts.mount_root + "/dev");

        fs::create_directories(opts.mount_root + "/dev/pts", ec);
        (void)ProcessRunner::run("mount", {"--bind", "/dev/pts", opts.mount_root + "/dev/pts"});
        unmount_guard.record_mount(opts.mount_root + "/dev/pts");

        fs::create_directories(opts.mount_root + "/proc", ec);
        (void)ProcessRunner::run("mount", {"--bind", "/proc", opts.mount_root + "/proc"});
        unmount_guard.record_mount(opts.mount_root + "/proc");

        fs::create_directories(opts.mount_root + "/sys", ec);
        (void)ProcessRunner::run("mount", {"--rbind", "/sys", opts.mount_root + "/sys"});
        unmount_guard.record_mount(opts.mount_root + "/sys");

        fs::create_directories(opts.mount_root + "/run", ec);
        (void)ProcessRunner::run("mount", {"--bind", "/run", opts.mount_root + "/run"});
        unmount_guard.record_mount(opts.mount_root + "/run");

        if (fs::exists("/sys/firmware/efi/efivars")) {
            fs::create_directories(opts.mount_root + "/sys/firmware/efi/efivars", ec);
            (void)ProcessRunner::run("mount", {"-t", "efivarfs", "efivarfs", opts.mount_root + "/sys/firmware/efi/efivars"});
            unmount_guard.record_mount(opts.mount_root + "/sys/firmware/efi/efivars");
        }
    }

    auto foreign_os = opts.foreign_os_entries;
    if (foreign_os.empty() && !opts.dry_run) {
        foreign_os = discover_foreign_os_targets(opts.target_disk_path);
    }
    const bool is_dual_boot = !foreign_os.empty();
    std::string grub_default_cfg = build_grub_default_config(is_dual_boot);
    std::string grub_custom_cfg = build_grub_custom_entries(foreign_os);

    if (opts.dry_run) {
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/etc/default");
        result.executed_commands.push_back("cat << 'EOF' > " + opts.mount_root + "/etc/default/grub\n" + grub_default_cfg + "EOF");
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/etc/grub.d");
        result.executed_commands.push_back("cat << 'EOF' > " + opts.mount_root + "/etc/grub.d/40_custom\n" + grub_custom_cfg + "EOF");
        result.executed_commands.push_back("chmod 755 " + opts.mount_root + "/etc/grub.d/40_custom");
    } else {
        std::error_code ec;
        fs::create_directories(opts.mount_root + "/etc/default", ec);
        std::ofstream(opts.mount_root + "/etc/default/grub") << grub_default_cfg;
        fs::create_directories(opts.mount_root + "/etc/grub.d", ec);
        std::ofstream(opts.mount_root + "/etc/grub.d/40_custom") << grub_custom_cfg;
        (void)ProcessRunner::run("chmod", {"755", opts.mount_root + "/etc/grub.d/40_custom"});
    }

    auto chroot_cmds = build_chroot_commands(opts);
    std::vector<std::string> config_cmds;
    std::vector<std::string> boot_cmds;
    for (const auto& cmd : chroot_cmds) {
        if (is_bootloader_command(cmd)) {
            boot_cmds.push_back(cmd);
        } else {
            config_cmds.push_back(cmd);
        }
    }

    // Configure chroot
    const std::string chroot_path_prefix = "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; ";
    if (opts.dry_run) {
        for (const auto& cmd : config_cmds) {
            result.executed_commands.push_back("chroot " + opts.mount_root + " " + cmd);
        }
        report(LinuxPipelineStep::ConfigureChroot, 85, "Configured chroot environment (dry-run)");
    } else {
        for (const auto& cmd : config_cmds) {
            auto proc = ProcessRunner::run("chroot", {opts.mount_root, "/bin/sh", "-c", chroot_path_prefix + cmd});
            if (!proc.success()) {
                std::string err = !proc.stderr_output.empty() ? proc.stderr_output :
                                  (!proc.error_message.empty() ? proc.error_message : cmd);
                while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
                return fail(LinuxPipelineStep::ConfigureChroot, "Chroot config command failed (" + cmd + "): " + err);
            }
        }
        std::string safe_user = opts.username;
        std::transform(safe_user.begin(), safe_user.end(), safe_user.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (safe_user.empty() || !is_valid_unix_identifier(safe_user)) {
            safe_user = "user";
        }
        ensure_unix_user_and_shadow(opts.mount_root, safe_user, opts.password);
        report(LinuxPipelineStep::ConfigureChroot, 85, "Configured chroot environment");
    }
    result.last_completed_step = LinuxPipelineStep::ConfigureChroot;

    // 7. InstallBootloader
    std::string root_uuid;
    if (!opts.dry_run) {
        auto blk_r = ProcessRunner::run("blkid", {"-s", "UUID", "-o", "value", plan.root_part});
        root_uuid = blk_r.stdout_output;
        while (!root_uuid.empty() && (root_uuid.back() == '\n' || root_uuid.back() == '\r' || root_uuid.back() == ' ')) {
            root_uuid.pop_back();
        }
        if (root_uuid.empty()) {
            auto lsblk_r = ProcessRunner::run("lsblk", {"-no", "UUID", plan.root_part});
            root_uuid = lsblk_r.stdout_output;
            while (!root_uuid.empty() && (root_uuid.back() == '\n' || root_uuid.back() == '\r' || root_uuid.back() == ' ')) {
                root_uuid.pop_back();
            }
        }
    }
    if (root_uuid.empty()) {
        root_uuid = "ROOT-UUID-PLACEHOLDER";
    }

    if (opts.dry_run) {
        for (const auto& cmd : boot_cmds) {
            result.executed_commands.push_back("chroot " + opts.mount_root + " " + cmd);
        }
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/boot/efi/EFI/BOOT");
        result.executed_commands.push_back("cp -f " + opts.mount_root + "/boot/efi/EFI/ubuntu/grubx64.efi " + opts.mount_root + "/boot/efi/EFI/BOOT/BOOTX64.EFI");
        std::string stub = build_forwarding_grub_cfg(root_uuid);
        result.executed_commands.push_back("cat << 'EOF' > " + opts.mount_root + "/boot/efi/EFI/ubuntu/grub.cfg\n" + stub + "EOF");
        result.executed_commands.push_back("cat << 'EOF' > " + opts.mount_root + "/boot/efi/EFI/BOOT/grub.cfg\n" + stub + "EOF");
        result.executed_commands.push_back("mkdir -p " + opts.mount_root + "/boot/efi/EFI/Litewin");
        result.executed_commands.push_back("echo 'INSTALLED_OS_READY' > " + opts.mount_root + "/boot/efi/EFI/Litewin/boot_stage.flag");
        result.executed_commands.push_back("efibootmgr -c -d " + opts.target_disk_path + " -p 1 -L Ubuntu -l \\EFI\\ubuntu\\grubx64.efi");
        result.executed_commands.push_back("efibootmgr -o 0001,0000,0002");
        result.executed_commands.push_back("efibootmgr -n 0001");
        report(LinuxPipelineStep::InstallBootloader, 92, "Installed bootloader and configured UEFI boot table (dry-run)");
    } else {
        bool boot_failed = false;
        for (const auto& cmd : boot_cmds) {
            auto proc = ProcessRunner::run("chroot", {opts.mount_root, "/bin/sh", "-c", chroot_path_prefix + cmd});
            if (!proc.success()) {
                boot_failed = true;
                break;
            }
        }
        std::error_code ec;
        if (boot_failed) {
            if (!fs::exists(opts.mount_root + "/usr/lib/grub/x86_64-efi", ec) &&
                fs::exists("/usr/lib/grub/x86_64-efi", ec)) {
                fs::create_directories(opts.mount_root + "/usr/lib/grub", ec);
                fs::copy("/usr/lib/grub/x86_64-efi",
                         opts.mount_root + "/usr/lib/grub/x86_64-efi",
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                         ec);
            }
            (void)ProcessRunner::run("grub-install", {
                "--target=x86_64-efi",
                "--directory=/usr/lib/grub/x86_64-efi",
                "--boot-directory=" + opts.mount_root + "/boot",
                "--efi-directory=" + opts.mount_root + "/boot/efi",
                "--bootloader-id=ubuntu",
                "--recheck",
                "--no-floppy"
            });
            (void)ProcessRunner::run("grub-install", {
                "--target=x86_64-efi",
                "--directory=/usr/lib/grub/x86_64-efi",
                "--boot-directory=" + opts.mount_root + "/boot",
                "--efi-directory=" + opts.mount_root + "/boot/efi",
                "--removable",
                "--recheck",
                "--no-floppy"
            });
            (void)ProcessRunner::run("chroot", {opts.mount_root, "/bin/sh", "-c", "update-grub || grub-mkconfig -o /boot/grub/grub.cfg"});
        }

        std::string detected_kernel = "vmlinuz-generic";
        std::string detected_initrd = "initrd.img-generic";
        if (fs::exists(opts.mount_root + "/boot", ec)) {
            for (const auto& entry : fs::directory_iterator(opts.mount_root + "/boot", ec)) {
                std::string fname = entry.path().filename().string();
                if (fname.starts_with("vmlinuz-") && fname != "vmlinuz-generic" && !fname.ends_with(".old")) {
                    detected_kernel = fname;
                    std::string ksuffix = fname.substr(std::string("vmlinuz-").size());
                    if (fs::exists(opts.mount_root + "/boot/initrd.img-" + ksuffix, ec)) {
                        detected_initrd = "initrd.img-" + ksuffix;
                    } else if (fs::exists(opts.mount_root + "/boot/initramfs-" + ksuffix + ".img", ec)) {
                        detected_initrd = "initramfs-" + ksuffix + ".img";
                    }
                    break;
                }
            }
            if (detected_kernel == "vmlinuz-generic" &&
                !fs::exists(opts.mount_root + "/boot/vmlinuz-generic", ec) &&
                fs::exists(opts.mount_root + "/boot/vmlinuz-linux", ec)) {
                detected_kernel = "vmlinuz-linux";
                if (fs::exists(opts.mount_root + "/boot/initramfs-linux.img", ec)) {
                    detected_initrd = "initramfs-linux.img";
                }
            }
            if (detected_kernel != "vmlinuz-generic" && !fs::exists(opts.mount_root + "/boot/vmlinuz-generic", ec)) {
                fs::create_symlink(detected_kernel, opts.mount_root + "/boot/vmlinuz-generic", ec);
                if (ec) {
                    ec.clear();
                    fs::copy_file(opts.mount_root + "/boot/" + detected_kernel, opts.mount_root + "/boot/vmlinuz-generic", fs::copy_options::overwrite_existing, ec);
                }
            }
            if (detected_initrd != "initrd.img-generic" && !fs::exists(opts.mount_root + "/boot/initrd.img-generic", ec)) {
                fs::create_symlink(detected_initrd, opts.mount_root + "/boot/initrd.img-generic", ec);
                if (ec) {
                    ec.clear();
                    fs::copy_file(opts.mount_root + "/boot/" + detected_initrd, opts.mount_root + "/boot/initrd.img-generic", fs::copy_options::overwrite_existing, ec);
                }
            }
            if (!fs::exists(opts.mount_root + "/boot/vmlinuz", ec)) {
                fs::create_symlink(detected_kernel, opts.mount_root + "/boot/vmlinuz", ec);
            }
            if (!fs::exists(opts.mount_root + "/boot/initrd.img", ec)) {
                fs::create_symlink(detected_initrd, opts.mount_root + "/boot/initrd.img", ec);
            }
        }

        std::string stub = build_forwarding_grub_cfg(root_uuid, detected_kernel, detected_initrd);
        fs::create_directories(opts.mount_root + "/boot/efi/EFI/ubuntu", ec);
        fs::create_directories(opts.mount_root + "/boot/efi/EFI/BOOT", ec);
        std::ofstream(opts.mount_root + "/boot/efi/EFI/ubuntu/grub.cfg") << stub;
        std::ofstream(opts.mount_root + "/boot/efi/EFI/BOOT/grub.cfg") << stub;

        std::string main_grub_cfg = opts.mount_root + "/boot/grub/grub.cfg";
        if (!fs::exists(main_grub_cfg, ec)) {
            (void)ProcessRunner::run("chroot", {opts.mount_root, "/bin/sh", "-c", "update-grub || grub-mkconfig -o /boot/grub/grub.cfg"});
        }
        bool valid_main_grub = false;
        if (fs::exists(main_grub_cfg, ec) && fs::file_size(main_grub_cfg, ec) > 0) {
            std::ifstream ifs(main_grub_cfg);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (content.find("menuentry ") != std::string::npos &&
                content.find(detected_kernel) != std::string::npos &&
                content.find("boot=local") != std::string::npos) {
                valid_main_grub = true;
            }
        }
        if (!valid_main_grub) {
            fs::create_directories(opts.mount_root + "/boot/grub", ec);
            std::ofstream(main_grub_cfg) << build_main_grub_cfg(
                root_uuid, is_dual_boot, foreign_os, detected_kernel, detected_initrd
            );
        }

        std::string target_grubx64 = opts.mount_root + "/boot/efi/EFI/ubuntu/grubx64.efi";
        if (!fs::exists(target_grubx64, ec) || fs::file_size(target_grubx64, ec) == 0) {
            (void)ProcessRunner::run("grub-mkstandalone", {
                "-O", "x86_64-efi",
                "-o", target_grubx64,
                "boot/grub/grub.cfg=" + main_grub_cfg
            });
        }

        std::string target_bootx64 = opts.mount_root + "/boot/efi/EFI/BOOT/BOOTX64.EFI";
        if (!fs::exists(target_bootx64, ec) || fs::file_size(target_bootx64, ec) == 0) {
            std::string shim = opts.mount_root + "/boot/efi/EFI/ubuntu/shimx64.efi";
            if (fs::exists(shim, ec) && fs::file_size(shim, ec) > 0) {
                fs::copy_file(shim, target_bootx64, fs::copy_options::overwrite_existing, ec);
            } else if (fs::exists(target_grubx64, ec) && fs::file_size(target_grubx64, ec) > 0) {
                fs::copy_file(target_grubx64, target_bootx64, fs::copy_options::overwrite_existing, ec);
            }
        }

        fs::create_directories(opts.mount_root + "/boot/efi/EFI/Litewin", ec);
        std::ofstream(opts.mount_root + "/boot/efi/EFI/Litewin/boot_stage.flag") << "INSTALLED_OS_READY\n";

        std::string loader_rel = (fs::exists(opts.mount_root + "/boot/efi/EFI/ubuntu/shimx64.efi", ec) &&
                                  fs::file_size(opts.mount_root + "/boot/efi/EFI/ubuntu/shimx64.efi", ec) > 0)
            ? "\\EFI\\ubuntu\\shimx64.efi"
            : "\\EFI\\ubuntu\\grubx64.efi";
        auto efi_res = ProcessRunner::run("efibootmgr", {"-c", "-d", opts.target_disk_path, "-p", "1", "-L", "Ubuntu", "-l", loader_rel});
        if (efi_res.success()) {
            std::istringstream iss(efi_res.stdout_output);
            std::string line;
            std::string boot_num;
            std::string current_order;
            while (std::getline(iss, line)) {
                if (line.starts_with("BootOrder:")) {
                    auto o_pos = line.find(':');
                    if (o_pos != std::string::npos && o_pos + 1 < line.size()) {
                        current_order = line.substr(o_pos + 1);
                        current_order.erase(0, current_order.find_first_not_of(" \t"));
                        current_order.erase(current_order.find_last_not_of(" \t\r\n") + 1);
                    }
                }
                if (line.starts_with("BootCurrent:") || line.find("Ubuntu") != std::string::npos) {
                    auto b_pos = line.find("Boot");
                    if (b_pos != std::string::npos && line.size() >= b_pos + 8) {
                        std::string num = line.substr(b_pos + 4, 4);
                        if (std::all_of(num.begin(), num.end(), [](char c) { return std::isxdigit(c); })) {
                            boot_num = num;
                        }
                    }
                }
            }
            if (!boot_num.empty()) {
                if (!current_order.empty()) {
                    std::string new_order = boot_num;
                    std::istringstream order_stream(current_order);
                    std::string entry;
                    while (std::getline(order_stream, entry, ',')) {
                        while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) entry.erase(0, 1);
                        while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t' || entry.back() == '\r')) entry.pop_back();
                        if (!entry.empty() && entry != boot_num) {
                            new_order += "," + entry;
                        }
                    }
                    (void)ProcessRunner::run("efibootmgr", {"-o", new_order});
                }
                (void)ProcessRunner::run("efibootmgr", {"-n", boot_num});
            }
        }
        report(LinuxPipelineStep::InstallBootloader, 92, "Installed bootloader and configured UEFI boot table");
    }
    result.last_completed_step = LinuxPipelineStep::InstallBootloader;

    // 8. UnmountVolumes
    if (opts.dry_run) {
        if (has_efivars) {
            result.executed_commands.push_back("umount -f " + opts.mount_root + "/sys/firmware/efi/efivars");
        }
        result.executed_commands.push_back("umount -f " + opts.mount_root + "/run");
        result.executed_commands.push_back("umount -R -f " + opts.mount_root + "/sys");
        result.executed_commands.push_back("umount -f " + opts.mount_root + "/proc");
        result.executed_commands.push_back("umount -f " + opts.mount_root + "/dev/pts");
        result.executed_commands.push_back("umount -R -f " + opts.mount_root + "/dev");
        result.executed_commands.push_back("umount -f " + opts.mount_root + "/boot/efi");
        result.executed_commands.push_back("umount -f " + opts.mount_root);
        report(LinuxPipelineStep::UnmountVolumes, 98, "Unmounted partitions and pseudo filesystems (dry-run)");
    } else {
        if (has_efivars && fs::exists("/sys/firmware/efi/efivars")) {
            (void)ProcessRunner::run("umount", {"-f", opts.mount_root + "/sys/firmware/efi/efivars"});
        }
        (void)ProcessRunner::run("umount", {"-f", opts.mount_root + "/run"});
        (void)ProcessRunner::run("umount", {"-R", "-f", opts.mount_root + "/sys"});
        (void)ProcessRunner::run("umount", {"-f", opts.mount_root + "/proc"});
        (void)ProcessRunner::run("umount", {"-f", opts.mount_root + "/dev/pts"});
        (void)ProcessRunner::run("umount", {"-R", "-f", opts.mount_root + "/dev"});
        (void)ProcessRunner::run("umount", {"-f", opts.mount_root + "/boot/efi"});
        (void)ProcessRunner::run("umount", {"-f", opts.mount_root});
        report(LinuxPipelineStep::UnmountVolumes, 98, "Unmounted partitions and pseudo filesystems");
    }

    // Dismiss RAII guard now that normal unmount is finished
    unmount_guard.dismiss();
    result.last_completed_step = LinuxPipelineStep::UnmountVolumes;

    // 9. Complete
    result.success = true;
    result.last_completed_step = LinuxPipelineStep::Complete;
    report(LinuxPipelineStep::Complete, 100, "Linux installation pipeline finished successfully");

    return result;
}

} // namespace litewin

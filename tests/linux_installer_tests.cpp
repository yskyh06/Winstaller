#include "platform/linux_installer.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace litewin;

static void test_shell_quote() {
    assert(LinuxInstaller::shell_quote("hello") == "'hello'");
    assert(LinuxInstaller::shell_quote("") == "''");
    assert(LinuxInstaller::shell_quote("p@ss'w0rd") == "'p@ss'\\''w0rd'");
    assert(LinuxInstaller::shell_quote("foo; rm -rf / ;") == "'foo; rm -rf / ;'");
    assert(LinuxInstaller::shell_quote("/mnt/litewin_linux/boot/efi") == "'/mnt/litewin_linux/boot/efi'");
}

static void test_is_valid_unix_identifier() {
    // Valid identifiers
    assert(LinuxInstaller::is_valid_unix_identifier("user"));
    assert(LinuxInstaller::is_valid_unix_identifier("root"));
    assert(LinuxInstaller::is_valid_unix_identifier("alice_99"));
    assert(LinuxInstaller::is_valid_unix_identifier("_daemon"));
    assert(LinuxInstaller::is_valid_unix_identifier("my-host"));
    assert(LinuxInstaller::is_valid_unix_identifier("node.cluster-01"));

    // Invalid identifiers
    assert(!LinuxInstaller::is_valid_unix_identifier("")); // empty
    assert(!LinuxInstaller::is_valid_unix_identifier("-bad")); // starts with -
    assert(!LinuxInstaller::is_valid_unix_identifier(".bad")); // starts with .
    assert(!LinuxInstaller::is_valid_unix_identifier("bad name")); // space
    assert(!LinuxInstaller::is_valid_unix_identifier("user;rm")); // semicolon
    assert(!LinuxInstaller::is_valid_unix_identifier("user$var")); // dollar
    assert(!LinuxInstaller::is_valid_unix_identifier("user'quote")); // single quote
    assert(!LinuxInstaller::is_valid_unix_identifier(std::string(65, 'a'))); // too long (>64)
    assert(LinuxInstaller::is_valid_unix_identifier(std::string(64, 'a'))); // exactly 64
}

static void test_to_string_helpers() {
    assert(to_string(LinuxBootFamily::Debian) == "Debian");
    assert(to_string(LinuxBootFamily::RedHat) == "RedHat");
    assert(to_string(LinuxBootFamily::Arch) == "Arch");

    assert(to_string(LinuxImageFormat::SingleSquashFS) == "SingleSquashFS");
    assert(to_string(LinuxImageFormat::LayeredSquashFS) == "LayeredSquashFS");
    assert(to_string(LinuxImageFormat::Erofs) == "Erofs");

    assert(to_string(LinuxPipelineStep::None) == "None");
    assert(to_string(LinuxPipelineStep::PreCheck) == "PreCheck");
    assert(to_string(LinuxPipelineStep::PartitionDisk) == "PartitionDisk");
    assert(to_string(LinuxPipelineStep::MountVolumes) == "MountVolumes");
    assert(to_string(LinuxPipelineStep::AcquireImage) == "AcquireImage");
    assert(to_string(LinuxPipelineStep::ExtractRootFS) == "ExtractRootFS");
    assert(to_string(LinuxPipelineStep::ConfigureChroot) == "ConfigureChroot");
    assert(to_string(LinuxPipelineStep::InstallBootloader) == "InstallBootloader");
    assert(to_string(LinuxPipelineStep::UnmountVolumes) == "UnmountVolumes");
    assert(to_string(LinuxPipelineStep::Complete) == "Complete");
}

static void test_partition_plan_sata() {
    constexpr std::uint64_t disk_size = 64ULL * 1024 * 1024 * 1024; // 64 GiB
    auto plan = LinuxInstaller::build_partition_plan("/dev/sda", disk_size);

    assert(plan.has_value());
    assert(plan->esp_part == "/dev/sda1");
    assert(plan->root_part == "/dev/sda2");

    const auto& s = plan->sfdisk_script;
    assert(s.find("label: gpt") != std::string::npos);
    assert(s.find("sector-size: 512") != std::string::npos);
    assert(s.find("C12A7328-F81F-11D2-BA4B-00A0C93EC93B") != std::string::npos);
    assert(s.find("0FC63DAF-8483-4772-8E79-3D69D8477DE4") != std::string::npos);
    assert(s.find("start=2048") != std::string::npos);
    assert(s.find("size=1048576") != std::string::npos);
    assert(s.find("start=1050624") != std::string::npos);
}

static void test_partition_plan_nvme() {
    constexpr std::uint64_t disk_size = 128ULL * 1024 * 1024 * 1024; // 128 GiB
    auto plan = LinuxInstaller::build_partition_plan("/dev/nvme0n1", disk_size);

    assert(plan.has_value());
    assert(plan->esp_part == "/dev/nvme0n1p1");
    assert(plan->root_part == "/dev/nvme0n1p2");
}

static void test_partition_plan_other_devices() {
    constexpr std::uint64_t disk_size = 32ULL * 1024 * 1024 * 1024;

    auto plan_mmc = LinuxInstaller::build_partition_plan("/dev/mmcblk0", disk_size);
    assert(plan_mmc.has_value());
    assert(plan_mmc->esp_part == "/dev/mmcblk0p1");
    assert(plan_mmc->root_part == "/dev/mmcblk0p2");

    auto plan_loop = LinuxInstaller::build_partition_plan("/dev/loop2", disk_size);
    assert(plan_loop.has_value());
    assert(plan_loop->esp_part == "/dev/loop2p1");
    assert(plan_loop->root_part == "/dev/loop2p2");

    auto plan_vda = LinuxInstaller::build_partition_plan("/dev/vda", disk_size);
    assert(plan_vda.has_value());
    assert(plan_vda->esp_part == "/dev/vda1");
    assert(plan_vda->root_part == "/dev/vda2");
}

static void test_partition_plan_rejections() {
    assert(!LinuxInstaller::build_partition_plan("", 64ULL * 1024 * 1024 * 1024).has_value());
    assert(!LinuxInstaller::build_partition_plan("/dev/sda", 0).has_value());
    assert(!LinuxInstaller::build_partition_plan("/dev/sda", 512ULL * 1024 * 1024).has_value());
    assert(!LinuxInstaller::build_partition_plan("/dev/sda", 1ULL * 1024 * 1024 * 1024).has_value());
}

static void test_extraction_commands_single_squashfs() {
    std::vector<std::string> layers = {"/iso/casper/filesystem.squashfs"};
    auto cmds = LinuxInstaller::build_extraction_commands(
        LinuxImageFormat::SingleSquashFS, layers, "/mnt/target");

    assert(cmds.size() == 1);
    assert(cmds[0] == "unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/target' '/iso/casper/filesystem.squashfs'");
}

static void test_extraction_commands_nested_liveos() {
    std::vector<std::string> layers = {"/iso/LiveOS/squashfs.img"};
    auto cmds = LinuxInstaller::build_extraction_commands(
        LinuxImageFormat::SingleSquashFS, layers, "/mnt/target");

    assert(cmds.size() == 2);
    assert(cmds[0] == "unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/target' '/iso/LiveOS/squashfs.img'");
    assert(cmds[1].find("LiveOS/rootfs.img") != std::string::npos);
    assert(cmds[1].find("mount -o loop,ro") != std::string::npos);
    assert(cmds[1].find("cp -a") != std::string::npos);
    assert(cmds[1].find("rm -rf") != std::string::npos);
}

static void test_extraction_commands_layered_squashfs() {
    std::vector<std::string> layers = {
        "/iso/casper/minimal.squashfs",
        "/iso/casper/minimal.standard.squashfs"
    };
    auto cmds = LinuxInstaller::build_extraction_commands(
        LinuxImageFormat::LayeredSquashFS, layers, "/mnt/target");

    assert(cmds.size() == 3);
    assert(cmds[0] == "unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/target' '/iso/casper/minimal.squashfs'");
    assert(cmds[1].find("unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/target/.litewin_stage' '/iso/casper/minimal.standard.squashfs'") != std::string::npos);
    assert(cmds[1].find("cp -af '/mnt/target/.litewin_stage'/. '/mnt/target'/") != std::string::npos);
    // Overlay whiteout cleanup
    assert(cmds[2].find("find '/mnt/target' -xdev -type c -size 0 -delete") != std::string::npos);
}

static void test_extraction_commands_erofs() {
    std::vector<std::string> layers = {"/iso/LiveOS/rootfs.erofs"};
    auto cmds = LinuxInstaller::build_extraction_commands(
        LinuxImageFormat::Erofs, layers, "/mnt/target");

    assert(cmds.size() == 1);
    assert(cmds[0] == "fsck.erofs --extract='/mnt/target' '/iso/LiveOS/rootfs.erofs'");
}

static void test_extraction_commands_empty() {
    auto cmds = LinuxInstaller::build_extraction_commands(
        LinuxImageFormat::SingleSquashFS, {}, "/mnt/target");
    assert(cmds.empty());
}

static void test_chroot_commands_common_and_injection_safety() {
    LinuxInstallOptions opts;
    opts.target_disk_path = "/dev/nvme0n1";
    opts.hostname = "my-host";
    opts.username = "alice";
    opts.password = "secret'quoted\"$pass";
    opts.locale = "ko_KR.UTF-8";
    opts.boot_family = LinuxBootFamily::Debian;

    auto cmds = LinuxInstaller::build_chroot_commands(opts);

    // Verify /etc/fstab with UUID resolution
    auto it_fstab_root = std::find_if(cmds.begin(), cmds.end(), [](const std::string& c) {
        return c.find("ROOT_UUID=$(blkid -s UUID -o value '/dev/nvme0n1p2'") != std::string::npos;
    });
    assert(it_fstab_root != cmds.end());

    auto it_fstab_esp = std::find_if(cmds.begin(), cmds.end(), [](const std::string& c) {
        return c.find("ESP_UUID=$(blkid -s UUID -o value '/dev/nvme0n1p1'") != std::string::npos;
    });
    assert(it_fstab_esp != cmds.end());

    // Verify /etc/hostname shell-quoted
    auto it_host = std::find_if(cmds.begin(), cmds.end(), [](const std::string& c) {
        return c.find("echo 'my-host' > /etc/hostname") != std::string::npos;
    });
    assert(it_host != cmds.end());

    // Verify /etc/locale.conf shell-quoted
    auto it_loc = std::find_if(cmds.begin(), cmds.end(), [](const std::string& c) {
        return c.find("echo 'LANG=ko_KR.UTF-8' > /etc/locale.conf") != std::string::npos;
    });
    assert(it_loc != cmds.end());

    // Verify useradd shell-quoted
    auto it_user = std::find_if(cmds.begin(), cmds.end(), [](const std::string& c) {
        return c.find("useradd -m -s /bin/bash 'alice'") != std::string::npos;
    });
    assert(it_user != cmds.end());

    // Verify password safely escaped with shell_quote (no injection)
    auto it_pass = std::find_if(cmds.begin(), cmds.end(), [](const std::string& c) {
        return c.find("chpasswd") != std::string::npos;
    });
    assert(it_pass != cmds.end());
    assert(it_pass->find("usermod -p") != std::string::npos);
    assert(it_pass->find("chpasswd -c SHA512") != std::string::npos);
    assert(it_pass->find("'alice:secret'\\''quoted\"$pass'") != std::string::npos);
}

static void test_chroot_commands_debian() {
    LinuxInstallOptions opts;
    opts.target_disk_path = "/dev/sda";
    opts.username = "bob";
    opts.boot_family = LinuxBootFamily::Debian;

    auto cmds = LinuxInstaller::build_chroot_commands(opts);

    auto has = [&](const std::string& sub) {
        return std::find_if(cmds.begin(), cmds.end(), [&](const std::string& c) {
                   return c.find(sub) != std::string::npos;
               }) != cmds.end();
    };

    // Resilient package purge with || true
    assert(has("apt-get purge -y --ignore-missing casper 'live-boot*' 'calamares*' || true"));
    assert(has("systemd-networkd-wait-online.service"));
    assert(has("usermod -aG sudo 'bob'"));
    assert(has("update-initramfs -c -k all"));
    assert(has("grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=ubuntu --recheck --no-floppy"));
    assert(has("grub-install --target=x86_64-efi --efi-directory=/boot/efi --removable --recheck --no-floppy"));
    assert(has("update-grub"));
}

static void test_chroot_commands_redhat() {
    LinuxInstallOptions opts;
    opts.target_disk_path = "/dev/sda";
    opts.username = "charlie";
    opts.boot_family = LinuxBootFamily::RedHat;

    auto cmds = LinuxInstaller::build_chroot_commands(opts);

    auto has = [&](const std::string& sub) {
        return std::find_if(cmds.begin(), cmds.end(), [&](const std::string& c) {
                   return c.find(sub) != std::string::npos;
               }) != cmds.end();
    };

    // Resilient package purge with || true
    assert(has("dnf remove -y 'anaconda*' livesys-scripts || true"));
    assert(has("usermod -aG wheel 'charlie'"));
    assert(has("touch /.autorelabel"));
    assert(has("dracut --regenerate-all --force"));
    assert(has("grub2-mkconfig -o /boot/grub2/grub.cfg"));
}

static void test_chroot_commands_arch() {
    LinuxInstallOptions opts;
    opts.target_disk_path = "/dev/sda";
    opts.username = "dan";
    opts.boot_family = LinuxBootFamily::Arch;

    auto cmds = LinuxInstaller::build_chroot_commands(opts);

    auto has = [&](const std::string& sub) {
        return std::find_if(cmds.begin(), cmds.end(), [&](const std::string& c) {
                   return c.find(sub) != std::string::npos;
               }) != cmds.end();
    };

    assert(has("pacman-init.service"));
    // Host kernel check before mkinitcpio
    assert(has("if [ ! -f /boot/vmlinuz-linux ] && [ -f /usr/lib/modules/*/vmlinuz ]; then cp -f /usr/lib/modules/*/vmlinuz /boot/vmlinuz-linux; fi"));
    assert(has("usermod -aG wheel 'dan'"));
    assert(has("mkinitcpio -P"));
    assert(has("grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=Arch"));
    assert(has("grub-mkconfig -o /boot/grub/grub.cfg"));
}

static void test_execute_dry_run_debian() {
    LinuxInstallOptions opts;
    opts.target_disk_path = "/dev/nvme0n1";
    opts.target_disk_size = 64ULL * 1024 * 1024 * 1024;
    opts.boot_family = LinuxBootFamily::Debian;
    opts.image_format = LinuxImageFormat::LayeredSquashFS;
    opts.image_layer_paths = {"/iso/minimal.squashfs", "/iso/standard.squashfs"};
    opts.username = "ubuntu";
    opts.dry_run = true;

    std::vector<std::pair<LinuxPipelineStep, int>> progress_events;
    opts.progress_callback = [&](LinuxPipelineStep s, int pct, const std::string&) {
        progress_events.emplace_back(s, pct);
    };

    auto res = LinuxInstaller::execute(opts);

    assert(res.success);
    assert(res.failed_step == LinuxPipelineStep::None);
    assert(res.last_completed_step == LinuxPipelineStep::Complete);
    assert(!res.step_logs.empty());
    assert(!res.executed_commands.empty());

    assert(!progress_events.empty());
    assert(progress_events.back().first == LinuxPipelineStep::Complete);
    assert(progress_events.back().second == 100);

    auto has_cmd = [&](const std::string& sub) {
        return std::find_if(res.executed_commands.begin(), res.executed_commands.end(),
                            [&](const std::string& c) { return c.find(sub) != std::string::npos; }) !=
               res.executed_commands.end();
    };

    assert(has_cmd("sfdisk --force /dev/nvme0n1"));
    assert(has_cmd("mkfs.fat -F32 -n EFI /dev/nvme0n1p1"));
    assert(has_cmd("mkfs.ext4 -F -L ROOT /dev/nvme0n1p2"));
    assert(has_cmd("mount -t ext4 /dev/nvme0n1p2 /mnt/litewin_linux"));
    assert(has_cmd("mount -t vfat /dev/nvme0n1p1 /mnt/litewin_linux/boot/efi"));
    // --rbind & pseudo filesystems
    assert(has_cmd("mount --rbind /dev /mnt/litewin_linux/dev"));
    assert(has_cmd("mount --bind /dev/pts /mnt/litewin_linux/dev/pts"));
    assert(has_cmd("mount --bind /proc /mnt/litewin_linux/proc"));
    assert(has_cmd("mount --rbind /sys /mnt/litewin_linux/sys"));
    assert(has_cmd("mount --bind /run /mnt/litewin_linux/run"));
    assert(has_cmd("mount -t efivarfs efivarfs /mnt/litewin_linux/sys/firmware/efi/efivars"));

    assert(has_cmd("unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/litewin_linux' '/iso/minimal.squashfs'"));
    assert(has_cmd("unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/litewin_linux/.litewin_stage' '/iso/standard.squashfs'"));
    assert(has_cmd("find '/mnt/litewin_linux' -xdev -type c -size 0 -delete"));
    assert(has_cmd("useradd -m -s /bin/bash 'ubuntu'"));
    assert(has_cmd("apt-get purge -y --ignore-missing casper 'live-boot*' 'calamares*' || true"));
    assert(has_cmd("systemd-networkd-wait-online.service"));
    assert(has_cmd("update-initramfs"));
    assert(has_cmd("grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=ubuntu"));
    assert(has_cmd("grub-install --target=x86_64-efi --efi-directory=/boot/efi --removable"));
    assert(has_cmd("update-grub"));
    assert(has_cmd("BOOTX64.EFI"));
    assert(has_cmd("boot_stage.flag"));
    assert(has_cmd("efibootmgr -c -d /dev/nvme0n1 -p 1 -L Ubuntu"));

    // Unmount sequence
    assert(has_cmd("umount -f /mnt/litewin_linux/sys/firmware/efi/efivars"));
    assert(has_cmd("umount -f /mnt/litewin_linux/run"));
    assert(has_cmd("umount -R -f /mnt/litewin_linux/sys"));
    assert(has_cmd("umount -R -f /mnt/litewin_linux/dev"));
    assert(has_cmd("umount -f /mnt/litewin_linux/boot/efi"));
    assert(has_cmd("umount -f /mnt/litewin_linux"));
}

static void test_execute_dry_run_redhat() {
    LinuxInstallOptions opts;
    opts.target_disk_path = "/dev/sda";
    opts.target_disk_size = 64ULL * 1024 * 1024 * 1024;
    opts.boot_family = LinuxBootFamily::RedHat;
    opts.image_format = LinuxImageFormat::SingleSquashFS;
    opts.image_layer_paths = {"/iso/LiveOS/squashfs.img"};
    opts.username = "rocky";
    opts.dry_run = true;

    auto res = LinuxInstaller::execute(opts);

    assert(res.success);
    assert(res.last_completed_step == LinuxPipelineStep::Complete);

    auto has_cmd = [&](const std::string& sub) {
        return std::find_if(res.executed_commands.begin(), res.executed_commands.end(),
                            [&](const std::string& c) { return c.find(sub) != std::string::npos; }) !=
               res.executed_commands.end();
    };

    assert(has_cmd("unsquashfs -f -ignore-errors -no-exit-code -d '/mnt/litewin_linux' '/iso/LiveOS/squashfs.img'"));
    assert(has_cmd("LiveOS/rootfs.img"));
    assert(has_cmd("dnf remove -y 'anaconda*' livesys-scripts || true"));
    assert(has_cmd("usermod -aG wheel 'rocky'"));
    assert(has_cmd("touch /.autorelabel"));
    assert(has_cmd("dracut --regenerate-all --force"));
    assert(has_cmd("grub2-mkconfig -o /boot/grub2/grub.cfg"));
}

static void test_execute_dry_run_arch_with_kernel() {
    LinuxInstallOptions opts;
    opts.target_disk_path = "/dev/sda";
    opts.target_disk_size = 64ULL * 1024 * 1024 * 1024;
    opts.boot_family = LinuxBootFamily::Arch;
    opts.image_format = LinuxImageFormat::SingleSquashFS;
    opts.download_url = "https://geo.mirror.pkgbuild.com/iso/archlinux.iso";
    opts.expected_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    opts.kernel_source_path = "arch/boot/x86_64/vmlinuz-linux";
    opts.username = "archuser";
    opts.dry_run = true;

    auto res = LinuxInstaller::execute(opts);

    assert(res.success);
    assert(res.last_completed_step == LinuxPipelineStep::Complete);

    auto has_cmd = [&](const std::string& sub) {
        return std::find_if(res.executed_commands.begin(), res.executed_commands.end(),
                            [&](const std::string& c) { return c.find(sub) != std::string::npos; }) !=
               res.executed_commands.end();
    };

    assert(has_cmd("curl -f -L -o '/mnt/litewin_linux/.litewin_source.iso'"));
    assert(has_cmd("sha256sum -c"));
    assert(has_cmd("mount -o loop,ro '/mnt/litewin_linux/.litewin_source.iso' '/mnt/litewin_linux_iso'"));
    assert(has_cmd("cp -f '/mnt/litewin_linux_iso/arch/boot/x86_64/vmlinuz-linux' '/mnt/litewin_linux/boot/vmlinuz-linux'"));
    assert(has_cmd("mkinitcpio -P"));
    assert(has_cmd("grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=Arch"));
    assert(has_cmd("umount -f /mnt/litewin_linux_iso"));
}

static void test_execute_precheck_rejections() {
    // 1. Empty disk path
    {
        LinuxInstallOptions opts;
        opts.target_disk_path = "";
        opts.image_layer_paths = {"/iso/rootfs.squashfs"};
        opts.dry_run = true;
        auto res = LinuxInstaller::execute(opts);
        assert(!res.success);
        assert(res.failed_step == LinuxPipelineStep::PreCheck);
    }

    // 2. Empty username
    {
        LinuxInstallOptions opts;
        opts.target_disk_path = "/dev/sda";
        opts.username = "";
        opts.image_layer_paths = {"/iso/rootfs.squashfs"};
        opts.dry_run = true;
        auto res = LinuxInstaller::execute(opts);
        assert(!res.success);
        assert(res.failed_step == LinuxPipelineStep::PreCheck);
    }

    // 3. Shell injection in username
    {
        LinuxInstallOptions opts;
        opts.target_disk_path = "/dev/sda";
        opts.username = "user;rm -rf /";
        opts.image_layer_paths = {"/iso/rootfs.squashfs"};
        opts.dry_run = true;
        auto res = LinuxInstaller::execute(opts);
        assert(!res.success);
        assert(res.failed_step == LinuxPipelineStep::PreCheck);
    }

    // 4. Invalid hostname
    {
        LinuxInstallOptions opts;
        opts.target_disk_path = "/dev/sda";
        opts.hostname = "-bad-host$name";
        opts.image_layer_paths = {"/iso/rootfs.squashfs"};
        opts.dry_run = true;
        auto res = LinuxInstaller::execute(opts);
        assert(!res.success);
        assert(res.failed_step == LinuxPipelineStep::PreCheck);
    }

    // 5. Empty image layers AND empty download_url in dry_run == true MUST reject!
    {
        LinuxInstallOptions opts;
        opts.target_disk_path = "/dev/sda";
        opts.image_layer_paths.clear();
        opts.download_url = "";
        opts.dry_run = true;
        auto res = LinuxInstaller::execute(opts);
        assert(!res.success);
        assert(res.failed_step == LinuxPipelineStep::PreCheck);
    }

    // 6. Empty image layers AND empty download_url in dry_run == false MUST reject!
    {
        LinuxInstallOptions opts;
        opts.target_disk_path = "/dev/sda";
        opts.image_layer_paths.clear();
        opts.download_url = "";
        opts.dry_run = false;
        auto res = LinuxInstaller::execute(opts);
        assert(!res.success);
        assert(res.failed_step == LinuxPipelineStep::PreCheck);
    }

    // 7. Too small disk
    {
        LinuxInstallOptions opts;
        opts.target_disk_path = "/dev/sda";
        opts.target_disk_size = 500ULL * 1024 * 1024;
        opts.image_layer_paths = {"/iso/rootfs.squashfs"};
        opts.dry_run = true;
        auto res = LinuxInstaller::execute(opts);
        assert(!res.success);
        assert(res.failed_step == LinuxPipelineStep::PreCheck);
    }
}

static void test_update_installed_user_credentials() {
    LinuxInstallOptions opts;
    opts.username = "valid_user";
    opts.hostname = "valid-host";
    opts.dry_run = true;
    assert(LinuxInstaller::update_installed_user_credentials(opts));

    LinuxInstallOptions bad_opts = opts;
    bad_opts.username = "bad;user";
    assert(!LinuxInstaller::update_installed_user_credentials(bad_opts));

    LinuxInstallOptions bad_host = opts;
    bad_host.hostname = "bad host";
    assert(!LinuxInstaller::update_installed_user_credentials(bad_host));
}

static void test_grub_config_single_boot() {
    auto default_cfg = LinuxInstaller::build_grub_default_config(false);
    assert(default_cfg.find("GRUB_DEFAULT=0") != std::string::npos);
    assert(default_cfg.find("GRUB_TIMEOUT=0") != std::string::npos);
    assert(default_cfg.find("GRUB_TIMEOUT_STYLE=hidden") != std::string::npos);

    auto custom_cfg = LinuxInstaller::build_grub_custom_entries({});
    assert(custom_cfg.find("exec tail -n +3 $0") != std::string::npos);
    assert(custom_cfg.find("menuentry") == std::string::npos);
}

static void test_grub_config_dual_boot_windows() {
    auto default_cfg = LinuxInstaller::build_grub_default_config(true);
    assert(default_cfg.find("GRUB_DEFAULT=0") != std::string::npos);
    assert(default_cfg.find("GRUB_TIMEOUT=5") != std::string::npos);
    assert(default_cfg.find("GRUB_TIMEOUT_STYLE=menu") != std::string::npos);

    ForeignOsEntry win_entry{
        .type = ForeignOsType::Windows,
        .os_name = "Windows Boot Manager (on /dev/nvme0n1p1)",
        .esp_device_path = "/dev/nvme0n1p1",
        .esp_uuid = "1234-ABCD",
        .efi_loader_path = "/EFI/Microsoft/Boot/bootmgfw.efi"
    };

    auto custom_cfg = LinuxInstaller::build_grub_custom_entries({win_entry});
    assert(custom_cfg.find("menuentry 'Windows Boot Manager (on /dev/nvme0n1p1)' --class windows --class os") != std::string::npos);
    assert(custom_cfg.find("search --no-floppy --fs-uuid --set=root 1234-ABCD") != std::string::npos);
    assert(custom_cfg.find("chainloader /EFI/Microsoft/Boot/bootmgfw.efi") != std::string::npos);
}

static void test_grub_config_dual_boot_linux() {
    ForeignOsEntry lin_entry{
        .type = ForeignOsType::Linux,
        .os_name = "Ubuntu 22.04 LTS (on /dev/sda1)",
        .esp_device_path = "/dev/sda1",
        .esp_uuid = "5678-EF01",
        .efi_loader_path = "/EFI/ubuntu/grubx64.efi"
    };

    auto custom_cfg = LinuxInstaller::build_grub_custom_entries({lin_entry});
    assert(custom_cfg.find("menuentry 'Ubuntu 22.04 LTS (on /dev/sda1)' --class ubuntu --class gnu-linux --class os") != std::string::npos);
    assert(custom_cfg.find("search --no-floppy --fs-uuid --set=root 5678-EF01") != std::string::npos);
    assert(custom_cfg.find("chainloader /EFI/ubuntu/grubx64.efi") != std::string::npos);
}

static void test_grub_forwarding_stub() {
    auto stub = LinuxInstaller::build_forwarding_grub_cfg("abcd-1234-root", "vmlinuz-6.14.0-15-generic", "initrd.img-6.14.0-15-generic");
    assert(stub.find("insmod ext2") != std::string::npos);
    assert(stub.find("search --no-floppy --fs-uuid --set=root abcd-1234-root") != std::string::npos);
    assert(stub.find("set prefix=($root)'/boot/grub'") != std::string::npos);
    assert(stub.find("configfile $prefix/grub.cfg") != std::string::npos);
    assert(stub.find("menuentry 'Ubuntu Linux (Direct Boot)'") != std::string::npos);
    assert(stub.find("linux /boot/vmlinuz-6.14.0-15-generic root=UUID=abcd-1234-root ro boot=local") != std::string::npos);
    assert(stub.find("initrd /boot/initrd.img-6.14.0-15-generic") != std::string::npos);

    ForeignOsEntry win_entry{
        .type = ForeignOsType::Windows,
        .os_name = "Windows Boot Manager (on /dev/nvme0n1p1)",
        .esp_device_path = "/dev/nvme0n1p1",
        .esp_uuid = "1234-ABCD",
        .efi_loader_path = "/EFI/Microsoft/Boot/bootmgfw.efi"
    };
    auto main_cfg = LinuxInstaller::build_main_grub_cfg(
        "abcd-1234-root", true, {win_entry}, "vmlinuz-6.14.0-15-generic", "initrd.img-6.14.0-15-generic"
    );
    assert(main_cfg.find("set timeout=5") != std::string::npos);
    assert(main_cfg.find("menuentry 'Ubuntu'") != std::string::npos);
    assert(main_cfg.find("linux /boot/vmlinuz-6.14.0-15-generic root=UUID=abcd-1234-root ro boot=local") != std::string::npos);
    assert(main_cfg.find("initrd /boot/initrd.img-6.14.0-15-generic") != std::string::npos);
    assert(main_cfg.find("menuentry 'Windows Boot Manager (on /dev/nvme0n1p1)'") != std::string::npos);
}

static void test_execute_dry_run_ubuntu_live_iso_dual_boot() {
    LinuxInstallOptions opts;
    opts.target_disk_path = "/dev/nvme0n1";
    opts.target_disk_size = 128ULL * 1024 * 1024 * 1024;
    opts.boot_family = LinuxBootFamily::Debian;
    opts.image_format = LinuxImageFormat::LayeredSquashFS;
    opts.download_url = "https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso";
    opts.expected_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    opts.username = "ubuntu";
    opts.dry_run = true;

    // Simulate multi-disk foreign Windows OS on /dev/sda1
    ForeignOsEntry win_entry{
        .type = ForeignOsType::Windows,
        .os_name = "Windows Boot Manager (on /dev/sda1)",
        .esp_device_path = "/dev/sda1",
        .esp_uuid = "9999-AAAA",
        .efi_loader_path = "/EFI/Microsoft/Boot/bootmgfw.efi"
    };
    opts.foreign_os_entries = {win_entry};

    auto res = LinuxInstaller::execute(opts);

    assert(res.success);
    assert(res.failed_step == LinuxPipelineStep::None);
    assert(res.last_completed_step == LinuxPipelineStep::Complete);

    auto has_cmd = [&](const std::string& sub) {
        return std::find_if(res.executed_commands.begin(), res.executed_commands.end(),
                            [&](const std::string& c) { return c.find(sub) != std::string::npos; }) !=
               res.executed_commands.end();
    };

    // 1. PartitionDisk
    assert(has_cmd("sfdisk --force /dev/nvme0n1"));
    assert(has_cmd("mkfs.fat -F32 -n EFI /dev/nvme0n1p1"));
    assert(has_cmd("mkfs.ext4 -F -L ROOT /dev/nvme0n1p2"));

    // 2. MountVolumes ONLY physical root & ESP
    assert(has_cmd("mount -t ext4 /dev/nvme0n1p2 /mnt/litewin_linux"));
    assert(has_cmd("mount -t vfat /dev/nvme0n1p1 /mnt/litewin_linux/boot/efi"));

    // 3. AcquireImage
    assert(has_cmd("curl -f -L -o '/mnt/litewin_linux/.litewin_source.iso'"));
    assert(has_cmd("sha256sum -c -"));
    assert(has_cmd("mount -o loop,ro '/mnt/litewin_linux/.litewin_source.iso' '/mnt/litewin_linux_iso'"));

    // 4. ExtractRootFS with -xdev, casper kernel/initrd sync, and immediate ISO unmount/delete
    assert(has_cmd("casper/filesystem.squashfs"));
    assert(has_cmd("find '/mnt/litewin_linux' -xdev -type c -size 0 -delete"));
    assert(has_cmd("cp -f '/mnt/litewin_linux_iso/casper/vmlinuz' '/mnt/litewin_linux/boot/vmlinuz-generic'"));
    assert(has_cmd("cp -f '/mnt/litewin_linux_iso/casper/initrd' '/mnt/litewin_linux/boot/initrd.img-generic'"));
    assert(has_cmd("umount -f /mnt/litewin_linux_iso"));
    assert(has_cmd("rm -f '/mnt/litewin_linux/.litewin_source.iso'"));

    // 5. ConfigureChroot mounts pseudo-fs, writes dual-boot GRUB configs, runs user/initramfs setup
    assert(has_cmd("mount --rbind /dev /mnt/litewin_linux/dev"));
    assert(has_cmd("mount --bind /dev/pts /mnt/litewin_linux/dev/pts"));
    assert(has_cmd("mount --bind /proc /mnt/litewin_linux/proc"));
    assert(has_cmd("mount --rbind /sys /mnt/litewin_linux/sys"));
    assert(has_cmd("mount --bind /run /mnt/litewin_linux/run"));
    assert(has_cmd("GRUB_TIMEOUT=5"));
    assert(has_cmd("GRUB_TIMEOUT_STYLE=menu"));
    assert(has_cmd("Windows Boot Manager (on /dev/sda1)"));
    assert(has_cmd("chmod 755 /mnt/litewin_linux/etc/grub.d/40_custom"));
    assert(has_cmd("useradd -m -s /bin/bash 'ubuntu'"));
    assert(has_cmd("update-initramfs -c -k all"));

    // 6. InstallBootloader
    assert(has_cmd("grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=ubuntu"));
    assert(has_cmd("grub-install --target=x86_64-efi --efi-directory=/boot/efi --removable"));
    assert(has_cmd("BOOTX64.EFI"));
    assert(has_cmd("boot_stage.flag"));
    assert(has_cmd("efibootmgr -c -d /dev/nvme0n1 -p 1 -L Ubuntu"));
    assert(has_cmd("efibootmgr -o 0001,0000,0002"));
    assert(has_cmd("efibootmgr -n 0001"));

    // 7. UnmountVolumes unmounts pseudo filesystems in reverse order
    assert(has_cmd("umount -f /mnt/litewin_linux/run"));
    assert(has_cmd("umount -R -f /mnt/litewin_linux/sys"));
    assert(has_cmd("umount -f /mnt/litewin_linux/proc"));
    assert(has_cmd("umount -f /mnt/litewin_linux/dev/pts"));
    assert(has_cmd("umount -R -f /mnt/litewin_linux/dev"));
    assert(has_cmd("umount -f /mnt/litewin_linux/boot/efi"));
    assert(has_cmd("umount -f /mnt/litewin_linux"));
}

int main() {
    test_shell_quote();
    test_is_valid_unix_identifier();
    test_to_string_helpers();

    test_partition_plan_sata();
    test_partition_plan_nvme();
    test_partition_plan_other_devices();
    test_partition_plan_rejections();

    test_extraction_commands_single_squashfs();
    test_extraction_commands_nested_liveos();
    test_extraction_commands_layered_squashfs();
    test_extraction_commands_erofs();
    test_extraction_commands_empty();

    test_chroot_commands_common_and_injection_safety();
    test_chroot_commands_debian();
    test_chroot_commands_redhat();
    test_chroot_commands_arch();

    test_grub_config_single_boot();
    test_grub_config_dual_boot_windows();
    test_grub_config_dual_boot_linux();
    test_grub_forwarding_stub();

    test_execute_dry_run_debian();
    test_execute_dry_run_redhat();
    test_execute_dry_run_arch_with_kernel();
    test_execute_dry_run_ubuntu_live_iso_dual_boot();
    test_execute_precheck_rejections();

    test_update_installed_user_credentials();

    std::cout << "All updated linux_installer tests passed successfully!\n";
    return 0;
}

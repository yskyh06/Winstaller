#!/usr/bin/env bash
# shellcheck disable=SC2034

iso_name="litewin"
iso_label="LITEWIN_LIVE"
iso_publisher="Litewin Project"
iso_application="Litewin Automated Windows 11 Deployment Environment"
iso_version="2026.11"
install_dir="arch"
buildmodes=('iso')
bootmodes=('uefi.grub')
arch="x86_64"
pacman_conf="pacman.conf"
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '19' '-b' '1M')
bootstrap_tarball_compression=('zstd' '-c' '-T0' '--auto-threads=logical' '--long' '-19')
file_permissions=(
  ["/etc/shadow"]="0:0:400"
  ["/usr/local/bin/litewin"]="0:0:0755"
  ["/etc/systemd/system/litewin.service"]="0:0:0644"
)

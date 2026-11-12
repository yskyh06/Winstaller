#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE_DIR="${PROJECT_ROOT}/packaging/archiso"
OUT_DIR="${LITEWIN_ISO_OUT_DIR:-${PROJECT_ROOT}/build/iso_out}"
ARCH_IMAGE="${LITEWIN_ARCH_IMAGE:-archlinux:latest}"
CACHE_DIR="${PROJECT_ROOT}/build/cache/pacman"
RELEASE_SEQUENCE="${LITEWIN_RELEASE_SEQUENCE:-1}"
if [[ ! "$RELEASE_SEQUENCE" =~ ^[1-9][0-9]{0,8}$ ]]; then
    echo '[ERROR] LITEWIN_RELEASE_SEQUENCE must be a positive integer with at most 9 digits.' >&2
    exit 1
fi
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

mkdir -p "${OUT_DIR}" "${CACHE_DIR}"
OUT_DIR="$(cd "${OUT_DIR}" && pwd)"

echo "[INFO] Starting Docker-based Archiso build (${ARCH_IMAGE})..."

docker run --rm --privileged \
    -e WINSTALLER_RELEASE_SEQUENCE="${RELEASE_SEQUENCE}" \
    -v "${PROJECT_ROOT}:/workspace:ro" \
    -v "${PROFILE_DIR}:/profile:ro" \
    -v "${OUT_DIR}:/out" \
    -v "${CACHE_DIR}:/var/cache/pacman/pkg" \
    "${ARCH_IMAGE}" bash -c "
        set -euo pipefail
        sed -i 's/^#ParallelDownloads = 5/ParallelDownloads = 10/' /etc/pacman.conf
        pacman -Syu --noconfirm archiso mkinitcpio-archiso grub base-devel cmake ninja curl openssl gtest python

        echo '[INFO] Building native Arch Linux litewin binary...'
        cmake -S /workspace -B /tmp/arch-build -GNinja -DCMAKE_BUILD_TYPE=Release
        cmake --build /tmp/arch-build --target litewin winstaller_boot -j\"\$(nproc)\"
        ldd /tmp/arch-build/litewin

        echo '[INFO] Preparing Archiso profile...'
        cp -r /usr/share/archiso/configs/releng /tmp/profile-merged
        rm -f /tmp/profile-merged/efiboot/loader/entries/*.conf
        cp -rf /profile/* /tmp/profile-merged/
        python /workspace/scripts/set_update_sequence.py /tmp/profile-merged/airootfs/etc/winstaller/updates.json
        rm -f /tmp/profile-merged/pacman-size-excludes.conf
        # The installer can configure networking offline; clock sync remains asynchronous.
        rm -f /tmp/profile-merged/airootfs/etc/systemd/system/sysinit.target.wants/systemd-time-wait-sync.service
        # Apply exclusions only to packages extracted into the Live image.
        if ! grep -Eq '^\[options\][[:space:]]*$' /tmp/profile-merged/pacman.conf; then
            echo '[ERROR] Archiso pacman.conf is missing [options].' >&2
            exit 1
        fi
        sed -i '/^\[options\][[:space:]]*$/r /profile/pacman-size-excludes.conf' /tmp/profile-merged/pacman.conf
        cp -f /profile/grub/grub.cfg /tmp/profile-merged/grub/loopback.cfg
        sed -i 's/archisolabel=%ARCHISO_LABEL%/archisolabel=%ARCHISO_LABEL% copytoram=y/g' /tmp/profile-merged/syslinux/*.cfg || true

        mkdir -p /tmp/profile-merged/airootfs/usr/local/bin
        cp -f /tmp/arch-build/litewin /tmp/profile-merged/airootfs/usr/local/bin/litewin
        cp -f /tmp/arch-build/winstaller-boot /tmp/profile-merged/airootfs/usr/local/bin/winstaller-boot
        chmod 0755 /tmp/profile-merged/airootfs/usr/local/bin/litewin /tmp/profile-merged/airootfs/usr/local/bin/winstaller-boot
        declare -A file_permissions
        source /profile/profiledef.sh
        python /workspace/scripts/prepare_boot_progress.py /tmp/profile-merged /usr/lib/initcpio/hooks/archiso \"\$iso_version\"
        pacman -Q archiso mkinitcpio mkinitcpio-archiso > /out/boot-component-versions.txt

        mkdir -p /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants
        sed -i 's/^#ParallelDownloads = 5/ParallelDownloads = 10/' /tmp/profile-merged/pacman.conf || true
        ln -sf ../litewin.service /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants/litewin.service
        ln -sf /usr/lib/systemd/system/systemd-networkd.service /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants/systemd-networkd.service
        ln -sf /usr/lib/systemd/system/NetworkManager.service /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants/NetworkManager.service
        ln -sf /usr/lib/systemd/system/dhcpcd.service /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants/dhcpcd.service
        ln -sf /usr/lib/systemd/system/iwd.service /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants/iwd.service

        mkarchiso -v -w /tmp/archiso-tmp -o /out /tmp/profile-merged
        cd /out && sha256sum litewin-*.iso > SHA256SUMS
        chown -R ${HOST_UID}:${HOST_GID} /out /var/cache/pacman/pkg
    "

echo "[INFO] ISO build completed successfully in ${OUT_DIR}"


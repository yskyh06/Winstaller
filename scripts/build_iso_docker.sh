#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE_DIR="${PROJECT_ROOT}/packaging/archiso"
OUT_DIR="${PROJECT_ROOT}/build/iso_out"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

mkdir -p "${OUT_DIR}"

echo "[INFO] Starting Docker-based Archiso build (archlinux:latest)..."

docker run --rm --privileged \
    -v "${PROJECT_ROOT}:/workspace:ro" \
    -v "${PROFILE_DIR}:/profile:ro" \
    -v "${OUT_DIR}:/out" \
    archlinux:latest bash -c "
        set -euo pipefail
        pacman -Syu --noconfirm archiso grub base-devel cmake ninja curl openssl gtest

        echo '[INFO] Building native Arch Linux litewin binary...'
        cmake -S /workspace -B /tmp/arch-build -GNinja -DCMAKE_BUILD_TYPE=Release
        cmake --build /tmp/arch-build --target litewin -j\"\$(nproc)\"
        ldd /tmp/arch-build/litewin

        echo '[INFO] Preparing Archiso profile...'
        cp -r /usr/share/archiso/configs/releng /tmp/profile-merged
        rm -f /tmp/profile-merged/efiboot/loader/entries/*.conf
        cp -rf /profile/* /tmp/profile-merged/
        cp -f /profile/grub/grub.cfg /tmp/profile-merged/grub/loopback.cfg
        sed -i 's/archisolabel=%ARCHISO_LABEL%/archisolabel=%ARCHISO_LABEL% copytoram=y/g' /tmp/profile-merged/syslinux/*.cfg || true

        mkdir -p /tmp/profile-merged/airootfs/usr/local/bin
        cp -f /tmp/arch-build/litewin /tmp/profile-merged/airootfs/usr/local/bin/litewin
        chmod 0755 /tmp/profile-merged/airootfs/usr/local/bin/litewin

        mkdir -p /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants
        ln -sf ../litewin.service /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants/litewin.service
        ln -sf /usr/lib/systemd/system/systemd-networkd.service /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants/systemd-networkd.service
        ln -sf /usr/lib/systemd/system/iwd.service /tmp/profile-merged/airootfs/etc/systemd/system/multi-user.target.wants/iwd.service

        mkarchiso -v -w /tmp/archiso-tmp -o /out /tmp/profile-merged
        cd /out && sha256sum litewin-*.iso > SHA256SUMS
        chown -R ${HOST_UID}:${HOST_GID} /out
    "

echo "[INFO] ISO build completed successfully in ${OUT_DIR}"


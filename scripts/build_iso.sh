#!/usr/bin/env bash
set -euo pipefail

# Litewin Bootable ISO / Initramfs Packaging Script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
ISO_ROOT="${BUILD_DIR}/iso_root"
OUTPUT_ISO="${BUILD_DIR}/litewin.iso"

echo "=== Packaging Litewin Bootable RAM ISO ==="

# 1. Ensure litewin binary is built
if [[ ! -f "${BUILD_DIR}/litewin" ]]; then
    echo ">> Building litewin binary first..."
    cmake -B "${BUILD_DIR}" -S "${PROJECT_ROOT}"
    cmake --build "${BUILD_DIR}" --target litewin -j"$(nproc)"
fi

# 2. Prepare ISO directory structure
rm -rf "${ISO_ROOT}"
mkdir -p "${ISO_ROOT}/boot/grub"
mkdir -p "${ISO_ROOT}/EFI/BOOT"

# 3. Copy GRUB configuration with boot_stage.flag USB loop guard
cp "${PROJECT_ROOT}/boot/grub/grub.cfg" "${ISO_ROOT}/boot/grub/grub.cfg"

echo ">> ISO staging structure prepared at: ${ISO_ROOT}"
echo ">> Next step: Inject minimal Linux kernel (vmlinuz) + initramfs bundling /usr/bin/litewin"

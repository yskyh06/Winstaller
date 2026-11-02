#!/usr/bin/env bash
set -euo pipefail

# Litewin Local QEMU / KVM Virtual Test Harness
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DISK_IMAGE="${PROJECT_ROOT}/build/virtual_test_disk.qcow2"

echo "=== Litewin QEMU E2E Test Harness ==="

# 1. Create virtual test disk if not already present
if [[ ! -f "${DISK_IMAGE}" ]]; then
    echo ">> Creating 64 GB virtual test disk at ${DISK_IMAGE}..."
    mkdir -p "${PROJECT_ROOT}/build"
    qemu-img create -f qcow2 "${DISK_IMAGE}" 64G
fi

# 2. Locate OVMF UEFI firmware
OVMF_CODE=""
for candidate in \
    "/usr/share/OVMF/OVMF_CODE.fd" \
    "/usr/share/ovmf/x64/OVMF.fd" \
    "/usr/share/qemu/OVMF.fd" \
    "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd"; do
    if [[ -f "${candidate}" ]]; then
        OVMF_CODE="${candidate}"
        break
    fi
done

echo ">> Virtual Disk: ${DISK_IMAGE}"
echo ">> OVMF Firmware: ${OVMF_CODE:-'Not found (defaulting to BIOS/QEMU internal)'}"

echo ""
echo "To test the compiled binary in simulation mode locally:"
echo "  ${PROJECT_ROOT}/build/litewin --dry-run --non-interactive"
echo ""
echo "To launch full QEMU machine once ISO is generated:"
echo "  qemu-system-x86_64 -m 4096 -enable-kvm -bios \"${OVMF_CODE}\" -drive file=\"${DISK_IMAGE}\",format=qcow2 -cdrom litewin.iso -netdev user,id=net0 -device virtio-net-pci,netdev=net0"

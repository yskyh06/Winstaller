#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/build/iso_out"

ISO_FILE="$(find "${OUT_DIR}" -maxdepth 1 -name 'litewin-*.iso' | sort | tail -n 1 || true)"

if [[ -z "${ISO_FILE}" || ! -f "${ISO_FILE}" ]]; then
    echo "[ERROR] No litewin-*.iso found in ${OUT_DIR}" >&2
    exit 1
fi

echo "[INFO] Verifying ISO artifact: ${ISO_FILE}"

if [[ -f "${OUT_DIR}/SHA256SUMS" ]]; then
    (cd "${OUT_DIR}" && sha256sum -c SHA256SUMS)
fi

if command -v isoinfo >/dev/null 2>&1; then
    FILE_LIST="$(isoinfo -R -f -i "${ISO_FILE}" 2>/dev/null || isoinfo -f -i "${ISO_FILE}")"
    for expected in "vmlinuz-linux" "initramfs-linux.img" "airootfs.sfs"; do
        if ! grep -qi "${expected}" <<<"${FILE_LIST}"; then
            echo "[ERROR] Missing required boot component in ISO: ${expected}" >&2
            exit 1
        fi
    done
    echo "[INFO] ISO structure verified via isoinfo (vmlinuz-linux, initramfs-linux.img, airootfs.sfs present)."
else
    echo "[WARN] isoinfo not installed; skipping internal ISO tree inspection."
fi

echo "[OK] Verification passed for ${ISO_FILE}"

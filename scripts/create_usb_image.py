#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid
try:
    from .update_usb_layout import prepare_slots
except ImportError:
    from update_usb_layout import prepare_slots

SECTOR = 512
MIB = 1024 * 1024
BOOT_MIB = 1536
MIN_DATA_MIB = 256
DATA_GUID = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
ESP_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
TOOLS = ["sfdisk", "mkfs.exfat", "mkfs.fat", "mcopy", "7z"]
REQUIRED_FILES = (
    "efi/boot/bootx64.efi",
    "arch/boot/x86_64/vmlinuz-linux",
    "arch/boot/x86_64/initramfs-linux.img",
    "arch/x86_64/airootfs.sfs",
    "boot/grub/grub.cfg",
)


def preflight(iso_path, out_path=None, check_tools=True):
    if not os.path.isfile(iso_path) or os.path.islink(iso_path):
        raise ValueError(f"ISO must be an existing regular file: {iso_path}")
    real_iso = os.path.realpath(iso_path)
    if real_iso == "/dev" or real_iso.startswith("/dev/"):
        raise ValueError(f"ISO path cannot be a device: {iso_path}")
    if not stat.S_ISREG(os.stat(real_iso).st_mode):
        raise ValueError(f"ISO must be a regular file: {iso_path}")
    if out_path is not None:
        if os.path.lexists(out_path):
            raise ValueError(f"Output path already exists: {out_path}")
        abs_out = os.path.abspath(out_path)
        out_dir = os.path.dirname(abs_out)
        if not os.path.isdir(out_dir):
            raise ValueError(f"Output directory does not exist: {out_dir}")
        real_out_dir = os.path.realpath(out_dir)
        if real_out_dir == "/dev" or real_out_dir.startswith("/dev/"):
            raise ValueError(f"Output path resolves under /dev: {out_path}")
        real_out = os.path.realpath(abs_out)
        if real_out == "/dev" or real_out.startswith("/dev/"):
            raise ValueError(f"Output path resolves under /dev: {out_path}")
        if real_iso == real_out:
            raise ValueError("Input ISO and output path resolve to the same location")
    missing = [cmd for cmd in TOOLS if not shutil.which(cmd)] if check_tools else []
    if missing:
        raise RuntimeError(f"Missing required tools: {', '.join(missing)}")


def resolve_ci(root, parts):
    curr = root
    for part in parts:
        matches = [n for n in os.listdir(curr) if n.lower() == part.lower()]
        if not matches:
            return None
        curr = os.path.join(curr, matches[0])
    return curr


def get_layout(size_mib, updatable=False):
    boot_mib=4096 if updatable else BOOT_MIB
    data_mib = size_mib - 1 - boot_mib - 1
    if data_mib < MIN_DATA_MIB:
        raise ValueError(f"Size {size_mib}MiB too small; data partition must be >= {MIN_DATA_MIB}MiB")
    p1_start = 2048
    p1_sectors = data_mib * 2048
    p2_start = p1_start + p1_sectors
    p2_sectors = boot_mib * 2048
    return {
        "layout_version": 2 if updatable else 1,
        "partuuid": str(uuid.uuid4()) if updatable else "",
        "fat_serial": uuid.uuid4().hex[:8] if updatable else "",
        "image_size_mib": size_mib,
        "total_sectors": size_mib * 2048,
        "partitions": [
            {"num": 1, "label": "LITEWINDATA", "fs": "exfat", "start_sector": p1_start,
             "size_sectors": p1_sectors, "size_mib": data_mib, "type_guid": DATA_GUID},
            {"num": 2, "label": "LITEWINBOOT", "fs": "vfat", "start_sector": p2_start,
             "size_sectors": p2_sectors, "size_mib": boot_mib, "type_guid": ESP_GUID},
        ]
    }


def verify_sha256(path, expected):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(MIB), b""):
            h.update(chunk)
    actual = h.hexdigest()
    if actual.lower() != expected.lower():
        raise ValueError(f"SHA256 mismatch: got {actual}, expected {expected}")


def validate_iso_tree(iso_path, boot_mib=BOOT_MIB):
    res = subprocess.run(["7z", "l", "-slt", iso_path], capture_output=True, text=True, check=True)
    cur, total_size = {}, 0
    files_set, dirs_set = set(), set()
    in_entries = False
    for line in res.stdout.splitlines() + [""]:
        sline = line.strip()
        if not in_entries:
            if sline.startswith("----------"):
                in_entries = True
                cur = {}
            continue
        if not sline:
            if cur.get("Path"):
                p = cur["Path"].replace("\\", "/").rstrip("/")
                parts = p.split("/")
                if not p or p.startswith("/") or any(part in ("..", ".", "") for part in parts):
                    raise ValueError(f"Unsafe path in ISO: {p}")
                if ":" in p or any(ord(c) < 32 or ord(c) == 127 for c in p):
                    raise ValueError(f"Unsafe character or prefix in ISO path: {p}")
                if cur.get("Symbolic Link") or cur.get("Hard Link"):
                    raise ValueError(f"Symlinks/hardlinks disallowed in ISO payload: {p}")
                attr = cur.get("Attributes", "")
                if attr.startswith(("L", "l", "c", "C", "b", "B", "p", "P", "s", "S")):
                    raise ValueError(f"Disallowed attributes in ISO payload: {p} ({attr})")
                mode = cur.get("Mode")
                if mode:
                    if mode[0] in "-dlcbps":
                        if mode[0] not in "-d":
                            raise ValueError(f"Disallowed mode in ISO: {p}")
                    else:
                        try:
                            m = int(mode, 8) if not mode.startswith("0x") else int(mode, 16)
                        except ValueError as error:
                            raise ValueError(f"Unrecognized mode in ISO: {p}") from error
                        if stat.S_IFMT(m) not in (0, stat.S_IFREG, stat.S_IFDIR):
                            raise ValueError(f"Disallowed mode in ISO: {p}")
                cf = p.lower()
                is_folder = cur.get("Folder") == "+"
                if cf in files_set or (cf in dirs_set and not is_folder):
                    raise ValueError(f"FAT casefold duplicate or file-dir collision: {p}")
                sub = ""
                for part in [pt.lower() for pt in parts[:-1]]:
                    sub = f"{sub}/{part}" if sub else part
                    if sub in files_set:
                        raise ValueError(f"File-dir collision with file ancestor: {p}")
                    dirs_set.add(sub)
                if is_folder:
                    dirs_set.add(cf)
                else:
                    if cf in dirs_set:
                        raise ValueError(f"File-dir collision: {p}")
                    files_set.add(cf)
                    sz_str = cur.get("Size", "0")
                    sz = int(sz_str) if sz_str else 0
                    if sz >= 4 * 1024 * 1024 * 1024:
                        raise ValueError(f"File exceeds FAT32 4GB limit: {p}")
                    total_size += sz
            cur = {}
        elif " = " in sline:
            k, v = sline.split(" = ", 1)
            cur[k.strip()] = v.strip()
    if total_size > (boot_mib - 64) * MIB:
        raise ValueError(f"ISO content size ({total_size // MIB}MiB) exceeds boot capacity")
    for req in REQUIRED_FILES:
        if req.lower() not in files_set:
            raise ValueError(f"Required file missing from ISO: {req}")


def patch_configs(extract_dir):
    known_dirs = {"boot", "efi", "loader"}
    for root, _, files in os.walk(extract_dir, followlinks=False):
        rel = os.path.relpath(root, extract_dir)
        if rel != ".":
            top = rel.split(os.sep)[0].lower()
            if top not in known_dirs:
                continue
        for f in files:
            if f.lower().endswith((".cfg", ".conf", ".entry")):
                fp = os.path.join(root, f)
                with open(fp, "r", encoding="utf-8") as fh:
                    content = fh.read()
                patched = content.replace("archisolabel=LITEWIN_LIVE", "archisolabel=LITEWINBOOT")
                patched = re.sub(r'(\b(?:search(?:\.fs_label)?|LABEL)\s+[^\n]*\b)LITEWIN_LIVE\b', r'\g<1>LITEWINBOOT', patched)
                patched = re.sub(r'\bLITEWIN_LIVE\b', 'LITEWINBOOT', patched)
                if patched != content:
                    with open(fp, "w", encoding="utf-8") as fh:
                        fh.write(patched)
    grub_cfg = resolve_ci(extract_dir, ("boot", "grub", "grub.cfg"))
    if not grub_cfg or not os.path.isfile(grub_cfg):
        raise ValueError("Required config missing: boot/grub/grub.cfg")
    with open(grub_cfg, "r", encoding="utf-8") as fh:
        if "LITEWIN_LIVE" in fh.read():
            raise ValueError("Unpatched LITEWIN_LIVE label in boot/grub/grub.cfg")


def copy_sparse(src_path, dst_file, offset):
    dst_file.seek(offset)
    with open(src_path, "rb") as sf:
        while True:
            buf = sf.read(4 * MIB)
            if not buf:
                break
            if buf == b"\x00" * len(buf):
                dst_file.seek(len(buf), os.SEEK_CUR)
            else:
                dst_file.write(buf)


def build_image(iso_path, out_path, layout, scratch_dir):
    preflight(iso_path, out_path)
    validate_iso_tree(iso_path,layout["partitions"][1]["size_mib"])
    extract_dir = os.path.join(scratch_dir, "iso_extract")
    os.makedirs(extract_dir, exist_ok=True)
    subprocess.run(["7z", "x", f"-o{extract_dir}", iso_path], capture_output=True, check=True)

    real_extract_dir = os.path.realpath(extract_dir)
    for root, dirs, files in os.walk(extract_dir, followlinks=False):
        for name in dirs + files:
            fp = os.path.join(root, name)
            st = os.lstat(fp)
            if stat.S_ISLNK(st.st_mode):
                raise ValueError(f"Extracted symlink disallowed: {fp}")
            if not (stat.S_ISREG(st.st_mode) or stat.S_ISDIR(st.st_mode)):
                raise ValueError(f"Extracted non-regular object disallowed: {fp}")
            real_fp = os.path.realpath(fp)
            if not (real_fp == real_extract_dir or real_fp.startswith(real_extract_dir + os.sep)):
                raise ValueError(f"Extracted path escapes extraction directory: {fp}")

    for req in REQUIRED_FILES:
        target = resolve_ci(extract_dir, req.split("/"))
        if not target or not os.path.isfile(target) or os.path.islink(target):
            raise ValueError(f"Extracted file missing or not regular: {req}")

    patch_configs(extract_dir)
    if layout.get("layout_version")==2:
        serial=layout["fat_serial"].upper()
        prepare_slots(extract_dir,layout["partuuid"],serial[:4]+"-"+serial[4:])

    p1_info, p2_info = layout["partitions"][0], layout["partitions"][1]
    p1_img = os.path.join(scratch_dir, "p1.img")
    with open(p1_img, "wb") as f:
        f.truncate(p1_info["size_mib"] * MIB)
    subprocess.run(["mkfs.exfat", "-L", p1_info["label"], p1_img], capture_output=True, check=True)

    p2_img = os.path.join(scratch_dir, "p2.img")
    with open(p2_img, "wb") as f:
        f.truncate(p2_info["size_mib"] * MIB)
    fat_args=["mkfs.fat", "-F", "32", "-n", p2_info["label"]]
    if layout.get("fat_serial"):fat_args += ["-i",layout["fat_serial"]]
    subprocess.run(fat_args+[p2_img],capture_output=True,check=True)

    env = os.environ.copy()
    env["MTOOLSRC"] = "/dev/null"
    for item in sorted(os.listdir(extract_dir)):
        src = os.path.join(extract_dir, item)
        subprocess.run(["mcopy", "-i", p2_img, "-s", "-p", "-Q", src, "::"], env=env, capture_output=True, check=True)

    final_tmp = os.path.join(scratch_dir, "output.raw")
    with open(final_tmp, "wb") as f:
        f.truncate(layout["image_size_mib"] * MIB)

    partition_uuid = f", uuid={layout['partuuid']}" if layout.get("partuuid") else ""
    sfdisk_cmd = (
        "label: gpt\n"
        f"1 : start={p1_info['start_sector']}, size={p1_info['size_sectors']}, type={p1_info['type_guid']}, name=\"{p1_info['label']}\"\n"
        f"2 : start={p2_info['start_sector']}, size={p2_info['size_sectors']}, type={p2_info['type_guid']}{partition_uuid}, name=\"{p2_info['label']}\"\n"
    )
    subprocess.run(["sfdisk", final_tmp], input=sfdisk_cmd, text=True, capture_output=True, check=True)

    with open(final_tmp, "r+b") as df:
        copy_sparse(p1_img, df, p1_info["start_sector"] * SECTOR)
        copy_sparse(p2_img, df, p2_info["start_sector"] * SECTOR)

    h = hashlib.sha256()
    with open(final_tmp, "rb") as f:
        for chunk in iter(lambda: f.read(4 * MIB), b""):
            h.update(chunk)
    digest = h.hexdigest()

    try:
        os.link(final_tmp, out_path)
    except FileExistsError:
        raise RuntimeError(f"Output path already exists: {out_path}")

    print(f"Built USB raw image: {out_path} ({layout['image_size_mib']} MiB)")
    print(f"Image SHA-256: {digest}")


def main():
    parser = argparse.ArgumentParser(description="Build safe raw GPT USB image from Arch Linux ISO")
    parser.add_argument("--iso", required=True, help="Path to source ISO")
    parser.add_argument("--output", help="Path to destination raw image file")
    parser.add_argument("--size-mib", type=int, required=True, help="Total image size in MiB")
    parser.add_argument("--updatable",action="store_true",help="Create opt-in v2 A/B update USB (4096 MiB product partition)")
    parser.add_argument("--plan", action="store_true", help="Print partition layout JSON without building")
    parser.add_argument("--iso-sha256", help="Optional expected ISO SHA-256 checksum")
    args = parser.parse_args()

    iso_path = os.path.abspath(args.iso)
    out_path = os.path.abspath(args.output) if args.output else None

    if not args.plan and not out_path:
        sys.exit("Error: --output is required when --plan is not specified")

    try:
        preflight(iso_path, out_path, check_tools=not args.plan)
    except Exception as e:
        sys.exit(f"Error: {e}")

    if args.updatable and not args.plan and not args.iso_sha256:
        sys.exit("Error: --updatable requires --iso-sha256")
    if args.iso_sha256:
        try:
            verify_sha256(iso_path, args.iso_sha256)
        except Exception as e:
            sys.exit(f"Error: {e}")

    try:
        layout = get_layout(args.size_mib,args.updatable)
    except Exception as e:
        sys.exit(f"Error: {e}")

    if args.plan:
        print(json.dumps(layout, indent=2))
        return 0

    out_dir = os.path.dirname(out_path)
    scratch_dir = tempfile.mkdtemp(prefix=".tmp_usb_build_", dir=out_dir)
    try:
        build_image(iso_path, out_path, layout, scratch_dir)
    except subprocess.CalledProcessError as error:
        detail = error.stderr or error.stdout or ""
        if isinstance(detail, bytes):
            detail = detail.decode("utf-8", errors="replace")
        sys.exit(f"Error: {error}\n{detail.strip()}")
    except Exception as e:
        sys.exit(f"Error: {e}")
    finally:
        shutil.rmtree(scratch_dir, ignore_errors=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())

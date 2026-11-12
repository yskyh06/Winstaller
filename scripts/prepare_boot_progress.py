#!/usr/bin/env python3
"""Apply guarded Archiso instrumentation to a generated build profile."""
from pathlib import Path
import argparse
import hashlib

ARCHISO_SHA256 = '6e7263e703bf9bab16cbf054937f1595e70ed760e296ff8c4ef6967311b25623'

def prepare(profile, upstream, version):
    original = upstream.read_text()
    if hashlib.sha256(upstream.read_bytes()).hexdigest() != ARCHISO_SHA256:
        raise ValueError('Archiso RAM-copy hook changed; review compatibility before building')
    start = original.index('        # in case we have pv')
    end = original.index('\n        if [ "$rc" != 0 ]; then', start)
    old = original[start:end]
    replacement = '''        if [ -f /run/winstaller-boot/enabled ]; then
            /usr/local/bin/winstaller-boot --copy "${img}" "/run/archiso/copytoram/${img_fullname}"
            local rc=$?
        else
'''+''.join('    '+line+'\n' for line in old.splitlines())+'''        fi
'''
    modified = original[:start] + replacement + original[end:]
    anchor = '    # We need this block at the top for early failure'
    media_record = r'''    # Preserve the boot device identity before copytoram unmounts the source.
    if [ -f /run/archiso/bootmnt/winstaller/device.json ]; then
        mkdir -p /run/winstaller-media
        local winstaller_dev winstaller_sys
        winstaller_dev="$(realpath "$(resolve_device "$archisodevice")")"
        blkid -s PARTUUID -o value "$winstaller_dev" > /run/winstaller-media/partuuid
        blkid -s UUID -o value "$winstaller_dev" > /run/winstaller-media/fsuuid
        winstaller_sys="$(readlink -f "/sys/class/block/${winstaller_dev##*/}")"
        cat "${winstaller_sys%/*}/diskseq" > /run/winstaller-media/diskseq
        sed -n 's/.*"instance_uuid"[[:space:]]*:[[:space:]]*"\([a-f0-9-]*\)".*/\1/p' /run/archiso/bootmnt/winstaller/device.json > /run/winstaller-media/instance
    fi

'''
    if modified.count(anchor) != 1:
        raise ValueError('Archiso media mount anchor changed')
    modified = modified.replace(anchor, media_record+anchor, 1)
    hook = profile/'airootfs/usr/local/share/winstaller/archiso'
    hook.parent.mkdir(parents=True, exist_ok=True)
    hook.write_text(modified)
    hook.chmod(0o755)
    config = profile/'airootfs/etc/mkinitcpio.conf.d/archiso.conf'
    text = config.read_text()
    if text.count(' archiso ') != 1:
        raise ValueError('Unexpected Archiso HOOKS layout')
    config.write_text(text.replace(' archiso ', ' archiso winstaller_progress ', 1))
    version_path = profile/'airootfs/etc/winstaller-version'
    version_path.write_text(version+'\n')
    for name in ('grub/grub.cfg', 'grub/loopback.cfg'):
        path = profile/name
        path.write_text(path.read_text().replace('@WINSTALLER_VERSION@', version))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('profile', type=Path)
    parser.add_argument('upstream', type=Path)
    parser.add_argument('version')
    args = parser.parse_args()
    prepare(args.profile, args.upstream, args.version)

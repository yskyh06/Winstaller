"""Factory-only v2 layout transformation. Never opens a physical device."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import uuid

COMPONENTS = {
    'kernel': 'arch/boot/x86_64/vmlinuz-linux',
    'initramfs': 'arch/boot/x86_64/initramfs-linux.img',
    'rootfs': 'arch/x86_64/airootfs.sfs',
}

def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()

def prepare_slots(root, partuuid, fsuuid, release='factory'):
    root = Path(root)
    product = root/'winstaller'
    if product.exists():
        raise ValueError('ISO already contains slot metadata; refusing to overwrite')
    config = root/'boot/grub/grub.cfg'
    original = config.read_text()
    anchor = '# Detect installed Linux ROOT partition'
    if anchor not in original:
        raise ValueError('Unsupported GRUB config; refusing to create update USB')
    slot = product/'slots/A'
    components = {}
    for role, relative in COMPONENTS.items():
        source = root/relative
        if not source.is_file() or source.is_symlink():
            raise ValueError('Missing factory component: '+relative)
        destination = slot/relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(source, destination)
        components[role] = {'sha256': digest(destination), 'size_bytes': destination.stat().st_size}
    (slot/'manifest.json').write_text(json.dumps({'slot':'A','release_id':release,'generation':1,'components':components}, sort_keys=True))
    env = b'# GRUB Environment Block\nwinstaller_generation=1\n'
    (slot/'ready.env').write_bytes(env.ljust(1024,b'#'))
    (slot/'ready.sha256').write_text(digest(slot/'ready.env')+'  ready.env\n')
    # The factory release is known-good for initial slot preparation. Runtime writes the same
    # acknowledgement when the first installation screen is reached after an update.
    (slot/'boot-success').write_text(release+'\n1\n')
    (product/'device.json').write_text(json.dumps({'layout_version':2,'instance_uuid':str(uuid.uuid4()),'partuuid':partuuid,'fsuuid':fsuuid,'boot_abi':'winstaller-slots-v1'}, sort_keys=True))
    prelude = '''insmod part_gpt
insmod fat
insmod search_fs_uuid
insmod linux
insmod gzio
insmod all_video
insmod gfxterm
insmod hashsum
insmod gcry_sha256
insmod loadenv
set timeout=5
set timeout_style=menu
set default=winstaller-slot-A
'''
    prelude += f'search --no-floppy --fs-uuid --set=winstaller_boot {fsuuid}\nexport winstaller_boot\n'
    for name in ('A','B'):
        p = '/winstaller/slots/'+name
        prelude += f'''set winstaller_generation=0
set winstaller_{name}=0
if [ -f ($winstaller_boot){p}/ready.sha256 ]; then
    if hashsum --hash sha256 --check ($winstaller_boot){p}/ready.sha256 --prefix ($winstaller_boot){p}; then
        if load_env --file ($winstaller_boot){p}/ready.env winstaller_generation; then
            if [ -f ($winstaller_boot){p}/arch/boot/x86_64/vmlinuz-linux -a -f ($winstaller_boot){p}/arch/boot/x86_64/initramfs-linux.img -a -f ($winstaller_boot){p}/arch/x86_64/airootfs.sfs ]; then
                set winstaller_{name}=$winstaller_generation
            fi
        fi
    fi
fi
'''
    prelude += '''if [ "$winstaller_B" -gt "$winstaller_A" ]; then
    set default=winstaller-slot-B
fi
'''
    prelude += 'set winstaller_label_A="Winstaller - current release"\nset winstaller_label_B="Winstaller - current release"\n'
    prelude += 'if [ "$winstaller_B" -gt "$winstaller_A" ]; then\n set winstaller_label_A="Winstaller - previous release"\nelse\n set winstaller_label_B="Winstaller - previous release"\nfi\n'
    for name in ('A','B'):
        p = '/winstaller/slots/'+name+'/arch'
        prelude += f'''if [ "$winstaller_{name}" -gt 0 ]; then
    menuentry "$winstaller_label_{name}" --id winstaller-slot-{name} {{
        set root=$winstaller_boot
        set gfxpayload=keep
        echo "Starting Winstaller ... Loading the kernel"
        linux {p}/boot/x86_64/vmlinuz-linux archisobasedir={p.lstrip('/')} archisodevice=PARTUUID={partuuid} copytoram=y cow_spacesize=4G winstaller.slot={name} winstaller.progress=1 rd.log=file vt.global_cursor_default=0 quiet loglevel=3 audit=0 udev.log_priority=3
        initrd {p}/boot/x86_64/initramfs-linux.img
    }}
fi
'''
    config.write_text(prelude+'\n'+anchor+original.split(anchor,1)[1])
    # USB boots through the stable GRUB EFI entry; do not retain alternative live entries
    # pointing to the old rootfs path.
    for path in (root/'loader/entries').glob('*.conf'):
        path.unlink()

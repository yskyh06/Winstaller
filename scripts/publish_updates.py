#!/usr/bin/env python3
"""Prepare local GitHub update assets. This tool never publishes or creates keys."""
import argparse
import base64
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import urlsplit

COMPONENTS = {'kernel':'arch/boot/x86_64/vmlinuz-linux',
              'initramfs':'arch/boot/x86_64/initramfs-linux.img',
              'rootfs':'arch/x86_64/airootfs.sfs'}

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def write_new(path, content):
    with path.open('xb') as f:f.write(content)

def parse_json(data):
    def unique(pairs):
        result={}
        for key,value in pairs:
            if key in result:raise ValueError('Duplicate JSON key: '+key)
            result[key]=value
        return result
    return json.loads(data,object_pairs_hook=unique)

def sign_document(source, output, key_path, key_id):
    # The signature covers these exact bytes, including whitespace/newlines.
    data=source.read_bytes()
    if len(data)>1024*1024:raise ValueError('Metadata exceeds 1 MiB')
    doc=parse_json(data)
    if doc.get('document_type') not in ('catalog','release'):raise ValueError('Unknown document type')
    if not re.fullmatch('[a-z0-9_.-]{1,80}',key_id):raise ValueError('Invalid key ID')
    with tempfile.TemporaryDirectory(prefix='winstaller-sign-') as directory:
        sig=Path(directory)/'signature.bin'
        subprocess.run(['openssl','pkeyutl','-sign','-rawin','-inkey',str(key_path),'-in',str(source),'-out',str(sig)],check=True,capture_output=True)
        signature=sig.read_bytes()
    if len(signature)!=64:raise ValueError('Expected an Ed25519 signing key')
    outer={'key_id':key_id,'payload_base64':base64.b64encode(data).decode('ascii'),'signature_base64':base64.b64encode(signature).decode('ascii')}
    write_new(output,(json.dumps(outer,separators=(',',':'))+'\n').encode())

def prepare_release(iso,output,release_id,version,sequence,revision,expires_days,expected_hash):
    if not re.fullmatch('[a-z0-9_.-]{1,80}',release_id) or release_id in ('.','..'):raise ValueError('Invalid release ID')
    if sequence<1 or revision<1 or not 1<=expires_days<=365:raise ValueError('Invalid release counters or expiry')
    if digest(iso).lower()!=expected_hash.lower():raise ValueError('Source ISO checksum mismatch')
    if output.exists():raise ValueError('Output directory already exists')
    output.mkdir(parents=True)
    now=int(datetime.datetime.now(datetime.timezone.utc).timestamp())
    document={'document_type':'release','schema_version':1,'revision':revision,'published_at':now,'expires_at':now+86400*expires_days,
              'release_id':release_id,'release_sequence':sequence,'version':version,'channel':'stable','architecture':'x64',
              'minimum_updater_api':1,'usb_layout_versions':[2],'boot_abi':'winstaller-slots-v1',
              'release_notes':'Replace with reviewed English release notes before signing.','components':[]}
    with tempfile.TemporaryDirectory(prefix='winstaller-release-') as scratch:
        subprocess.run(['7z','x','-y','-o'+scratch,str(iso),*COMPONENTS.values()],check=True,capture_output=True)
        rootfs=Path(scratch)/COMPONENTS['rootfs']
        config=subprocess.run(['unsquashfs','-cat',str(rootfs),'etc/winstaller/updates.json'],check=True,capture_output=True)
        if parse_json(config.stdout).get('release_sequence')!=sequence:
            raise ValueError('ISO release_sequence differs; rebuild with LITEWIN_RELEASE_SEQUENCE set before preparing assets')
        for role,relative in COMPONENTS.items():
            source=Path(scratch)/relative
            size=source.stat().st_size
            if not 0<size<=0xffffffff:raise ValueError('Invalid FAT32 component size')
            name=role+'-'+release_id+source.suffix
            destination=output/name
            with source.open('rb') as src,destination.open('xb') as dst:
                import shutil
                shutil.copyfileobj(src,dst,1024*1024)
            document['components'].append({'role':role,'url':f'https://github.com/yskyh06/Winstaller/releases/download/{release_id}/{name}','size_bytes':size,'sha256':digest(destination)})
    write_new(output/'release-v1.json',(json.dumps(document,indent=2)+'\n').encode())
    write_new(output/'SOURCE_ISO_SHA256',(expected_hash.lower()+'\n').encode())

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='action',required=True)
    sign=sub.add_parser('sign',help='Sign reviewed JSON using an existing Ed25519 PEM key')
    sign.add_argument('--input',type=Path,required=True);sign.add_argument('--output',type=Path,required=True)
    sign.add_argument('--key',type=Path,required=True);sign.add_argument('--key-id',required=True)
    release=sub.add_parser('prepare-release',help='Extract a coherent complete component set from one verified ISO')
    release.add_argument('--iso',type=Path,required=True);release.add_argument('--iso-sha256',required=True)
    release.add_argument('--output',type=Path,required=True);release.add_argument('--release-id',required=True)
    release.add_argument('--version',required=True);release.add_argument('--sequence',type=int,required=True)
    release.add_argument('--revision',type=int,required=True);release.add_argument('--expires-days',type=int,default=30)
    args=parser.parse_args()
    try:
        if args.action=='sign':sign_document(args.input,args.output,args.key,args.key_id)
        else:prepare_release(args.iso,args.output,args.release_id,args.version,args.sequence,args.revision,args.expires_days,args.iso_sha256)
    except (ValueError,OSError,subprocess.CalledProcessError) as error:parser.exit(1,str(error)+'\n')
if __name__=='__main__':main()

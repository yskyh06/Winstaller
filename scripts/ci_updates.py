#!/usr/bin/env python3
"""Manual Actions preparation, role-scoped signing and verified metadata handling."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
try:
    from .publish_updates import parse_json, sign_document, write_new
except ImportError:
    from publish_updates import parse_json, sign_document, write_new

CONFIG = Path('packaging/archiso/airootfs/etc/winstaller/updates.json')
SPKI_PREFIX = bytes.fromhex('302a300506032b6570032100')


def counter(value):
    if isinstance(value, bool) or not re.fullmatch(r'[1-9][0-9]{0,8}', str(value)):
        raise ValueError('Counters must be positive integers of at most 9 digits')
    return int(value)


def version(value):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,39}', value):
        raise ValueError('Version must contain 1-40 ASCII letters, digits, dots, plus, underscore or hyphen')
    return value


def key_for(config, role, key_id):
    matches = [key for key in config['keys'] if key['id'] == key_id and role in key['roles']]
    if len(matches) != 1 or not re.fullmatch('[0-9a-fA-F]{64}', matches[0]['public_hex']):
        raise ValueError('The signing key ID and role must be registered in updates.json')
    return matches[0]


def public_der(private_path):
    result = subprocess.run(['openssl', 'pkey', '-in', str(private_path), '-passin', 'pass:',
                             '-pubout', '-outform', 'DER'], capture_output=True)
    if result.returncode or len(result.stdout) != 44 or not result.stdout.startswith(SPKI_PREFIX):
        raise ValueError('Expected an unencrypted Ed25519 signing key')
    return result.stdout


def validate_signed(path, role, config, validator):
    result = subprocess.run([str(Path(validator).resolve()), str(config), role, str(path)],
                            capture_output=True, text=True)
    if result.returncode:
        raise ValueError('Client signature/schema/expiry validation failed; metadata was not published')


def archive_payload(envelope, config, role):
    """Authenticate old metadata even after expiry, solely for monotonic publication checks."""
    if len(envelope) > 2*1024*1024:
        raise ValueError('Oversized signed metadata')
    outer = parse_json(envelope)
    if set(outer) != {'key_id', 'payload_base64', 'signature_base64'}:
        raise ValueError('Invalid signed metadata envelope')
    body = base64.b64decode(outer['payload_base64'], validate=True)
    signature = base64.b64decode(outer['signature_base64'], validate=True)
    if len(body) > 1024*1024 or len(signature) != 64:
        raise ValueError('Invalid signed metadata size')
    key = key_for(config, role, outer['key_id'])
    with tempfile.TemporaryDirectory(prefix='winstaller-public-verify-') as directory:
        root = Path(directory)
        (root/'key.der').write_bytes(SPKI_PREFIX+bytes.fromhex(key['public_hex']))
        (root/'payload').write_bytes(body)
        (root/'signature').write_bytes(signature)
        result = subprocess.run(['openssl', 'pkeyutl', '-verify', '-pubin', '-keyform', 'DER',
                                 '-inkey', str(root/'key.der'), '-rawin', '-in', str(root/'payload'),
                                 '-sigfile', str(root/'signature')], capture_output=True)
        if result.returncode:
            raise ValueError('Existing metadata has an invalid signature; refusing to replace it')
    document = parse_json(body)
    if document.get('document_type') != role or document.get('schema_version') != 1:
        raise ValueError('Metadata role/schema mismatch')
    counter(document['revision'])
    if role == 'release':
        counter(document['release_sequence'])
    return document


def check_monotonic(candidate, previous, role):
    if previous is None:
        return
    if counter(candidate['revision']) <= counter(previous['revision']):
        raise ValueError('Revision must be higher than the published revision')
    if role == 'release' and (candidate['release_id'] == previous['release_id'] or
                            counter(candidate['release_sequence']) <= counter(previous['release_sequence'])):
        raise ValueError('Use a new release ID and a higher release sequence')


def prepare_catalog(source, output, revision, days, allow_empty=False):
    raw = Path(source).read_bytes()
    if len(raw) > 1024*1024:
        raise ValueError('Catalog source exceeds 1 MiB')
    document = parse_json(raw)
    if not isinstance(document, dict) or not isinstance(document.get('targets'), list):
        raise ValueError('Catalog source must contain a targets array')
    if not document['targets'] and not allow_empty:
        raise ValueError('Add verified targets to catalog/catalog-v1.json, or explicitly allow an empty catalog')
    days = counter(days)
    if days > 365:
        raise ValueError('Expiry must be between 1 and 365 days')
    now = int(time.time())
    document.update(document_type='catalog', schema_version=1, revision=counter(revision),
                    published_at=now, expires_at=now+86400*days)
    document.setdefault('min_catalog_api', 1)
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    write_new(Path(output), (json.dumps(document, indent=2)+'\n').encode())


def sign_ci(source, output, role, key_id, config_path, validator):
    config = parse_json(Path(config_path).read_bytes())
    expected = key_for(config, role, key_id)
    private = os.environ.pop('WINSTALLER_SIGNING_KEY', '')
    if not private:
        raise ValueError('The required WINSTALLER signing Secret is missing')
    # Never place a private key under the checkout or the uploaded artifact directory.
    with tempfile.TemporaryDirectory(prefix='winstaller-sign-', dir=os.environ.get('RUNNER_TEMP')) as directory:
        key_path = Path(directory)/'signing.pem'
        with open(key_path, 'x', opener=lambda p, flags: os.open(p, flags, 0o600)) as handle:
            handle.write(private.replace('\r\n', '\n').strip()+'\n')
        del private
        if public_der(key_path)[len(SPKI_PREFIX):].hex() != expected['public_hex'].lower():
            raise ValueError('The Secret does not match the configured verification public key')
        document = parse_json(Path(source).read_bytes())
        if document.get('document_type') != role:
            raise ValueError('Signing role does not match document_type')
        sign_document(Path(source), Path(output), key_path, key_id)
        try:
            validate_signed(output, role, config_path, validator)
        except Exception:
            Path(output).unlink(missing_ok=True)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    catalog = commands.add_parser('prepare-catalog')
    catalog.add_argument('--source', required=True)
    catalog.add_argument('--output', required=True)
    catalog.add_argument('--revision', required=True)
    catalog.add_argument('--expires-days', default='30')
    catalog.add_argument('--allow-empty', action='store_true')
    sign = commands.add_parser('sign')
    sign.add_argument('--source', required=True)
    sign.add_argument('--output', required=True)
    sign.add_argument('--role', choices=['catalog', 'release'], required=True)
    sign.add_argument('--key-id', required=True)
    sign.add_argument('--config', default=str(CONFIG))
    sign.add_argument('--validator', default='build/validate_update')
    args = parser.parse_args()
    try:
        if args.command == 'prepare-catalog':
            prepare_catalog(args.source, args.output, args.revision, args.expires_days, args.allow_empty)
        else:
            sign_ci(args.source, args.output, args.role, args.key_id, args.config, args.validator)
    except (ValueError, KeyError, TypeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()

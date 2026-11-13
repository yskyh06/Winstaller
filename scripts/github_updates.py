#!/usr/bin/env python3
"""Publish verified manual Actions results. Stable metadata is always the last write."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
from urllib.error import HTTPError
from urllib.parse import quote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
try:
    from .ci_updates import CONFIG, archive_payload, check_monotonic, validate_signed
    from .publish_updates import digest, parse_json
except ImportError:
    from ci_updates import CONFIG, archive_payload, check_monotonic, validate_signed
    from publish_updates import digest, parse_json

REPOSITORY = 'yskyh06/Winstaller'
PATHS = {'catalog': 'catalog-v1.signed.json', 'release': 'updates/stable-v1.signed.json'}


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


class GitHub:
    def __init__(self):
        if os.environ.get('GITHUB_REPOSITORY') != REPOSITORY:
            raise ValueError('Publication is restricted to yskyh06/Winstaller')
        self.token = os.environ.get('GH_TOKEN', '')
        if not self.token:
            raise ValueError('Missing publication token')
        self.opener = build_opener(NoRedirect())

    def request(self, method, path, data=None, raw=False, missing=False):
        body = None if data is None else json.dumps(data).encode()
        request = Request('https://api.github.com/repos/'+REPOSITORY+'/'+path,
                          data=body, method=method, headers={
                              'Authorization': 'Bearer '+self.token,
                              'Accept': 'application/vnd.github.raw+json' if raw else 'application/vnd.github+json',
                              'Content-Type': 'application/json',
                              'X-GitHub-Api-Version': '2022-11-28',
                              'User-Agent': 'Winstaller-manual-publisher'})
        try:
            with self.opener.open(request, timeout=60) as response:
                result = response.read(2*1024*1024+1)
        except HTTPError as error:
            if missing and error.code == 404:
                return None
            raise ValueError(f'GitHub request failed (HTTP {error.code}); no force overwrite was attempted') from None
        if len(result) > 2*1024*1024:
            raise ValueError('GitHub response exceeds metadata limits')
        return result if raw else parse_json(result)

    def head(self):
        result = self.request('GET', 'git/ref/heads/catalog', missing=True)
        return None if result is None else result['object']['sha']

    def read_file(self, path, head):
        if head is None:
            return None
        return self.request('GET', 'contents/'+quote(path, safe='/')+'?ref='+quote(head, safe=''), raw=True, missing=True)

    def write_metadata(self, head, files, message):
        # Parent is the exact revision inspected above; a concurrent writer causes a non-FF failure.
        base = None
        if head:
            base = self.request('GET', 'git/commits/'+head)['tree']['sha']
        tree = []
        for name, body in files.items():
            if name not in ('catalog-v1.json', 'catalog-v1.signed.json', 'updates/stable-v1.signed.json'):
                raise ValueError('Unexpected metadata destination')
            blob = self.request('POST', 'git/blobs', {'content': base64.b64encode(body).decode(), 'encoding': 'base64'})
            tree.append({'path': name, 'mode': '100644', 'type': 'blob', 'sha': blob['sha']})
        spec = {'tree': tree}
        if base:
            spec['base_tree'] = base
        new_tree = self.request('POST', 'git/trees', spec)['sha']
        commit = self.request('POST', 'git/commits', {
            'message': message, 'tree': new_tree, 'parents': [head] if head else [],
            'author': {'name': 'github-actions[bot]', 'email': '41898282+github-actions[bot]@users.noreply.github.com'}})['sha']
        if head:
            self.request('PATCH', 'git/refs/heads/catalog', {'sha': commit, 'force': False})
        else:
            self.request('POST', 'git/refs', {'ref': 'refs/heads/catalog', 'sha': commit})
        return commit


def inspected_candidate(github, signed, role, config_path, validator):
    validate_signed(signed, role, config_path, validator)
    config = parse_json(Path(config_path).read_bytes())
    candidate = archive_payload(Path(signed).read_bytes(), config, role)
    head = github.head()
    old = github.read_file(PATHS[role], head)
    previous = None if old is None else archive_payload(old, config, role)
    check_monotonic(candidate, previous, role)
    return candidate, head


def publish_catalog(github, source, signed, config, validator):
    candidate, head = inspected_candidate(github, signed, 'catalog', config, validator)
    outer = parse_json(Path(signed).read_bytes())
    if base64.b64decode(outer['payload_base64'], validate=True) != Path(source).read_bytes():
        raise ValueError('Catalog source and signed payload differ')
    github.write_metadata(head, {'catalog-v1.json': Path(source).read_bytes(),
                                 PATHS['catalog']: Path(signed).read_bytes()},
                          'Publish verified OS catalog revision '+str(candidate['revision']))


def gh(arguments):
    result = subprocess.run(['gh', *arguments], capture_output=True, text=True)
    if result.returncode:
        raise ValueError('GitHub Release operation failed; any incomplete draft was retained for inspection')


def release_files(directory, iso_directory, document):
    directory, iso_directory = Path(directory), Path(iso_directory)
    images = list(iso_directory.glob('*.iso'))
    if len(images) != 1:
        raise ValueError('Exactly one source ISO is required')
    image = images[0]
    if digest(image) != (directory/'SOURCE_ISO_SHA256').read_text().strip():
        raise ValueError('Source ISO checksum changed after preparation')
    checksum_lines = (iso_directory/'SHA256SUMS').read_text().splitlines()
    if checksum_lines != [digest(image)+'  '+image.name]:
        raise ValueError('ISO checksum file does not match the source ISO')
    files = [image, iso_directory/'SHA256SUMS', directory/'release-v1.json', directory/'release-v1.signed.json']
    for component in document['components']:
        name = urlsplit(component['url']).path.rsplit('/', 1)[-1]
        expected = f'https://github.com/{REPOSITORY}/releases/download/{document["release_id"]}/{name}'
        if component['url'] != expected or not re.fullmatch(r'[A-Za-z0-9_.-]{1,200}', name) or name in ('.', '..'):
            raise ValueError('Release asset URL does not match its release ID')
        path = directory/name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != component['size_bytes'] or digest(path) != component['sha256'].lower():
            raise ValueError('Component size/checksum mismatch')
        files.append(path)
    if len(files) != 7 or len({p.name for p in files}) != 7:
        raise ValueError('A release requires seven distinct ISO, checksum, metadata and component assets')
    for path in files:
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size < 2*1024**3:
            raise ValueError('Release files must be regular files below the GitHub asset size limit')
    return files


def publish_release(github, directory, iso_directory, config, validator, commit):
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('Release source must be a full commit SHA')
    signed = Path(directory)/'release-v1.signed.json'
    document, head = inspected_candidate(github, signed, 'release', config, validator)
    tag = document['release_id']
    if not re.fullmatch('[a-z0-9][a-z0-9_.-]{0,79}', tag):
        raise ValueError('Invalid release tag')
    if parse_json((Path(directory)/'release-v1.json').read_bytes()) != document:
        raise ValueError('Release source and signed payload differ')
    files = release_files(directory, iso_directory, document)
    if github.request('GET', 'releases/tags/'+quote(tag, safe=''), missing=True) is not None or \
       github.request('GET', 'git/ref/tags/'+quote(tag, safe=''), missing=True) is not None:
        raise ValueError('Release/tag already exists; use a new ID rather than overwriting it')
    notes = Path(directory)/'github-release-notes.txt'
    notes.write_text(document['release_notes']+'\n\nSource commit: '+commit+'\n')
    gh(['release', 'create', tag, '--repo', REPOSITORY, '--target', commit, '--draft',
        '--title', 'Winstaller '+document['version'], '--notes-file', str(notes), *[str(p) for p in files]])
    expected = {p.name: (p.stat().st_size, 'sha256:'+digest(p)) for p in files}
    for attempt in range(6):
        remote = github.request('GET', 'releases/tags/'+quote(tag, safe=''))
        assets = {asset['name']: (asset['size'], asset.get('digest')) for asset in remote['assets'] if asset['state'] == 'uploaded'}
        if assets == expected:
            break
        if attempt == 5:
            raise ValueError('Uploaded asset sizes/digests do not match; draft retained and stable metadata unchanged')
        time.sleep(2)
    # Publish only after the complete set has been uploaded and independently checked.
    gh(['release', 'edit', tag, '--repo', REPOSITORY, '--draft=false', '--latest'])
    published = github.request('GET', 'releases/tags/'+quote(tag, safe=''))
    if published['draft'] or published.get('prerelease'):
        raise ValueError('Release is not publicly ready; stable metadata was not changed')
    # The catalog head may have changed while uploading; fail rather than overwriting it.
    if github.head() != head:
        raise ValueError('Catalog branch changed during release upload; stable metadata was not changed')
    github.write_metadata(head, {PATHS['release']: signed.read_bytes()}, 'Publish Winstaller '+tag)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('role', choices=['catalog', 'release'])
    parser.add_argument('--source', default='build/catalog/catalog-v1.json')
    parser.add_argument('--signed', default='build/catalog/catalog-v1.signed.json')
    parser.add_argument('--directory', default='incoming/assets')
    parser.add_argument('--iso-directory', default='incoming/iso')
    parser.add_argument('--config', default=str(CONFIG))
    parser.add_argument('--validator', default='build/validate_update')
    parser.add_argument('--commit')
    args = parser.parse_args()
    try:
        github = GitHub()
        if args.role == 'catalog':
            publish_catalog(github, args.source, args.signed, args.config, args.validator)
        else:
            publish_release(github, args.directory, args.iso_directory, args.config, args.validator, args.commit)
    except (ValueError, KeyError, TypeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()

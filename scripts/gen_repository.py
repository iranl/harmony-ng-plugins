#!/usr/bin/env python3
"""Write repository.json from every <plugin>/manifest.json.

Usage: gen_repository.py <repo root> <archives dir>

<archives dir> holds <id>-plugin.tar.gz for each plugin: the archives just built (local test),
or the assets downloaded from the published releases (release workflow). Each entry gets the
SHA-256 of its archive; the Hub refuses a marketplace download whose SHA-256 differs.
Release tags are <id>-v<version>.

When a release raises min_harmony_version, the entry gets "legacy": the previous entry's
release (version, download_url, sha256, its own min_harmony_version), the last one older Hubs
run. A Hub below the minimum installs that one instead (HarmonyNG 3.x Hubs keep the plugins they
can run once a release needs v4). It is carried forward until the minimum rises again.
"""
import glob
import hashlib
import json
import os
import sys

BASE = 'https://github.com/iranl/harmony-ng-plugins'
LEGACY_KEYS = ('version', 'download_url', 'sha256', 'min_harmony_version')


def version_tuple(v):
    return tuple(int(x) for x in str(v or '').lstrip('v').split('.') if x)


def legacy_release(entry, prev):
    """The "legacy" of a new entry, from the previous entry of the same id (or None)"""
    new_min = version_tuple(entry.get('min_harmony_version'))
    if not prev or not new_min:
        return None
    if prev.get('version') != entry['version'] and version_tuple(prev.get('min_harmony_version')) < new_min:
        return {k: prev[k] for k in LEGACY_KEYS if k in prev}
    old = prev.get('legacy')
    if old and version_tuple(old.get('min_harmony_version')) < new_min:
        return old
    return None


def main():
    root, archives = sys.argv[1], sys.argv[2]
    repo_path = os.path.join(root, 'repository.json')
    repo = json.load(open(repo_path, encoding='utf-8-sig'))
    previous = {p['id']: p for p in repo.get('plugins', []) if isinstance(p, dict) and 'id' in p}
    plugins = []
    for mpath in sorted(glob.glob(os.path.join(root, '*', 'manifest.json'))):
        m = json.load(open(mpath, encoding='utf-8-sig'))
        pid = m['id']
        if os.path.basename(os.path.dirname(mpath)) != pid:
            sys.exit(f'{mpath}: id "{pid}" does not match its directory')
        archive = os.path.join(archives, f'{pid}-plugin.tar.gz')
        if not os.path.isfile(archive):
            sys.exit(f'missing archive {archive}')
        # min_harmony_version too: an older Hub decides before it downloads the archive
        entry = {k: m[k] for k in ('id', 'name', 'version', 'min_harmony_version', 'author', 'description') if k in m}
        entry['homepage'] = f'{BASE}/tree/main/{pid}'
        entry['download_url'] = f'{BASE}/releases/download/{pid}-v{m["version"]}/{pid}-plugin.tar.gz'
        with open(archive, 'rb') as f:
            entry['sha256'] = hashlib.sha256(f.read()).hexdigest()
        legacy = legacy_release(entry, previous.get(pid))
        if legacy:
            entry['legacy'] = legacy
        if 'device_definition' in m:
            entry['device_definition'] = m['device_definition']
        plugins.append(entry)
    repo['plugins'] = plugins
    with open(repo_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(repo, f, indent=2, ensure_ascii=False)
        f.write('\n')
    for p in plugins:
        print(f"[+] {p['id']} {p['version']} sha256 {p['sha256']}")


if __name__ == '__main__':
    main()

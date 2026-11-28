#!/usr/bin/env python3
"""Write repository.json from every <plugin>/manifest.json.

Usage: gen_repository.py <repo root> <archives dir>

<archives dir> holds <id>-plugin.tar.gz for each plugin: the archives just built (local test),
or the assets downloaded from the published releases (release workflow). Each entry gets the
SHA-256 of its archive; the Hub refuses a marketplace download whose SHA-256 differs.
Release tags are <id>-v<version>.

The top level of an entry is the release every Hub can run: Hubs on HarmonyNG 3.0.0 and older
read nothing else. When a release raises min_harmony_version, the top level keeps the previous
release (the last one without that minimum) and the new one goes into "next", a list of release
blocks (version, download_url, sha256, min_harmony_version). A Hub that knows "next" takes the
newest block whose minimum it meets (iranl/harmony-ng#305). Later releases with the same minimum
replace their block; a higher minimum adds one.
"""
import glob
import hashlib
import json
import os
import sys

BASE = 'https://github.com/iranl/harmony-ng-plugins'
RELEASE_KEYS = ('version', 'download_url', 'sha256', 'min_harmony_version')


def version_tuple(v):
    return tuple(int(x) for x in str(v or '').lstrip('v').split('.') if x)


def release_of(entry):
    return {k: entry[k] for k in RELEASE_KEYS if k in entry}


def place_release(entry, prev):
    """entry (the new release at the top level) with the release every Hub runs at the top level
    and the releases that need a newer HarmonyNG in "next", from the previous entry of the id"""
    new_min = version_tuple(entry.get('min_harmony_version'))
    if not prev or not new_min:
        return entry
    top = release_of(prev)
    if version_tuple(top.get('min_harmony_version')) >= new_min:
        return entry
    nexts = prev.get('next') or []
    if isinstance(nexts, dict):
        nexts = [nexts]
    nexts = [n for n in nexts if isinstance(n, dict) and version_tuple(n.get('min_harmony_version')) < new_min]
    # the entry's order, with the top level's release fields; "next" before device_definition
    placed = {k: top[k] if k in RELEASE_KEYS else v for k, v in entry.items() if k not in RELEASE_KEYS or k in top}
    placed.update({k: v for k, v in top.items() if k not in placed})
    placed['next'] = nexts + [release_of(entry)]
    if 'device_definition' in placed:
        placed['device_definition'] = placed.pop('device_definition')
    return placed


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
        if 'device_definition' in m:
            entry['device_definition'] = m['device_definition']
        plugins.append(place_release(entry, previous.get(pid)))
    repo['plugins'] = plugins
    with open(repo_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(repo, f, indent=2, ensure_ascii=False)
        f.write('\n')
    for p in plugins:
        print(f"[+] {p['id']} {p['version']} sha256 {p['sha256']}")


if __name__ == '__main__':
    main()

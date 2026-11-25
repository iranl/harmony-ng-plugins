#!/usr/bin/env python3
"""Device templates (devices/<id>.json): check them and write devices/index.json.

Usage: device_templates.py <repo root> [--write]

Without --write the index must already match the templates (CI). A template is refused when the
Hub would drop or refuse part of it (payload/source/config_sanitize.c of harmony-ng): see
devices/README.md for the format.
"""
import glob
import json
import os
import re
import sys

TRANSPORTS = ('ir', 'bt', 'http', 'tcp', 'mqtt', 'plugin')
METHODS = ('GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'HEAD')
FIELD_TYPES = ('string', 'number', 'password')
MAX_HEADERS = 8
BODY_MAX = 2048
TOKEN = re.compile(r"^[A-Za-z0-9!#$%&'*+.^_`|~-]{1,64}$")
RESERVED_HEADERS = ('host', 'connection', 'content-length', 'transfer-encoding')
PLACEHOLDER = re.compile(r'\{\{([a-zA-Z0-9_]+)\}\}')
ID = re.compile(r'^[a-z0-9][a-z0-9-]{0,62}$')


def strings(o):
    if isinstance(o, str):
        yield o
    elif isinstance(o, dict):
        for v in o.values():
            yield from strings(v)
    elif isinstance(o, list):
        for v in o:
            yield from strings(v)


def check_headers(where, hdrs, errs):
    if not isinstance(hdrs, list):
        errs.append(f'{where}: httpHeaders must be a list')
        return 0
    for h in hdrs:
        n = h.get('name', '') if isinstance(h, dict) else ''
        if not TOKEN.match(n) or n.lower() in RESERVED_HEADERS:
            errs.append(f'{where}: header name "{n}" is not allowed')
        v = h.get('value', '') if isinstance(h, dict) else ''
        if not isinstance(v, str) or len(v) > 1024 or any(ord(c) < 0x20 and c != '\t' for c in v):
            errs.append(f'{where}: header "{n}" has a bad value')
    return len(hdrs)


def check(path, t, plugin_ids):
    errs = []
    tid = t.get('id', '')
    if t.get('schema') != 1:
        errs.append('schema must be 1')
    if not ID.match(tid) or os.path.basename(path) != tid + '.json':
        errs.append(f'id "{tid}" must be lower-case and match the file name')
    for k in ('name', 'manufacturer', 'model'):
        if not isinstance(t.get(k), str) or not t[k].strip():
            errs.append(f'{k} is required')
    trs = t.get('transports')
    if not isinstance(trs, dict) or not trs:
        errs.append('transports must name at least one transport')
        trs = {}
    for k, v in trs.items():
        if k not in TRANSPORTS:
            errs.append(f'unknown transport "{k}"')
        if not isinstance(v, dict):
            errs.append(f'transport "{k}" must be an object')
    if not any(isinstance(v, dict) and v.get('default') for v in trs.values()):
        errs.append('one transport must be "default": true')
    plug = trs.get('plugin')
    if isinstance(plug, dict) and plug.get('id') not in plugin_ids:
        errs.append(f'plugin "{plug.get("id")}" is not in repository.json')
    conn_headers = {}
    for k in ('http', 'tcp'):
        c = trs.get(k, {}).get('connection') if isinstance(trs.get(k), dict) else None
        if k in trs and not isinstance(c, dict):
            errs.append(f'transport "{k}" needs a connection')
        elif c is not None:
            if 'netHost' not in c or 'netPort' not in c:
                errs.append(f'{k} connection needs netHost and netPort')
            conn_headers[k] = check_headers(f'{k} connection', c.get('httpHeaders', []), errs)

    setup = t.get('setup', {})
    fields = setup.get('fields', []) if isinstance(setup, dict) else []
    keys = set()
    for f in fields:
        if not isinstance(f, dict) or not re.match(r'^[a-zA-Z0-9_]+$', f.get('key', '')) or not f.get('label'):
            errs.append(f'setup field {f!r} needs a key and a label')
            continue
        if f.get('type', 'string') not in FIELD_TYPES:
            errs.append(f'setup field "{f["key"]}": type must be one of {FIELD_TYPES}')
        keys.add(f['key'])
    for s in strings({'transports': trs, 'commands': t.get('commands', [])}):
        for k in PLACEHOLDER.findall(s):
            if k not in keys:
                errs.append(f'placeholder {{{{{k}}}}} has no setup field')

    seen = set()
    cmds = t.get('commands', [])
    if not isinstance(cmds, list):
        errs.append('commands must be a list')
        cmds = []
    for c in cmds:
        n, tr = c.get('name', ''), c.get('transport', '')
        where = f'command "{n}" ({tr})'
        if not n or (n, tr) in seen:
            errs.append(f'{where}: a name is required and unique per transport')
        seen.add((n, tr))
        if tr not in trs:
            errs.append(f'{where}: transport not in the template\'s transports')
        if tr == 'http':
            if c.get('httpMethod', 'GET') not in METHODS:
                errs.append(f'{where}: method {c.get("httpMethod")}')
            p = c.get('httpPath', '/')
            if not p.startswith('/') or ' ' in p or len(p) > 1024:
                errs.append(f'{where}: httpPath must start with / and have no spaces')
            if len(c.get('httpBody', '')) > BODY_MAX:
                errs.append(f'{where}: body over {BODY_MAX} bytes')
            n_h = check_headers(where, c.get('httpHeaders', []), errs)
            dev_h = {h['name'].lower() for h in trs.get('http', {}).get('connection', {}).get('httpHeaders', [])}
            own_h = {h.get('name', '').lower() for h in c.get('httpHeaders', [])}
            if n_h + len(dev_h - own_h) > MAX_HEADERS:
                errs.append(f'{where}: more than {MAX_HEADERS} headers with the connection\'s')
        elif tr == 'tcp':
            pl = c.get('tcpPayload', '')
            if c.get('tcpFormat', 'text') == 'hex':
                digits = re.sub(r'[ :]', '', pl)
                if not digits or len(digits) % 2 or not re.match(r'^[0-9A-Fa-f]+$', digits):
                    errs.append(f'{where}: invalid hex payload')
            elif not pl or len(pl) > BODY_MAX:
                errs.append(f'{where}: tcpPayload required, at most {BODY_MAX} bytes')
        elif tr == 'plugin' and not str(c.get('keyCode', '')).startswith('PLUGIN:'):
            errs.append(f'{where}: keyCode must be PLUGIN:<id>:<command>')
    return errs


def index_entry(t):
    trs = t['transports']
    e = {'id': t['id'], 'name': t['name'], 'manufacturer': t['manufacturer'], 'model': t['model'],
         'category': t.get('category', 'generic'), 'description': t.get('description', ''),
         'transports': list(trs), 'file': f'devices/{t["id"]}.json'}
    if isinstance(trs.get('plugin'), dict):
        e['plugin'] = trs['plugin']['id']
    return e


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    write = '--write' in sys.argv[2:]
    repo = json.load(open(os.path.join(root, 'repository.json'), encoding='utf-8-sig'))
    plugin_ids = {p['id'] for p in repo.get('plugins', [])}
    entries, failed = [], False
    for path in sorted(glob.glob(os.path.join(root, 'devices', '*.json'))):
        if os.path.basename(path) == 'index.json':
            continue
        t = json.load(open(path, encoding='utf-8'))
        errs = check(path, t, plugin_ids)
        for e in errs:
            print(f'{path}: {e}')
        failed |= bool(errs)
        if not errs:
            entries.append(index_entry(t))
    if failed:
        sys.exit(1)
    index = {'schema': 1, 'templates': entries}
    text = json.dumps(index, indent=2, ensure_ascii=False) + '\n'
    ipath = os.path.join(root, 'devices', 'index.json')
    if write:
        open(ipath, 'w', encoding='utf-8', newline='\n').write(text)
        print(f'wrote {ipath} ({len(entries)} templates)')
    elif not os.path.isfile(ipath) or open(ipath, encoding='utf-8').read() != text:
        sys.exit('devices/index.json is out of date: run scripts/device_templates.py . --write')
    else:
        print(f'{len(entries)} device templates ok')


if __name__ == '__main__':
    main()

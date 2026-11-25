#!/usr/bin/env python3
"""device_templates.py refuses what the Hub would drop: run from run_host_tests.sh."""
import copy
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import device_templates as dt  # noqa: E402

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
path = os.path.join(root, 'devices', 'denon-avr.json')
good = json.load(open(path, encoding='utf-8'))
assert dt.check(path, good, {'sony-bravia'}) == [], 'the shipped template passes'

bad_cases = {
    'unknown transport': lambda t: t['transports'].update({'fax': {}}),
    'no default transport': lambda t: [v.pop('default', None) for v in t['transports'].values()],
    'placeholder without field': lambda t: t['transports']['tcp']['connection'].update({'netHost': '{{ip}}'}),
    'bad hex': lambda t: t['commands'][0].update({'tcpFormat': 'hex', 'tcpPayload': '0G'}),
    'path without slash': lambda t: t['commands'].append({'name': 'X', 'transport': 'http', 'httpPath': 'x'}),
    'reserved header': lambda t: t['commands'].append({'name': 'Y', 'transport': 'http', 'httpPath': '/',
                                                        'httpHeaders': [{'name': 'Host', 'value': 'a'}]}),
    'body too long': lambda t: t['commands'].append({'name': 'Z', 'transport': 'http', 'httpPath': '/',
                                                      'httpBody': 'x' * 2049}),
    'command transport not listed': lambda t: t['commands'].append({'name': 'W', 'transport': 'mqtt'}),
    'unknown plugin': lambda t: t['transports'].update({'plugin': {'id': 'nope'}}),
    'duplicate name and transport': lambda t: t['commands'].append(dict(t['commands'][0])),
}
for what, mutate in bad_cases.items():
    t = copy.deepcopy(good)
    mutate(t)
    assert dt.check(path, t, {'sony-bravia'}), 'not refused: ' + what
print('device template checks ok (%d refusals)' % len(bad_cases))

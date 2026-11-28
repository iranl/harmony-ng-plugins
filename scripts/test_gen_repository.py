#!/usr/bin/env python3
"""gen_repository.py keeps the release every Hub runs at the top level and the releases that need
a newer HarmonyNG in "next": run from run_host_tests.sh."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_repository as gr  # noqa: E402

v3 = {'id': 'p', 'name': 'P', 'version': '1.2.0', 'download_url': 'u120', 'sha256': 'a' * 64, 'device_definition': {}}
v4 = {'id': 'p', 'name': 'P', 'version': '1.3.0', 'min_harmony_version': '4.0.0', 'download_url': 'u130',
      'sha256': 'b' * 64, 'device_definition': {}}
rel4 = gr.release_of(v4)

# the minimum rises: the previous release stays at the top level, the new one goes into next
e = gr.place_release(dict(v4), v3)
assert gr.release_of(e) == gr.release_of(v3) and e['next'] == [rel4], e
assert list(e)[-1] == 'device_definition' and e['name'] == 'P', list(e)
# regenerated unchanged, and a later release with the same minimum replaces its block
assert gr.place_release(dict(v4), e) == e
e2 = gr.place_release(dict(v4, version='1.4.0', download_url='u140'), e)
assert gr.release_of(e2) == gr.release_of(v3) and [n['version'] for n in e2['next']] == ['1.4.0'], e2
# the minimum rises again: the block of the previous minimum stays
e5 = gr.place_release(dict(v4, version='2.0.0', min_harmony_version='5.0.0'), e2)
assert [n['version'] for n in e5['next']] == ['1.4.0', '2.0.0'] and e5['version'] == '1.2.0', e5
# no minimum, or the same minimum as the top level: a plain entry
assert 'next' not in gr.place_release(dict(v3, version='1.2.1'), v3)
assert 'next' not in gr.place_release(dict(v4, version='1.3.1'), v4)
assert 'next' not in gr.place_release(dict(v4), None)
print('[+] gen_repository top level and next releases: all checks passed')

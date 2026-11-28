#!/usr/bin/env python3
"""gen_repository.py keeps the last release for older Hubs ("legacy"): run from run_host_tests.sh."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_repository as gr  # noqa: E402

old = {'id': 'p', 'version': '1.2.0', 'download_url': 'u120', 'sha256': 'a' * 64}
new = {'id': 'p', 'version': '1.3.0', 'min_harmony_version': '4.0.0', 'download_url': 'u130', 'sha256': 'b' * 64}

# the minimum rises: the previous release becomes the legacy one
leg = gr.legacy_release(dict(new), old)
assert leg == {'version': '1.2.0', 'download_url': 'u120', 'sha256': 'a' * 64}, leg
# carried forward: the same version regenerated, and a later release with the same minimum
prev = dict(new, legacy=leg)
assert gr.legacy_release(dict(new), prev) == leg
assert gr.legacy_release(dict(new, version='1.4.0'), prev) == leg
# a release that keeps the old minimum, or no minimum at all: no legacy
assert gr.legacy_release(dict(old, version='1.2.1'), old) is None
assert gr.legacy_release(dict(new), None) is None
# the minimum rises again: the last release of the previous minimum
leg5 = gr.legacy_release(dict(new, version='2.0.0', min_harmony_version='5.0.0'), prev)
assert leg5['version'] == '1.3.0' and leg5['min_harmony_version'] == '4.0.0', leg5
print('[+] gen_repository legacy release: all checks passed')

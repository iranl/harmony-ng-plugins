#!/usr/bin/env python3
"""Host test of the wol plugin: magic packets for the device command and inline MACs, one-shot
on a held button, and the Find MAC status. Usage: test_wol.py <wol binary> <work dir>"""
import json, os, socket, subprocess, sys, time

BIN, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)
SOCK = os.path.join(WORK, 'wol.sock')
STATUS = '/tmp/plugins/wol.status'
PORT = 40009
fails = 0


def check(name, cond):
    global fails
    print(('  PASS ' if cond else '  FAIL ') + name)
    fails += 0 if cond else 1


def send(msg):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.sendto(json.dumps(msg).encode(), SOCK)
    s.close()


def packets(timeout=0.6):
    got, end = [], time.time() + timeout
    while time.time() < end:
        try:
            got.append(udp.recv(200))
        except socket.timeout:
            pass
    return got


def magic(mac):
    return b'\xff' * 6 + bytes.fromhex(mac.replace(':', '').replace('-', '')) * 16


def status(timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(STATUS) as f:
                return json.load(f)
        except (OSError, ValueError):
            time.sleep(0.1)
    return None


udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind(('127.0.0.1', PORT))
udp.settimeout(0.1)

env = dict(os.environ, HARMONY_PLUGIN_DATA=os.path.join(WORK, 'data'))
os.makedirs(env['HARMONY_PLUGIN_DATA'], exist_ok=True)
log = open(os.path.join(WORK, 'plugin.log'), 'w')
proc = subprocess.Popen([BIN, SOCK], env=env, stdout=log, stderr=subprocess.STDOUT)
for _ in range(50):
    if os.path.exists(SOCK):
        break
    time.sleep(0.05)

cfg = {'mac': 'aa-bb-cc-dd-ee-01', 'broadcast': '127.0.0.1', 'port': PORT}
send({'plugin_id': 'wol', 'command': 'PowerOn', 'action': 'tap', 'config': cfg})
p = packets()
check('PowerOn sends the magic packet (3 times)', len(p) == 3 and all(x == magic('aabbccddee01') for x in p))

for a in ('press', 'repeat', 'repeat', 'release'):
    send({'plugin_id': 'wol', 'command': 'PowerOn', 'action': a, 'config': cfg})
p = packets()
check('a held button wakes once', len(p) == 3)

send({'plugin_id': 'wol', 'command': 'PowerOn', 'action': 'tap', 'config': {'mac': 'nonsense'}})
check('a bad MAC sends nothing', packets(0.3) == [])

# inline: any device's command with keyCode PLUGIN:wol:<MAC>@<broadcast> (port 9: not caught
# here, so the inline form is checked by the log)
send({'plugin_id': 'wol', 'command': '11:22:33:44:55:66@127.0.0.1', 'action': 'tap'})
time.sleep(0.3)
log.flush()
check('inline MAC is woken', 'Wake-on-LAN sent to 11:22:33:44:55:66' in open(os.path.join(WORK, 'plugin.log')).read())

try:
    os.unlink(STATUS)
except OSError:
    pass
send({'plugin_id': 'wol', 'command': 'pair', 'action': 'start_pair', 'host': 'not/an address'})
st = status()
check('Find MAC refuses a bad address', st and st['ok'] is False and st['stage'] == 'error')

# a host name is resolved by the Hub (here from the hosts file); the messages name it as typed
os.unlink(STATUS)
send({'plugin_id': 'wol', 'command': 'pair', 'action': 'start_pair', 'host': 'localhost'})
st = status()
check('Find MAC takes a host name (localhost): no ARP entry, ends with an error naming it',
      st and st['ok'] is False and st['message'].startswith('No answer from localhost'))

os.unlink(STATUS)
send({'plugin_id': 'wol', 'command': 'pair', 'action': 'start_pair', 'host': '127.0.0.1'})
st = status()
# loopback has no ARP entry: the Find ends with an error after its 3 s
check('Find MAC without an answer ends with an error', st and st['ok'] is False and '127.0.0.1' in st['message'])

proc.terminate()
check('stops on SIGTERM', proc.wait(timeout=3) == 0)
print('[+] wol: all checks passed' if not fails else '[-] wol: %d failed' % fails)
sys.exit(1 if fails else 0)

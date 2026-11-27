#!/usr/bin/env python3
"""sony-bravia against a fake Bravia (HTTP on 127.0.0.1:18080, X-Auth-PSK). The plugin is built
with -DBRAVIA_PORT=18080. Usage: test_fake_bravia.py <bin> <work>"""
import http.server, json, os, queue, re, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts'))
from fake_ws import Plugin, check, failures, wait_for  # noqa: E402

BIN, WORK = sys.argv[1], sys.argv[2]
PSK = '0000'
# IRCC codes as the published Bravia code lists (getRemoteControllerInfo answers) give them
KNOWN = {'Home': 'AAAAAQAAAAEAAABgAw==', 'Back': 'AAAAAgAAAJcAAAAjAw==', 'Up': 'AAAAAQAAAAEAAAB0Aw==',
         'Select': 'AAAAAQAAAAEAAABlAw==', 'VolumeUp': 'AAAAAQAAAAEAAAASAw==', 'Netflix': 'AAAAAgAAABoAAAB8Aw==',
         'Guide': 'AAAAAgAAAKQAAABbAw==', 'ActionMenu': 'AAAAAgAAAMQAAABLAw==', 'Blue': 'AAAAAgAAAJcAAAAkAw==',
         '0': 'AAAAAQAAAAEAAAAJAw==', 'Pause': 'AAAAAgAAAJcAAAAZAw=='}


class TV:
    power = 'active'
    ircc = queue.Queue()
    rpc = queue.Queue()

    @staticmethod
    def get(q, timeout=2.0):
        try:
            return q.get(timeout=timeout)
        except queue.Empty:
            return None


class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        if self.headers.get('X-Auth-PSK') != PSK:
            self.send_response(403)
            self.end_headers()
            return
        out = b''
        if self.path == '/sony/IRCC':
            TV.ircc.put(re.search('<IRCCCode>(.*)</IRCCCode>', body).group(1))
        else:
            m = json.loads(body)
            if m['method'] == 'getPowerStatus':
                out = json.dumps({'result': [{'status': TV.power}], 'id': 1}).encode()
            else:
                TV.rpc.put((self.path, m['method'], m['params'][0] if m['params'] else None))
                if m['method'] == 'setPowerStatus':
                    TV.power = 'active' if m['params'][0]['status'] else 'standby'
                out = b'{"result":[],"id":1}'
        self.send_response(200)
        self.send_header('Content-Length', str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *a):
        pass


srv = http.server.ThreadingHTTPServer(('127.0.0.1', 18080), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
p = Plugin(BIN, WORK, 'sony-bravia')
cfg = {'host': '127.0.0.1', 'psk': PSK, 'mac': '00:11:22:33:44:66'}

try:
    for name, code in KNOWN.items():
        p.send(name, config=cfg)
        got = TV.get(TV.ircc)
        check('%s -> IRCC %s' % (name, code), got == code)

    for a in ('press', 'repeat', 'repeat', 'release'):
        p.send('VolumeDown', a, config=cfg)
    check('held VolumeDown -> sent on press and each repeat', [TV.get(TV.ircc) for _ in range(3)] == ['AAAAAQAAAAEAAAATAw=='] * 3
          and TV.get(TV.ircc, 0.4) is None)
    for a in ('press', 'repeat', 'release'):
        p.send('Home', a, config=cfg)
    check('held Home -> once', TV.get(TV.ircc) == KNOWN['Home'] and TV.get(TV.ircc, 0.4) is None)

    TV.power = 'active'
    p.send('PowerOn', config=cfg)
    check('PowerOn while on: nothing', TV.get(TV.rpc, 0.6) is None)
    p.send('PowerOff', config=cfg)
    check('PowerOff -> setPowerStatus false', TV.get(TV.rpc) == ('/sony/system', 'setPowerStatus', {'status': False}))
    p.send('PowerToggle', config=cfg)
    check('PowerToggle in standby -> setPowerStatus true', TV.get(TV.rpc) == ('/sony/system', 'setPowerStatus', {'status': True}))
    p.send('HDMI 3', config=cfg)
    check('HDMI 3 -> setPlayContent extInput:hdmi?port=3',
          TV.get(TV.rpc) == ('/sony/avContent', 'setPlayContent', {'uri': 'extInput:hdmi?port=3'}))
    p.send('Volume', value=15, config=cfg)
    check('Volume 15 -> setAudioVolume', TV.get(TV.rpc) == ('/sony/audio', 'setAudioVolume', {'target': 'speaker', 'volume': '15'}))
    p.send('text:hello', config=cfg)
    check('text -> setTextForm', TV.get(TV.rpc) == ('/sony/appControl', 'setTextForm', {'text': 'hello'}))

    # the address may be a host name the Hub resolves (hosts file, DNS), not only an IPv4 address
    p.send('Home', config=dict(cfg, host='localhost'))
    check('host name (localhost) -> IRCC sent', TV.get(TV.ircc) == KNOWN['Home'])
    p.send('Home', config=dict(cfg, host='bad/name'))
    check('a host name with other characters is refused, nothing sent', TV.get(TV.ircc, 0.6) is None)

    p.send('Home', config=dict(cfg, psk='wrong'))
    check('wrong PSK: refusal logged', wait_for(lambda: p.logged('refused the pre-shared key')))

    p.send('PowerOn', config=dict(cfg, host='127.0.0.2'))
    check('PowerOn on a TV off the network -> Wake-on-LAN', wait_for(lambda: p.logged('Wake-on-LAN sent to 00:11:22:33:44:66'), 5))
finally:
    rc = p.stop()
check('stops on SIGTERM', rc == 0)
print('sony-bravia fake TV: %s' % ('all passed' if not failures else '%d FAILED' % len(failures)))
sys.exit(1 if failures else 0)

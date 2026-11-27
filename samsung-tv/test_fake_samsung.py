#!/usr/bin/env python3
"""samsung-tv against a fake Samsung TV: wss on 127.0.0.1:8002 (Allow prompt, token), REST on
8001 (PowerState). Usage: test_fake_samsung.py <samsung_tv binary> <work dir>"""
import base64, http.server, json, os, queue, sys, threading, time
from urllib.parse import quote
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts'))
from fake_ws import FakeWsServer, Plugin, check, failures, wait_for  # noqa: E402

BIN, WORK = sys.argv[1], sys.argv[2]
TOKEN = '11223344'


class TV:
    mode = 'prompt'      # prompt: wait for allow(); deny; auto: connect at once
    power = 'on'
    model = '19_FAKE_QTV'   # the info page's "model": year prefix (2019)
    msgs = queue.Queue()
    pending = []

    @classmethod
    def next(cls, timeout=2.0):
        try:
            return cls.msgs.get(timeout=timeout)
        except queue.Empty:
            return None


# As the real TV: the connect event lists the clients it knows, each with its own token, before
# the token it issues to this connection (data.token, only when it issues one)
OTHER = {'attributes': {'name': 'W0hvbWUgQXNzaXN0YW50XSBUZXN0', 'token': '99999999'},
         'deviceName': 'W0hvbWUgQXNzaXN0YW50XSBUZXN0', 'id': 'other', 'isHost': False}


def connect_event(issue):
    me = {'attributes': {'name': 'SGFybW9ueSBIdWI=', 'token': TOKEN}, 'deviceName': 'SGFybW9ueSBIdWI=', 'id': 'x'}
    data = {'clients': [OTHER, me], 'id': 'x'}
    if issue:
        data['token'] = TOKEN
    return {'event': 'ms.channel.connect', 'data': data}


def on_open(c):
    if 'token=' + TOKEN in c.path:
        c.send(connect_event(False))
    elif TV.mode == 'auto':
        c.send(connect_event(True))
    elif TV.mode == 'deny':
        c.send({'event': 'ms.channel.unauthorized'})
    else:
        TV.pending.append(c)


def allow():
    for c in TV.pending:
        c.send(connect_event(True))
    TV.pending.clear()


def on_text(c, text):
    TV.msgs.put(json.loads(text))


class Rest(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps({'device': {'PowerState': TV.power, 'name': 'Fake', 'model': TV.model,
                                      'modelName': 'QE65Q90RATXZT'}}).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


srv = FakeWsServer(8002, WORK, tls=True, on_open=on_open, on_text=on_text)
rest = http.server.HTTPServer(('127.0.0.1', 8001), Rest)
threading.Thread(target=rest.serve_forever, daemon=True).start()
p = Plugin(BIN, WORK, 'samsung-tv')
cfg = {'host': '127.0.0.1', 'port': 8002, 'mac': 'AA:BB:CC:00:11:22'}


def name_q(name):
    """The client name as the TV wants it in the query: base64, URL-encoded"""
    return 'name=' + quote(base64.b64encode(name.encode()).decode(), safe='')


def key(m):
    return m and m.get('method') == 'ms.remote.control' and (m['params']['Cmd'], m['params']['DataOfCmd'])


try:
    p.clear_status()
    p.send('pair', 'start_pair', host='127.0.0.1', port=8002, timeout_s=20)
    time.sleep(1.0)
    check('pairing: no answer before Allow', p.status(0.2) is None and len(TV.pending) == 1)
    allow()
    st = p.status()
    check('pairing: done after Allow', st and st['ok'] and st['stage'] == 'done')
    check('pairing a 2019 TV ticks HDMI via IR', st and st.get('config', {}).get('hdmi_via_ir') is True)
    check('pairing: token saved', open(os.path.join(p.data, 'token-127.0.0.1')).read().strip() == TOKEN)

    check('no hub_name from the Hub (older firmware): registered as Harmony-NG Hub',
          name_q('Harmony-NG Hub') in srv.conns[-1].path)

    p.send('Home', config=cfg)
    check('tap Home -> Click KEY_HOME', key(TV.next()) == ('Click', 'KEY_HOME'))

    # the TV pings every ~6 s; the connection must survive it (it used to wait for a next frame
    # after the pong, ran into the 2 s socket timeout and closed)
    n = len(srv.conns)
    for c in srv.open_conns():
        c.ping()
    time.sleep(3.0)
    check('a ping from the TV keeps the connection', len(srv.open_conns()) == 1 and len(srv.conns) == n
          and not p.logged('the TV closed the connection'))
    p.send('Info', config=cfg)
    check('after the ping, the next key goes on the same connection',
          key(TV.next()) == ('Click', 'KEY_INFO') and len(srv.conns) == n)

    p.send('VolumeUp', 'press', config=cfg)
    p.send('VolumeUp', 'repeat', config=cfg)
    p.send('VolumeUp', 'repeat', config=cfg)
    p.send('VolumeUp', 'release', config=cfg)
    got = [key(TV.next()), key(TV.next())]
    check('hold VolumeUp -> Press, Release (repeats kept by the TV)',
          got == [('Press', 'KEY_VOLUP'), ('Release', 'KEY_VOLUP')] and TV.next(0.5) is None)

    p.send('Down', 'press', config=cfg)
    t0 = time.time()
    got = [key(TV.next()), key(TV.next())]
    check('lost release -> Release by the plugin within 1 s',
          got == [('Press', 'KEY_DOWN'), ('Release', 'KEY_DOWN')] and time.time() - t0 < 1.5)

    p.send('app:netflix', 'press', config=cfg)
    p.send('app:netflix', 'release', config=cfg)
    m = TV.next()
    check('app:netflix -> ed.apps.launch 3201907018807 once',
          m and m['params']['event'] == 'ed.apps.launch' and m['params']['data']['appId'] == '3201907018807'
          and TV.next(0.5) is None)

    p.send('text:héllo', config=cfg)
    m = TV.next()
    check('text -> SendInputString base64', m and m['params']['TypeOfRemote'] == 'SendInputString'
          and base64.b64decode(m['params']['Cmd']).decode() == 'héllo')

    TV.power = 'on'
    p.send('PowerOn', config=cfg)
    check('PowerOn while on: nothing', TV.next(0.8) is None)
    p.send('PowerOff', config=cfg)
    check('PowerOff while on -> KEY_POWER', key(TV.next()) == ('Click', 'KEY_POWER'))
    TV.power = 'standby'
    p.send('PowerOff', config=cfg)
    check('PowerOff in standby: nothing', TV.next(0.8) is None)
    p.send('PowerOn', config=cfg)
    check('PowerOn in standby -> KEY_POWER', key(TV.next()) == ('Click', 'KEY_POWER'))
    TV.power = 'on'

    p.send('Disconnect', config=cfg)
    check('Disconnect closes the connection', wait_for(lambda: not srv.open_conns()))
    n = len(srv.conns)
    p.send('Connect', config=cfg)
    check('Connect opens one and sends nothing', wait_for(lambda: len(srv.open_conns()) == 1)
          and len(srv.conns) == n + 1 and TV.next(0.5) is None)

    for c in srv.open_conns():
        c.close()
    time.sleep(0.3)
    p.send('Back', config=cfg)
    check('TV closed the connection -> next key reconnects with the token', key(TV.next()) == ('Click', 'KEY_RETURN'))

    # idle past IDLE_PING_MS (1.5 s in the test build): a ping checks the link before the send
    time.sleep(2.0)
    n = len(srv.conns)
    p.send('Up', config=cfg)
    check('after idle, ping answered -> same connection', key(TV.next()) == ('Click', 'KEY_UP') and len(srv.conns) == n)
    srv.answer_pings = False
    time.sleep(2.0)
    p.send('Left', config=cfg)
    m = key(TV.next(4))
    srv.answer_pings = True
    check('after idle, no pong -> new connection, key delivered', m == ('Click', 'KEY_LEFT') and len(srv.conns) == n + 1)

    p.send('NoSuchKey', config=cfg)
    p.send('KEY_SOURCE', config=cfg)
    check('unknown command skipped, raw KEY_ passes', key(TV.next()) == ('Click', 'KEY_SOURCE'))

    # the Hub's name (settings Hub Name) is the name the TV shows; a rename registers it anew
    n = len(srv.conns)
    p.send('Home', config=cfg, hub_name='Harmony Living')
    m = key(TV.next(4))
    check('a new Hub name -> a new connection under that name, key delivered',
          m == ('Click', 'KEY_HOME') and len(srv.conns) == n + 1 and name_q('Harmony Living') in srv.conns[-1].path)
    p.send('Back', config=cfg, hub_name='Harmony Living')
    check('the same name again keeps that connection', key(TV.next()) == ('Click', 'KEY_RETURN') and len(srv.conns) == n + 1)

    # the TV off the network: PowerOn goes out as Wake-on-LAN
    off = dict(cfg, host='127.0.0.2')
    p.send('PowerOn', config=off)
    check('PowerOn on a TV off the network -> Wake-on-LAN',
          wait_for(lambda: p.logged('Wake-on-LAN sent to AA:BB:CC:00:11:22'), 6))

    # an older TV keeps the HDMI keys over the network
    TV.model = '17_FAKE_UHD'
    p.clear_status()
    p.send('pair', 'start_pair', host='127.0.0.1', port=8002, timeout_s=20)
    time.sleep(0.5)
    allow()
    st = p.status()
    check('pairing a 2017 TV leaves HDMI via IR off', st and st['ok'] and st.get('config', {}).get('hdmi_via_ir') is False)
    TV.model = '19_FAKE_QTV'

    # the manifest: HDMI 1-4 carry the Samsung IR codes for the Hub, switched by hdmi_via_ir
    man = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'manifest.json')))
    dd = man['device_definition']
    field = next((f for f in dd['config_fields'] if f.get('key') == 'hdmi_via_ir'), None)
    hdmi = {c['name']: c for c in dd['default_commands'] if c['name'].startswith('HDMI ')}
    codes = {'HDMI 1': '0xE0E09768', 'HDMI 2': '0xE0E07D82', 'HDMI 3': '0xE0E043BC', 'HDMI 4': '0xE0E0A35C'}
    check('manifest: HDMI via IR is a checkbox field', field is not None and field.get('type') == 'boolean')
    check('manifest: HDMI 1-4 carry their IR code and irWhen hdmi_via_ir',
          all(n in hdmi and hdmi[n].get('irWhen') == 'hdmi_via_ir' and code in hdmi[n].get('irKeyCode', '')
              for n, code in codes.items()))

    TV.mode = 'deny'
    os.unlink(os.path.join(p.data, 'token-127.0.0.1'))
    p.clear_status()
    p.send('pair', 'start_pair', host='127.0.0.1', port=8002, timeout_s=20)
    st = p.status()
    check('pairing denied on the TV -> error', st and not st['ok'] and st['stage'] == 'error')
finally:
    rc = p.stop()
check('stops on SIGTERM', rc == 0)
print('samsung-tv fake TV: %s' % ('all passed' if not failures else '%d FAILED' % len(failures)))
sys.exit(1 if failures else 0)

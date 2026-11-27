#!/usr/bin/env python3
"""End-to-end check of the roku-ecp plugin against a fake Roku (HTTP on 127.0.0.1).

  gcc roku_ecp.c -o <bin> && python3 test_fake_roku.py <bin> <work dir>
"""
import os, socket, subprocess, sys, threading, time, json, queue

BIN, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)
SOCK = os.path.join(WORK, 'roku.sock')

posts = queue.Queue()
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', 0))
srv.listen(16)
PORT = srv.getsockname()[1]


def serve():
    while True:
        c, _ = srv.accept()
        data = b''
        while b'\r\n\r\n' not in data:
            d = c.recv(4096)
            if not d:
                break
            data += d
        line = data.split(b'\r\n', 1)[0].decode()
        path = line.split(' ')[1]
        posts.put(path)
        c.sendall(b'HTTP/1.1 404 Not Found\r\n\r\n' if 'Bad' in path else b'HTTP/1.1 200 OK\r\n\r\n')
        c.close()


threading.Thread(target=serve, daemon=True).start()
log = open(os.path.join(WORK, 'plugin.log'), 'w')
proc = subprocess.Popen([BIN, SOCK], stdout=log, stderr=subprocess.STDOUT)
for _ in range(50):
    if os.path.exists(SOCK):
        break
    time.sleep(0.05)
out = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)


def mono_ms():
    return int(time.clock_gettime(time.CLOCK_MONOTONIC) * 1000)


def send(command, action='tap', ts=None, host='127.0.0.1'):
    msg = {'plugin_id': 'roku-ecp', 'command': command, 'action': action, 'value': 0,
           'ts': mono_ms() if ts is None else ts,
           'config': {'ip_address': host, 'port': PORT}}
    out.sendto(json.dumps(msg, separators=(',', ':')).encode(), SOCK)


def expect(*paths, wait=1.0):
    got = []
    for _ in paths:
        got.append(posts.get(timeout=wait))
    assert got == list(paths), f'got {got}, want {list(paths)}'


def expect_none(wait=0.3):
    try:
        p = posts.get(timeout=wait)
    except queue.Empty:
        return
    raise AssertionError(f'unexpected {p}')


try:
    send('DirectionUp')
    expect('/keypress/Up')

    # the address may be a host name the Hub resolves, not only an IPv4 address
    send('DirectionUp', host='localhost')
    expect('/keypress/Up')
    send('DirectionUp', host='bad/name')
    expect_none()

    # Hold: one keydown, repeats only keep it alive, one keyup
    send('Down', 'press')
    expect('/keydown/Down')
    for _ in range(3):
        time.sleep(0.2)
        send('Down', 'repeat')
    send('Down', 'release')
    expect('/keyup/Down')
    expect_none()

    # Lost release: released by the hold deadline
    send('Left', 'press')
    expect('/keydown/Left')
    expect('/keyup/Left', wait=1.5)
    send('Left', 'release')
    expect_none()

    # Stale repeat dropped
    send('Right', 'repeat', ts=mono_ms() - 5000)
    expect_none()

    # Launch and text once per button, encoded
    for a in ('press', 'repeat', 'release'):
        send('Launch:12?contentId=a%20b&mediaType=movie', a)
    expect('/launch/12?contentId=a%20b&mediaType=movie')
    expect_none()
    send('Text:a,b é', 'press')
    send('Text:a,b é', 'release')
    expect('/keypress/Lit_a', '/keypress/Lit_%2C', '/keypress/Lit_b', '/keypress/Lit_%20', '/keypress/Lit_%C3%A9')
    expect_none()

    # A key name that would break the request line is encoded
    send('Bad Key\r\nX')
    expect('/keypress/Bad%20Key%0D%0AX')
    time.sleep(0.2)

    # An empty datagram does not stop the daemon
    out.sendto(b'', SOCK)
    time.sleep(0.2)
    assert proc.poll() is None, 'daemon exited on an empty datagram'
    send('Home')
    expect('/keypress/Home')

    log.flush()
    text = open(os.path.join(WORK, 'plugin.log')).read()
    assert "answered 'HTTP/1.1 404 Not Found'" in text, text
    print('[+] roku-ecp fake Roku checks passed')
finally:
    proc.terminate()
    proc.wait(5)

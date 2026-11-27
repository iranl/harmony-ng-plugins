"""Minimal WebSocket server (RFC 6455, plain or TLS) for the host tests of the TV plugins.

FakeWsServer(port, tls=True, on_open=..., on_text=...) accepts connections in a thread; each
connection is a Conn with .path, .send(text), .close() and .closed. on_open(conn) runs after
the upgrade, on_text(conn, text) for every text frame. Pings are answered.
Also: plugin helpers (start the plugin, send a datagram, read the status file, check()).
"""
import base64, hashlib, json, os, socket, ssl, subprocess, threading, time

GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


def make_cert(work, name='tv'):
    os.makedirs(work, exist_ok=True)
    cert, key = os.path.join(work, name + '.pem'), os.path.join(work, name + '.key')
    if not os.path.exists(cert):
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert,
                        '-days', '2', '-subj', '/CN=' + name], check=True, capture_output=True)
    return cert, key


class Conn:
    def __init__(self, sock, path):
        self.sock, self.path, self.closed = sock, path, False
        self.lock = threading.Lock()

    def _frame(self, op, data):
        n = len(data)
        hdr = bytes([0x80 | op]) + (bytes([n]) if n < 126 else bytes([126]) + n.to_bytes(2, 'big'))
        with self.lock:
            self.sock.sendall(hdr + data)

    def send(self, text):
        try:
            self._frame(1, text.encode() if isinstance(text, str) else json.dumps(text).encode())
        except OSError:
            self.closed = True

    def ping(self, data=b'\0'):
        """A ping from the TV (a Samsung pings every ~6 s with one zero byte)."""
        try:
            self._frame(9, data)
        except OSError:
            self.closed = True

    def close(self):
        self.closed = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


class FakeWsServer:
    def __init__(self, port, work, tls=True, on_open=None, on_text=None, cert_name='tv'):
        self.on_open, self.on_text = on_open, on_text
        self.conns = []
        self.answer_pings = True   # False: a TV that went away without closing
        self.ctx = None
        if tls:
            self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.ctx.load_cert_chain(*make_cert(work, cert_name))
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(('127.0.0.1', port))
        self.srv.listen(8)
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            raw, _ = self.srv.accept()
            threading.Thread(target=self._serve, args=(raw,), daemon=True).start()

    def _serve(self, raw):
        try:
            s = self.ctx.wrap_socket(raw, server_side=True) if self.ctx else raw
            f = s.makefile('rb')
            req = b''
            while not req.endswith(b'\r\n\r\n'):
                b = f.read(1)
                if not b:
                    return
                req += b
            lines = req.decode().split('\r\n')
            path = lines[0].split(' ')[1]
            key = [l.split(':', 1)[1].strip() for l in lines if l.lower().startswith('sec-websocket-key:')][0]
            acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
            s.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                       'Sec-WebSocket-Accept: %s\r\n\r\n' % acc).encode())
            c = Conn(s, path)
            self.conns.append(c)
            if self.on_open:
                self.on_open(c)
            while True:
                h = f.read(2)
                if len(h) < 2:
                    break
                op, n = h[0] & 0x0F, h[1] & 0x7F
                if n == 126:
                    n = int.from_bytes(f.read(2), 'big')
                elif n == 127:
                    n = int.from_bytes(f.read(8), 'big')
                mask = f.read(4) if h[1] & 0x80 else b'\0\0\0\0'
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(f.read(n)))
                if op == 8:
                    break
                if op == 9 and self.answer_pings:
                    c._frame(10, data)
                elif op == 1 and self.on_text:
                    self.on_text(c, data.decode())
        except (OSError, ssl.SSLError, IndexError, ValueError):
            pass
        finally:
            try:
                c.closed = True
            except NameError:
                pass

    def open_conns(self):
        return [c for c in self.conns if not c.closed]


class Plugin:
    """The plugin under test: its process, datagram socket and status file."""

    def __init__(self, binary, work, pid, env_extra=None):
        os.makedirs(work, exist_ok=True)
        self.sock, self.pid = os.path.join(work, pid + '.sock'), pid
        self.data = os.path.join(work, 'data')
        os.makedirs(self.data, exist_ok=True)
        self.status_path = '/tmp/plugins/%s.status' % pid
        self.log_path = os.path.join(work, 'plugin.log')
        self.log = open(self.log_path, 'w')
        env = dict(os.environ, HARMONY_PLUGIN_DATA=self.data, **(env_extra or {}))
        self.proc = subprocess.Popen([binary, self.sock], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(100):
            if os.path.exists(self.sock):
                break
            time.sleep(0.05)

    def send(self, command, action='tap', **fields):
        msg = {'plugin_id': self.pid, 'command': command, 'action': action,
               'ts': int(time.clock_gettime(time.CLOCK_MONOTONIC) * 1000)}
        msg.update(fields)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.sendto(json.dumps(msg).encode(), self.sock)
        s.close()

    def clear_status(self):
        try:
            os.unlink(self.status_path)
        except OSError:
            pass

    def status(self, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            try:
                with open(self.status_path) as f:
                    return json.load(f)
            except (OSError, ValueError):
                time.sleep(0.1)
        return None

    def logged(self, text):
        self.log.flush()
        with open(self.log_path) as f:
            return text in f.read()

    def stop(self):
        self.proc.terminate()
        try:
            return self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return -1


failures = []


def check(name, cond):
    print(('  PASS ' if cond else '  FAIL ') + name)
    if not cond:
        failures.append(name)


def wait_for(pred, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.05)
    return pred()

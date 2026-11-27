/* WebSocket client (RFC 6455) over plain TCP (ws://) or TLS (wss://, tls.h), for TV
 * plugins. One connection per ws_t; text frames out (masked), whole messages in, pings
 * answered. Header only, needs plugin.h and tls.h. */
#ifndef HNG_WS_H
#define HNG_WS_H

#include "plugin.h"
#include "tls.h"

typedef struct {
    int fd;           /* -1 when closed */
    int secure;
    tls_conn_t *tls;  /* the TLS state of a wss:// connection (caller's storage) */
    uint64_t last_rx_ms;  /* the last frame from the device */
} ws_t;

PLG_FN int ws_io_write(ws_t *w, const void *buf, size_t len) {
    return w->secure ? tls_write(w->tls, buf, len) : plg_send_all(w->fd, buf, len);
}

PLG_FN int ws_io_read(ws_t *w, void *buf, size_t len) {
    if (w->secure) return tls_read(w->tls, buf, len);
    unsigned char *p = buf;
    while (len) {
        ssize_t n = recv(w->fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

PLG_FN void ws_close(ws_t *w) {
    if (w->fd < 0) return;
    if (w->secure) tls_close(w->tls);
    else close(w->fd);
    w->fd = -1;
}

/* Bytes waiting: the socket, or (wss) data BearSSL holds that poll() cannot see */
PLG_FN int ws_pending(ws_t *w) {
    return w->fd >= 0 && w->secure && tls_pending(w->tls);
}

/* Connects to host:port, TLS when tls is not NULL (key pinned in pin_path), and upgrades on
 * path ("/..."). 0 when connected. */
PLG_FN int ws_connect(ws_t *w, const char *host, int port, const char *path, tls_conn_t *tls,
                      const char *pin_path, int timeout_ms) {
    w->fd = -1;
    w->secure = tls != NULL;
    w->tls = tls;
    int fd = plg_tcp_connect(host, port, timeout_ms);
    if (fd < 0) return -1;
    if (tls) {
        if (tls_open(tls, fd, pin_path) != 0) return -1;
    }
    w->fd = fd;
    w->last_rx_ms = plg_mono_ms();

    unsigned char nonce[16];
    char key[32];
    plg_random(nonce, sizeof(nonce));
    plg_base64(nonce, sizeof(nonce), key, sizeof(key));
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     path, host, port, key);
    if (n <= 0 || (size_t)n >= sizeof(req) || ws_io_write(w, req, (size_t)n) != 0) {
        ws_close(w);
        return -1;
    }
    /* the answer's headers, byte by byte up to the blank line (frames may follow at once) */
    char hdr[1024];
    size_t got = 0;
    while (got + 1 < sizeof(hdr)) {
        if (ws_io_read(w, hdr + got, 1) != 0) break;
        got++;
        if (got >= 4 && memcmp(hdr + got - 4, "\r\n\r\n", 4) == 0) break;
    }
    hdr[got] = '\0';
    if (strncmp(hdr, "HTTP/1.1 101", 12) != 0) {
        char *eol = strpbrk(hdr, "\r\n");
        if (eol) *eol = '\0';
        printf("[-] WebSocket upgrade on %s:%d%s refused: '%s'\n", host, port, path, hdr);
        ws_close(w);
        return -1;
    }
    return 0;
}

/* One frame (FIN set) of opcode 1 (text), 9/10 (ping/pong) or 8 (close), masked */
PLG_FN int ws_send_frame(ws_t *w, int opcode, const void *data, size_t len) {
    unsigned char hdr[14];
    size_t h = 0;
    hdr[h++] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        hdr[h++] = (unsigned char)(0x80 | len);
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(len >> 8);
        hdr[h++] = (unsigned char)len;
    } else {
        return -1;
    }
    unsigned char mask[4];
    plg_random(mask, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;
    unsigned char buf[4096];
    if (h + len > sizeof(buf)) return -1;
    memcpy(buf, hdr, h);
    const unsigned char *d = data;
    for (size_t i = 0; i < len; i++) buf[h + i] = d[i] ^ mask[i & 3];
    return ws_io_write(w, buf, h + len);
}

PLG_FN int ws_send_text(ws_t *w, const char *text) {
    if (w->fd < 0) return -1;
    if (ws_send_frame(w, 1, text, strlen(text)) != 0) {
        ws_close(w);
        return -1;
    }
    return 0;
}

/* The next whole message (text or binary) into out, NUL-terminated; a message longer than
 * out is cut (the rest read and dropped). Pings are answered. Returns the length, 0 for a
 * pong, -1 when the connection closed (and is closed here). Blocks until a frame, bounded by
 * the socket's timeout: call when ws_pending() or poll() says there is data. */
PLG_FN int ws_recv(ws_t *w, char *out, size_t out_sz) {
    size_t len = 0;
    for (;;) {
        unsigned char h[2];
        if (w->fd < 0 || ws_io_read(w, h, 2) != 0) goto closed;
        w->last_rx_ms = plg_mono_ms();
        int fin = h[0] & 0x80, op = h[0] & 0x0F, masked = h[1] & 0x80;
        uint64_t n = h[1] & 0x7F;
        unsigned char ext[8];
        if (n == 126) {
            if (ws_io_read(w, ext, 2) != 0) goto closed;
            n = (uint64_t)ext[0] << 8 | ext[1];
        } else if (n == 127) {
            if (ws_io_read(w, ext, 8) != 0) goto closed;
            n = 0;
            for (int i = 0; i < 8; i++) n = n << 8 | ext[i];
        }
        unsigned char mask[4] = {0};
        if (masked && ws_io_read(w, mask, 4) != 0) goto closed;
        if (n > (1u << 20)) goto closed;
        if (op >= 8) { /* control frame, at most 125 bytes */
            unsigned char c[128];
            if (n > sizeof(c) || ws_io_read(w, c, (size_t)n) != 0) goto closed;
            if (op == 8) goto closed;
            if (op == 9 && ws_send_frame(w, 10, c, (size_t)n) != 0) goto closed;
            /* A ping or pong between messages is the whole read: waiting here for a next frame
             * that is not coming ran into the socket timeout and closed a healthy connection
             * (a Samsung TV pings every ~6 s). Inside a fragmented message, read on. */
            if ((op == 9 || op == 10) && len == 0) {
                out[0] = '\0';
                return 0;
            }
            continue;
        }
        uint64_t done = 0;
        while (done < n) {
            unsigned char chunk[512];
            size_t k = n - done > sizeof(chunk) ? sizeof(chunk) : (size_t)(n - done);
            if (ws_io_read(w, chunk, k) != 0) goto closed;
            for (size_t i = 0; i < k; i++, done++) {
                if (len + 1 < out_sz) out[len++] = (char)(chunk[i] ^ mask[done & 3]);
            }
        }
        if (fin) break;
    }
    out[len] = '\0';
    return (int)len;
closed:
    ws_close(w);
    return -1;
}

/* Whether the connection still works: when nothing came for idle_ms, a ping must get an
 * answer within wait_ms (a TV switched off leaves a connection that takes writes and delivers
 * nothing). A message that arrives meanwhile goes to on_msg (may be NULL). Closed when dead. */
PLG_FN int ws_alive(ws_t *w, int idle_ms, int wait_ms, void (*on_msg)(const char *)) {
    if (w->fd < 0) return 0;
    if (plg_mono_ms() - w->last_rx_ms < (uint64_t)idle_ms) return 1;
    uint64_t sent = plg_mono_ms();
    if (ws_send_frame(w, 9, "hng", 3) != 0) {
        ws_close(w);
        return 0;
    }
    static char msg[8192];
    while (w->fd >= 0 && w->last_rx_ms < sent) {
        int left = wait_ms - (int)(plg_mono_ms() - sent);
        if (left <= 0) break;
        struct pollfd pfd = { .fd = w->fd, .events = POLLIN };
        if (!ws_pending(w) && poll(&pfd, 1, left) <= 0) continue;
        int n = ws_recv(w, msg, sizeof(msg));
        if (n > 0 && on_msg) on_msg(msg);
    }
    if (w->fd >= 0 && w->last_rx_ms < sent) {
        printf("[-] no answer to a WebSocket ping in %d ms: connection dropped\n", wait_ms);
        ws_close(w);
    }
    return w->fd >= 0;
}

#endif

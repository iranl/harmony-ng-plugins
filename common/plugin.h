/* Shared code of the Harmony-NG plugins: socket, datagram fields, status file, plain TCP and HTTP.
 * Header only: #include "plugin.h" (build_plugins.sh passes -I common). Every function is static,
 * so a plugin carries only what it calls (-ffunction-sections -Wl,--gc-sections).
 *
 * The plugins run on the Linux 6.18 Hub system (HarmonyNG 4, shared uClibc-ng): <ctype.h>,
 * threads and blocking waits work. One poll() loop per plugin is still the smallest design. */
#ifndef HNG_PLUGIN_H
#define HNG_PLUGIN_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PLG_FN static __attribute__((unused))

/* A held key with no repeat or release for this long is released by the plugin (a lost
 * release); the Hub's deadline for held IR keys is the same */
#define PLG_HOLD_DEADLINE_MS 600
/* A repeat that waited this long in the queue is dropped: replayed late, it overshoots */
#define PLG_STALE_REPEAT_MS 1000

static volatile int plg_running = 1;

PLG_FN void plg_on_signal(int sig) {
    (void)sig;
    plg_running = 0;
}

PLG_FN uint64_t plg_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

PLG_FN int plg_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Value of the first "key": in buf, as text (a string with its escapes undone, else the bare
 * token). Empty when the key is missing. The Hub's datagrams have unique keys, a "config" key
 * included, so the first match is the one meant. */
PLG_FN void plg_json_str(const char *buf, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char search[72];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ') p++;
    size_t i = 0;
    if (*p != '"') {
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && i + 1 < out_sz) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return;
    }
    p++;
    while (*p && *p != '"' && i + 1 < out_sz) {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else if (e == 'b') c = '\b';
            else if (e == 'f') c = '\f';
            else if (e == 'u') {
                unsigned int cp = 0;
                int k = 0;
                for (; k < 4 && plg_hex_nibble(p[k]) >= 0; k++) cp = cp * 16 + (unsigned)plg_hex_nibble(p[k]);
                p += k;
                if (cp < 0x80) c = (char)cp;
                else if (cp < 0x800 && i + 2 < out_sz) {
                    out[i++] = (char)(0xC0 | (cp >> 6));
                    c = (char)(0x80 | (cp & 0x3F));
                } else if (i + 3 < out_sz) {
                    out[i++] = (char)(0xE0 | (cp >> 12));
                    out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    c = (char)(0x80 | (cp & 0x3F));
                } else {
                    break;
                }
            } else {
                c = e; /* \" \\ \/ */
            }
        }
        out[i++] = c;
    }
    out[i] = '\0';
}

PLG_FN long plg_json_int(const char *buf, const char *key, long dflt) {
    char v[32];
    plg_json_str(buf, key, v, sizeof(v));
    if (!v[0]) return dflt;
    char *end;
    long n = strtol(v, &end, 10);
    return end == v ? dflt : n;
}

/* s as the inside of a JSON string (quotes, backslashes and control characters escaped) */
PLG_FN void plg_json_escape(const char *s, char *out, size_t out_sz) {
    size_t i = 0;
    for (; *s && i + 7 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[i++] = '\\';
            out[i++] = (char)c;
        } else if (c < 0x20) {
            i += (size_t)snprintf(out + i, out_sz - i, "\\u%04x", c);
        } else {
            out[i++] = (char)c;
        }
    }
    out[i] = '\0';
}

PLG_FN int plg_streq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

PLG_FN int plg_prefix(const char *s, const char *prefix) {
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

/* The Hub's command datagram, the fields every plugin reads */
typedef struct {
    char command[512];
    char action[24];  /* tap, press, repeat, release; start_pair, finish_pair for pairing */
    char host[64];    /* config "host" (or "ip_address") */
    long value;
    int is_press, is_repeat, is_release, is_tap;
    int stale;        /* a repeat that waited too long: drop it */
} plg_msg_t;

PLG_FN void plg_parse(const char *buf, plg_msg_t *m) {
    memset(m, 0, sizeof(*m));
    plg_json_str(buf, "command", m->command, sizeof(m->command));
    plg_json_str(buf, "action", m->action, sizeof(m->action));
    if (!m->action[0]) strcpy(m->action, "tap");
    plg_json_str(buf, "host", m->host, sizeof(m->host));
    if (!m->host[0]) plg_json_str(buf, "ip_address", m->host, sizeof(m->host));
    m->value = plg_json_int(buf, "value", 0);
    m->is_press = strcmp(m->action, "press") == 0;
    m->is_repeat = strcmp(m->action, "repeat") == 0;
    m->is_release = strcmp(m->action, "release") == 0;
    m->is_tap = !m->is_press && !m->is_repeat && !m->is_release;
    char ts[24];
    plg_json_str(buf, "ts", ts, sizeof(ts));
    uint64_t t = strtoull(ts, NULL, 10);
    m->stale = m->is_repeat && t && plg_mono_ms() > t + PLG_STALE_REPEAT_MS;
}

/* A one-shot command (wake, launch, text) runs on tap or press: a remote button also sends
 * repeats and a release */
PLG_FN int plg_once(const plg_msg_t *m) {
    return m->is_tap || m->is_press;
}

/* Signals, line-buffered stdout, and the bound datagram socket (argv[1], else HARMONY_SOCKET,
 * else /tmp/plugins/<id>.sock). -1 on error. */
PLG_FN int plg_open(int argc, char **argv, const char *id, char *path, size_t path_sz) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, plg_on_signal);
    signal(SIGTERM, plg_on_signal);
    signal(SIGPIPE, SIG_IGN);
    const char *p = argc > 1 && argv[1][0] ? argv[1] : getenv("HARMONY_SOCKET");
    if (p && p[0]) snprintf(path, path_sz, "%s", p);
    else snprintf(path, path_sz, "/tmp/plugins/%s.sock", id);
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket AF_UNIX");
        return -1;
    }
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, path, sizeof(sun.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
        perror("bind AF_UNIX");
        close(fd);
        return -1;
    }
    printf("[*] %s: listening on %s\n", id, path);
    return fd;
}

/* One datagram into buf (NUL-terminated). 1 read, 0 nothing (timeout, EINTR), -1 error. */
PLG_FN int plg_recv(int fd, char *buf, size_t sz, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return errno == EINTR ? 0 : -1;
    if (pr == 0 || !(pfd.revents & POLLIN)) return 0;
    ssize_t n = recv(fd, buf, sz - 1, 0);
    if (n < 0) return errno == EINTR ? 0 : -1;
    buf[n] = '\0';
    return 1;
}

/* HARMONY_PLUGIN_DATA (kept across updates), created when missing */
PLG_FN const char *plg_data_dir(const char *id) {
    static char dir[160];
    const char *d = getenv("HARMONY_PLUGIN_DATA");
    if (d && d[0]) snprintf(dir, sizeof(dir), "%s", d);
    else snprintf(dir, sizeof(dir), "/data/harmony/plugin-data/%s", id);
    mkdir(dir, 0700);
    return dir;
}

/* Whole small file into buf (NUL-terminated, trailing CR/LF dropped). Length, -1 when missing. */
PLG_FN int plg_read_file(const char *path, char *buf, size_t sz) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return (int)n;
}

/* Written whole then renamed (readers never see half a file). Unchanged content is not
 * written again: /data is flash. */
PLG_FN int plg_write_file(const char *path, const char *text, mode_t mode) {
    char old[1024];
    if (strlen(text) < sizeof(old) && plg_read_file(path, old, sizeof(old)) >= 0 && strcmp(old, text) == 0) return 0;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* The pairing answer the Hub polls (/tmp/plugins/<id>.status). config_json: "" or the inside
 * of the "config" object, e.g. "\"mac\": \"AA:BB:CC:DD:EE:FF\"". */
PLG_FN void plg_status(const char *id, int ok, const char *action, const char *stage, const char *msg,
                       const char *config_json) {
    char path[128], esc[512], text[1200];
    mkdir("/tmp/plugins", 0755);
    snprintf(path, sizeof(path), "/tmp/plugins/%s.status", id);
    plg_json_escape(msg, esc, sizeof(esc));
    snprintf(text, sizeof(text),
             "{\"ok\": %s, \"action\": \"%s\", \"stage\": \"%s\", \"message\": \"%s\"%s%s%s}\n",
             ok ? "true" : "false", action, stage, esc,
             config_json && config_json[0] ? ", \"config\": {" : "", config_json ? config_json : "",
             config_json && config_json[0] ? "}" : "");
    plg_write_file(path, text, 0644);
}

PLG_FN int plg_parse_ipv4(const char *host, struct in_addr *out) {
    return host && inet_pton(AF_INET, host, out) == 1 ? 0 : -1;
}

/* A host name as a device address may be written: letters, digits, '-' and '.', 1 to 253
 * characters, not starting with '-' or '.' (also keeps it safe as part of a file name) */
PLG_FN int plg_host_name_ok(const char *host) {
    if (!host || !host[0] || host[0] == '-' || host[0] == '.') return 0;
    size_t n = 0;
    for (const char *p = host; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return 0;
    }
    return n <= 253;
}

/* The IPv4 address of a device's host as configured: an address ("192.168.1.70") or a host
 * name ("tv.home.lan", resolved by the Hub's resolver: hosts file, then DNS). 0 when found.
 * A lookup can take a few seconds when the DNS server does not answer. */
PLG_FN int plg_resolve_ipv4(const char *host, struct in_addr *out) {
    if (plg_parse_ipv4(host, out) == 0) return 0;
    if (!plg_host_name_ok(host)) return -1;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
    int rc = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET && ai->ai_addr) {
            *out = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
            rc = 0;
            break;
        }
    }
    freeaddrinfo(res);
    return rc;
}

/* plg_resolve_ipv4 as dotted text in out (16 bytes) */
PLG_FN int plg_resolve_ipv4_text(const char *host, char out[16]) {
    struct in_addr a;
    out[0] = '\0';
    if (plg_resolve_ipv4(host, &a) != 0) return -1;
    return inet_ntop(AF_INET, &a, out, 16) ? 0 : -1;
}

/* TCP connection to host:port (IPv4 address or host name), connect bounded by timeout_ms, then reads and
 * writes bounded by the same time. -1 on error. */
PLG_FN int plg_tcp_connect(const char *host, int port, int timeout_ms) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (plg_resolve_ipv4(host, &sin.sin_addr) != 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&sin, sizeof(sin));
    if (r != 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int err = 0;
        socklen_t len = sizeof(err);
        if (poll(&pfd, 1, timeout_ms) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) r = 0;
    }
    if (r != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

PLG_FN int plg_send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Plain HTTP/1.1 request, Connection: close. headers: "" or lines each ending in "\r\n".
 * The body of the answer goes to resp (NUL-terminated, cut at resp_sz). Returns the status
 * code, -1 when there was no answer. */
PLG_FN int plg_http(const char *host, int port, const char *method, const char *path, const char *headers,
                    const char *body, char *resp, size_t resp_sz, int timeout_ms) {
    if (resp && resp_sz) resp[0] = '\0';
    int fd = plg_tcp_connect(host, port, timeout_ms);
    if (fd < 0) return -1;
    size_t blen = body ? strlen(body) : 0;
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: Harmony-NG\r\n%sContent-Length: %u\r\nConnection: close\r\n\r\n",
                     method, path, host, port, headers ? headers : "", (unsigned)blen);
    if (n <= 0 || (size_t)n >= sizeof(req) || plg_send_all(fd, req, (size_t)n) != 0 ||
        (blen && plg_send_all(fd, body, blen) != 0)) {
        close(fd);
        return -1;
    }
    char buf[4096];
    size_t got = 0;
    for (;;) {
        ssize_t r = recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
        if (got == sizeof(buf) - 1) break;
    }
    close(fd);
    buf[got] = '\0';
    if (strncmp(buf, "HTTP/", 5) != 0) return -1;
    const char *sp = strchr(buf, ' ');
    int code = sp ? atoi(sp + 1) : -1;
    const char *b = strstr(buf, "\r\n\r\n");
    if (resp && resp_sz && b) snprintf(resp, resp_sz, "%s", b + 4);
    return code;
}

PLG_FN size_t plg_base64(const unsigned char *in, size_t len, char *out, size_t out_sz) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 5 < out_sz; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

PLG_FN int plg_random(void *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

/* A held key with no repeat or release for this long is released (a lost release kept the
 * key down on the Roku); the hub's IR hold deadline is the same */
#define ROKU_HOLD_DEADLINE_MS 600
/* A repeat that waited this long in the queue is dropped: replayed late, it overshoots */
#define ROKU_STALE_REPEAT_MS  1000

static volatile int s_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    s_running = 0;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *map_roku_key(const char *cmd) {
    if (!cmd || !cmd[0]) return "Select";

    if (strcasecmp(cmd, "Home") == 0) return "Home";
    if (strcasecmp(cmd, "Back") == 0 || strcasecmp(cmd, "Return") == 0) return "Back";
    if (strcasecmp(cmd, "Select") == 0 || strcasecmp(cmd, "OK") == 0 || strcasecmp(cmd, "Enter") == 0) return "Select";
    if (strcasecmp(cmd, "Up") == 0 || strcasecmp(cmd, "DirectionUp") == 0) return "Up";
    if (strcasecmp(cmd, "Down") == 0 || strcasecmp(cmd, "DirectionDown") == 0) return "Down";
    if (strcasecmp(cmd, "Left") == 0 || strcasecmp(cmd, "DirectionLeft") == 0) return "Left";
    if (strcasecmp(cmd, "Right") == 0 || strcasecmp(cmd, "DirectionRight") == 0) return "Right";
    if (strcasecmp(cmd, "Play") == 0 || strcasecmp(cmd, "Pause") == 0 || strcasecmp(cmd, "PlayPause") == 0) return "Play";
    if (strcasecmp(cmd, "Rev") == 0 || strcasecmp(cmd, "Rewind") == 0) return "Rev";
    if (strcasecmp(cmd, "Fwd") == 0 || strcasecmp(cmd, "FastForward") == 0) return "Fwd";
    if (strcasecmp(cmd, "VolumeUp") == 0) return "VolumeUp";
    if (strcasecmp(cmd, "VolumeDown") == 0) return "VolumeDown";
    if (strcasecmp(cmd, "VolumeMute") == 0 || strcasecmp(cmd, "Mute") == 0) return "VolumeMute";
    if (strcasecmp(cmd, "PowerOff") == 0) return "PowerOff";
    if (strcasecmp(cmd, "PowerOn") == 0) return "PowerOn";
    if (strcasecmp(cmd, "Power") == 0 || strcasecmp(cmd, "PowerToggle") == 0) return "Power";
    if (strcasecmp(cmd, "InstantReplay") == 0 || strcasecmp(cmd, "Replay") == 0) return "InstantReplay";
    if (strcasecmp(cmd, "Info") == 0 || strcasecmp(cmd, "Guide") == 0) return "Info";
    if (strcasecmp(cmd, "Search") == 0) return "Search";
    if (strcasecmp(cmd, "Backspace") == 0) return "Backspace";
    if (strcasecmp(cmd, "ChannelUp") == 0) return "ChannelUp";
    if (strcasecmp(cmd, "ChannelDown") == 0) return "ChannelDown";
    if (strcasecmp(cmd, "InputHDMI1") == 0) return "InputHDMI1";
    if (strcasecmp(cmd, "InputHDMI2") == 0) return "InputHDMI2";
    if (strcasecmp(cmd, "InputHDMI3") == 0) return "InputHDMI3";
    if (strcasecmp(cmd, "InputHDMI4") == 0) return "InputHDMI4";
    if (strcasecmp(cmd, "InputAV1") == 0) return "InputAV1";
    if (strcasecmp(cmd, "InputTuner") == 0) return "InputTuner";

    return cmd; /* Custom pass-through key */
}

/* Appends s to out, percent-encoded except for unreserved characters and those in keep: a
 * space or CR in a key name or app ID broke the request line */
static void url_append(char *out, size_t out_sz, const char *s, const char *keep) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i = strlen(out);
    for (; *s && i + 4 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || strchr("-._~", c) || (keep && strchr(keep, c))) {
            out[i++] = (char)c;
        } else {
            out[i++] = '%';
            out[i++] = hex[c >> 4];
            out[i++] = hex[c & 15];
        }
    }
    out[i] = '\0';
}

/* The IPv4 address of the Roku as configured: an address or a host name the Hub can resolve
 * (hosts file, then DNS; letters, digits, '-' and '.' only). 0 when found. */
static int resolve_ipv4(const char *host, struct in_addr *out) {
    if (inet_pton(AF_INET, host, out) == 1) return 0;
    if (!host[0] || host[0] == '-' || host[0] == '.' || strlen(host) > 253) return -1;
    for (const char *p = host; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return -1;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* POSTs /path (already encoded) and reads the status line. 0 when the Roku answered 2xx. */
static int send_roku_http_post(const char *ip, int port, const char *path) {
    if (!ip || !ip[0] || !path || !path[0]) return -1;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (resolve_ipv4(ip, &sin.sin_addr) != 0) {
        printf("[-] roku-ecp: '%s' is not an IPv4 address or a host name the Hub can resolve\n", ip);
        return -1;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 800000 }; /* 800ms timeout */
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        /* SO_SNDTIMEO makes a connect that runs out of time fail with EINPROGRESS */
        printf("[-] roku-ecp: connect to %s:%d failed: %s\n", ip, port,
               (errno == EINPROGRESS || errno == EAGAIN) ? "timed out" : strerror(errno));
        close(s);
        return -1;
    }

    char req[1280];
    int len = snprintf(req, sizeof(req),
                       "POST /%s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "User-Agent: Harmony-Hub-Plugin\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n\r\n",
                       path, ip, port);
    if (len <= 0 || (size_t)len >= sizeof(req) || send(s, req, (size_t)len, MSG_NOSIGNAL) != len) {
        printf("[-] roku-ecp: POST /%s to %s:%d not sent\n", path, ip, port);
        close(s);
        return -1;
    }

    char resp[64];
    ssize_t n = recv(s, resp, sizeof(resp) - 1, 0);
    close(s);
    if (n <= 0) {
        printf("[-] roku-ecp: no answer to POST /%s from %s:%d\n", path, ip, port);
        return -1;
    }
    resp[n] = '\0';
    const char *code = strchr(resp, ' ');
    if (!code || code[1] != '2') {
        char *eol = strpbrk(resp, "\r\n");
        if (eol) *eol = '\0';
        printf("[-] roku-ecp: POST /%s answered '%s'\n", path, resp);
        return -1;
    }
    return 0;
}

static int send_roku_key(const char *ip, int port, const char *endpoint, const char *key) {
    char path[512];
    snprintf(path, sizeof(path), "%s/", endpoint);
    url_append(path, sizeof(path), key, NULL);
    return send_roku_http_post(ip, port, path);
}

/* One Lit_ keypress per character, a multi-byte UTF-8 character encoded whole */
static void send_roku_text(const char *ip, int port, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        size_t len = 1;
        if (*p >= 0xF0) len = 4;
        else if (*p >= 0xE0) len = 3;
        else if (*p >= 0xC0) len = 2;
        char ch[5] = {0};
        size_t k = 0;
        for (; k < len && p[k]; k++) ch[k] = (char)p[k];
        p += k;

        char path[64] = "keypress/Lit_";
        url_append(path, sizeof(path), ch, NULL);
        if (send_roku_http_post(ip, port, path) != 0) return; /* one timeout, not one per character */
        usleep(30000);
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Value of the first "key": in buf, as text. A string value is read to its closing quote with
 * its escapes undone: stopping at the first ',' cut text that held one. */
static void extract_json_str(const char *buf, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(buf, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ') p++;
    size_t i = 0;
    if (*p != '"') {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && *p != '\r' && i + 1 < out_sz) {
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
                for (; k < 4 && hex_nibble(p[k]) >= 0; k++) cp = cp * 16 + (unsigned)hex_nibble(p[k]);
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
                c = e;  /* \" \\ \/ */
            }
        }
        out[i++] = c;
    }
    out[i] = '\0';
}

/* The key down on the Roku (keydown sent, keyup not yet) */
static struct { char key[256]; char ip[64]; int port; uint64_t last_ms; } s_held;

static void release_held(void) {
    if (!s_held.key[0]) return;
    send_roku_key(s_held.ip, s_held.port, "keyup", s_held.key);
    s_held.key[0] = '\0';
}

static void handle_message(const char *buf) {
    char ip[64];
    extract_json_str(buf, "ip_address", ip, sizeof(ip));
    if (!ip[0]) extract_json_str(buf, "host", ip, sizeof(ip));

    char port_str[16];
    extract_json_str(buf, "port", port_str, sizeof(port_str));
    int port = atoi(port_str);
    if (port <= 0 || port > 65535) port = 8060;

    char cmd[256];
    extract_json_str(buf, "command", cmd, sizeof(cmd));

    char action[32];
    extract_json_str(buf, "action", action, sizeof(action));
    if (!action[0]) strcpy(action, "tap");
    int is_repeat = strcmp(action, "repeat") == 0;
    int is_press = strcmp(action, "press") == 0;
    int is_release = strcmp(action, "release") == 0;

    if (!cmd[0] || !ip[0]) return;

    char ts_str[24];
    extract_json_str(buf, "ts", ts_str, sizeof(ts_str));
    uint64_t ts = strtoull(ts_str, NULL, 10);
    if (ts && is_repeat && mono_ms() > ts + ROKU_STALE_REPEAT_MS) return;

    if (!is_repeat) printf("[*] roku-ecp: Executing command '%s' (action: %s) on %s:%d\n", cmd, action, ip, port);

    /* App launches and text act once per button: they came on press, on each repeat and on
     * release, and the app was launched 2 or more times */
    int is_launch = strncasecmp(cmd, "Launch:", 7) == 0 || strncasecmp(cmd, "App:", 4) == 0;
    int is_text = strncasecmp(cmd, "Text:", 5) == 0;
    if (is_launch || is_text) {
        if (is_repeat || is_release) return;
        release_held();
        if (is_launch) {
            char path[512] = "launch/";
            /* Kept as written: the query (contentId=...&mediaType=...) and its own escapes */
            url_append(path, sizeof(path), strchr(cmd, ':') + 1, "?&=%+");
            send_roku_http_post(ip, port, path);
        } else {
            send_roku_text(ip, port, cmd + 5);
        }
        return;
    }

    const char *key = map_roku_key(cmd);
    int same = s_held.key[0] && strcmp(s_held.key, key) == 0 && strcmp(s_held.ip, ip) == 0 &&
               s_held.port == port;

    if (is_release) {
        if (same) release_held();  /* else already released by the hold deadline */
        return;
    }
    if (is_repeat && same) {       /* the Roku repeats a held key itself */
        s_held.last_ms = mono_ms();
        return;
    }
    release_held();
    if (is_press || is_repeat) {   /* a repeat with no held key: its press was lost */
        if (send_roku_key(ip, port, "keydown", key) == 0) {
            snprintf(s_held.key, sizeof(s_held.key), "%s", key);
            snprintf(s_held.ip, sizeof(s_held.ip), "%s", ip);
            s_held.port = port;
            s_held.last_ms = mono_ms();
        }
    } else {
        send_roku_key(ip, port, "keypress", key);
    }
}

int main(int argc, char **argv) {
    const char *sock_path = (argc > 1) ? argv[1] : "/tmp/plugins/roku-ecp.sock";

    setlinebuf(stdout);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sfd < 0) {
        perror("socket AF_UNIX");
        return 1;
    }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
        perror("bind AF_UNIX");
        close(sfd);
        return 1;
    }

    printf("[*] roku-ecp daemon listening on %s\n", sock_path);

    char buf[4096];
    while (s_running) {
        struct pollfd pfd = { .fd = sfd, .events = POLLIN };
        int pr = poll(&pfd, 1, s_held.key[0] ? 100 : -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (s_held.key[0] && mono_ms() - s_held.last_ms > ROKU_HOLD_DEADLINE_MS) {
            printf("[-] roku-ecp: key %s had no repeat or release for %d ms, releasing it\n",
                   s_held.key, ROKU_HOLD_DEADLINE_MS);
            release_held();
        }

        if (!(pfd.revents & POLLIN)) continue;
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            break;
        }
        buf[n] = '\0';  /* an empty datagram is ignored: it used to stop the daemon */
        handle_message(buf);
    }

    release_held();
    close(sfd);
    unlink(sock_path);
    printf("[*] roku-ecp daemon stopped\n");
    return 0;
}

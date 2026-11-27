/* LG webOS TV (2014 and later) over its SSAP WebSocket: wss://<tv>:3001 (ws://<tv>:3000 on
 * older firmware). The TV gives a client key once the user accepts the Hub on the TV screen.
 * Remote buttons go to the TV's pointer input socket; volume, channels, inputs, apps, text and
 * power are SSAP requests. Wake-on-LAN for PowerOn while the TV is off the network. One poll()
 * loop, no threads. Protocol as in aiowebostv (Apache-2.0). */
#include "plugin.h"
#include "tls.h"
#include "ws.h"
#include "wol.h"
#include "lg_manifest.h"

#define ID "lg-webos"
#define CONNECT_MS 2000
#define READY_WAIT_MS 3000
#ifndef IDLE_PING_MS
#define IDLE_PING_MS 20000
#endif

#ifndef LG_PORT_WSS
#define LG_PORT_WSS 3001
#endif
#ifndef LG_PORT_WS
#define LG_PORT_WS 3000
#endif

static tls_conn_t s_tls, s_ptls;      /* main and pointer connections (TLS state) */
static ws_t s_ws = { .fd = -1 }, s_pws = { .fd = -1 };
static struct {
    char host[64];
    int registered;      /* the TV took our client key */
    int signed_failed;   /* this TV refused the signed manifest: send the unsigned one */
    int prefer_ws;       /* wss did not work on this TV: try ws first */
    char pointer_url[256];
} s_tv;
static struct { char host[64]; uint64_t until_ms; } s_pair;
static const char *s_data;
static unsigned s_req_id;

static void key_path(const char *host, char *out, size_t sz) {
    snprintf(out, sz, "%s/key-%s", s_data, host);
}

static void tv_close(void) {
    ws_close(&s_pws);
    ws_close(&s_ws);
    s_tv.registered = 0;
    s_tv.pointer_url[0] = '\0';
    s_tv.host[0] = '\0';
}

static void pair_end(int ok, const char *msg) {
    plg_status(ID, ok, "start_pair", ok ? "done" : "error", msg, "");
    printf("[%c] " ID ": %s\n", ok ? '+' : '-', msg);
    s_pair.host[0] = '\0';
}

static int send_register(void) {
    char key[80] = "", path[200], esc[100];
    key_path(s_tv.host, path, sizeof(path));
    plg_read_file(path, key, sizeof(key));
    plg_json_escape(key, esc, sizeof(esc));
    static char msg[4096];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"register\",\"id\":\"register_0\",\"payload\":{\"forcePairing\":false,"
             "\"pairingType\":\"PROMPT\",%s%s%s\"manifest\":%s}}",
             key[0] ? "\"client-key\":\"" : "", esc, key[0] ? "\"," : "",
             s_tv.signed_failed ? LG_MANIFEST_UNSIGNED : LG_MANIFEST_SIGNED);
    return ws_send_text(&s_ws, msg);
}

/* ws://host:port/path or wss://... of the pointer socket: connected with TLS when wss */
static int pointer_connect(void) {
    if (s_pws.fd >= 0) return 0;
    if (!s_tv.pointer_url[0]) return -1;
    const char *u = s_tv.pointer_url;
    int secure = strncmp(u, "wss://", 6) == 0;
    if (!secure && strncmp(u, "ws://", 5) != 0) return -1;
    u += (secure ? 6 : 5) + strcspn(u + (secure ? 6 : 5), ":/"); /* the host part is skipped: */
    int port = secure ? 443 : 80;                                  /* the TV's own address is used */
    if (*u == ':') port = atoi(++u);
    const char *path = strchr(u, '/');
    char pin[200];
    snprintf(pin, sizeof(pin), "%s/tv-%s.pin", s_data, s_tv.host);
    if (ws_connect(&s_pws, s_tv.host, port, path ? path : "/", secure ? &s_ptls : NULL, pin, CONNECT_MS) != 0) {
        printf("[-] " ID ": the pointer socket of %s did not open\n", s_tv.host);
        return -1;
    }
    return 0;
}

static void on_msg(const char *msg) {
    char type[24], id[32];
    plg_json_str(msg, "type", type, sizeof(type));
    plg_json_str(msg, "id", id, sizeof(id));
    if (strcmp(type, "registered") == 0) {
        char key[80], path[200];
        plg_json_str(msg, "client-key", key, sizeof(key));
        if (key[0]) {
            key_path(s_tv.host, path, sizeof(path));
            if (plg_write_file(path, key, 0600) == 0) printf("[*] " ID ": client key of %s saved\n", s_tv.host);
        }
        s_tv.registered = 1;
        printf("[+] " ID ": registered with %s\n", s_tv.host);
        wol_learn(s_data, s_tv.host);
        ws_send_text(&s_ws, "{\"type\":\"request\",\"id\":\"pointer\","
                            "\"uri\":\"ssap://com.webos.service.networkinput/getPointerInputSocket\"}");
        if (s_pair.host[0] && strcmp(s_pair.host, s_tv.host) == 0) pair_end(1, "Paired: the TV accepted the Hub");
    } else if (strcmp(type, "response") == 0 && strcmp(id, "pointer") == 0) {
        plg_json_str(msg, "socketPath", s_tv.pointer_url, sizeof(s_tv.pointer_url));
    } else if (strcmp(type, "response") == 0 && strcmp(id, "register_0") == 0) {
        char pt[16];
        plg_json_str(msg, "pairingType", pt, sizeof(pt));
        if (strcmp(pt, "PROMPT") == 0) printf("[*] " ID ": %s asks to accept the Hub: choose Accept on the TV\n", s_tv.host);
    } else if (strcmp(type, "error") == 0) {
        char err[128];
        plg_json_str(msg, "error", err, sizeof(err));
        if (strcmp(id, "register_0") == 0) {
            if (!s_tv.signed_failed) { /* newer firmware: the unsigned manifest */
                s_tv.signed_failed = 1;
                printf("[*] " ID ": %s refused the signed manifest (%s), sending the unsigned one\n", s_tv.host, err);
                send_register();
                return;
            }
            printf("[-] " ID ": %s refused the Hub: %s\n", s_tv.host, err);
            if (s_pair.host[0]) pair_end(0, "The TV refused the Hub (Decline on the TV, or no answer)");
            s_tv.signed_failed = 0;
            tv_close();
        } else {
            printf("[-] " ID ": request %s failed: %s\n", id, err);
        }
    }
}

static void pump(ws_t *w, int wait_ms, int until_registered) {
    static char msg[16384];
    uint64_t end = plg_mono_ms() + (uint64_t)wait_ms;
    do {
        if (w->fd < 0) return;
        struct pollfd pfd = { .fd = w->fd, .events = POLLIN };
        int left = (int)(end - plg_mono_ms());
        if (!ws_pending(w) && poll(&pfd, 1, left > 0 ? left : 0) <= 0) {
            if (wait_ms) continue;
            return;
        }
        int n = ws_recv(w, msg, sizeof(msg));
        if (n < 0) {
            if (w == &s_ws) {
                printf("[*] " ID ": the TV closed the connection\n");
                tv_close();
            }
            return;
        }
        if (w == &s_ws && n > 0) on_msg(msg);
    } while (wait_ms && plg_mono_ms() < end &&
             (until_registered ? !s_tv.registered : !s_tv.pointer_url[0]));
}

/* Registered connection to host (wss, else ws). 0 when the TV takes requests; -1 when it is
 * off the network, refuses, or shows its accept prompt (the connection stays open for it). */
static int tv_connect(const char *host) {
    if (s_ws.fd >= 0 && strcmp(s_tv.host, host) == 0) {
        if (s_tv.registered && ws_alive(&s_ws, IDLE_PING_MS, 1000, on_msg)) return 0;
        if (!s_tv.registered && s_ws.fd >= 0) return -1; /* the prompt is still up */
    }
    tv_close();
    snprintf(s_tv.host, sizeof(s_tv.host), "%s", host);
    char pin[200];
    snprintf(pin, sizeof(pin), "%s/tv-%s.pin", s_data, host);
    int ok = -1;
    for (int i = 0; i < 2 && ok != 0; i++) {
        int wss = (i == 0) != (s_tv.prefer_ws != 0);
        ok = ws_connect(&s_ws, host, wss ? LG_PORT_WSS : LG_PORT_WS, "/", wss ? &s_tls : NULL, pin, CONNECT_MS);
        if (ok == 0) s_tv.prefer_ws = !wss;
    }
    if (ok != 0 || send_register() != 0) {
        s_tv.host[0] = '\0';
        return -1;
    }
    pump(&s_ws, READY_WAIT_MS, 1);
    if (!s_tv.registered) return -1;
    pump(&s_ws, 1500, 0); /* the pointer socket's address */
    return 0;
}

static int request(const char *host, const char *uri, const char *payload) {
    static char msg[2048];
    for (int attempt = 0; attempt < 2; attempt++) {
        if (tv_connect(host) != 0) return -1;
        snprintf(msg, sizeof(msg), "{\"type\":\"request\",\"id\":\"req_%u\",\"uri\":\"ssap://%s\"%s%s}",
                 ++s_req_id, uri, payload ? ",\"payload\":" : "", payload ? payload : "");
        if (ws_send_text(&s_ws, msg) == 0) return 0;
        tv_close();
    }
    return -1;
}

static int button(const char *host, const char *name) {
    char msg[96];
    snprintf(msg, sizeof(msg), "type:button\nname:%s\n\n", name);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (tv_connect(host) != 0) return -1;
        if (pointer_connect() == 0 && ws_send_text(&s_pws, msg) == 0) return 0;
        ws_close(&s_pws); /* a new pointer socket (the TV gives a new address on request) */
        s_tv.pointer_url[0] = '\0';
        ws_send_text(&s_ws, "{\"type\":\"request\",\"id\":\"pointer\","
                            "\"uri\":\"ssap://com.webos.service.networkinput/getPointerInputSocket\"}");
        pump(&s_ws, 1500, 0);
    }
    return -1;
}

typedef struct { const char *name, *value; } map_t;
/* Remote buttons (pointer socket names) */
static const map_t s_buttons[] = {
    {"Up", "UP"}, {"DirectionUp", "UP"}, {"Down", "DOWN"}, {"DirectionDown", "DOWN"}, {"Left", "LEFT"},
    {"DirectionLeft", "LEFT"}, {"Right", "RIGHT"}, {"DirectionRight", "RIGHT"}, {"Select", "ENTER"},
    {"OK", "ENTER"}, {"Enter", "ENTER"}, {"Back", "BACK"}, {"Return", "BACK"}, {"Exit", "EXIT"},
    {"Home", "HOME"}, {"Menu", "MENU"}, {"QuickSettings", "QMENU"}, {"Settings", "QMENU"}, {"Info", "INFO"},
    {"Guide", "GUIDE"}, {"Captions", "CC"}, {"Subtitles", "CC"}, {"VolumeUp", "VOLUMEUP"},
    {"VolumeDown", "VOLUMEDOWN"}, {"VolumeMute", "MUTE"}, {"Mute", "MUTE"}, {"ChannelUp", "CHANNELUP"},
    {"ChannelDown", "CHANNELDOWN"}, {"Play", "PLAY"}, {"Pause", "PAUSE"}, {"Stop", "STOP"},
    {"FastForward", "FASTFORWARD"}, {"Fwd", "FASTFORWARD"}, {"Rewind", "REWIND"}, {"Rev", "REWIND"},
    {"Record", "RECORD"}, {"Red", "RED"}, {"Green", "GREEN"}, {"Yellow", "YELLOW"}, {"Blue", "BLUE"},
    {"Input", "INPUT_HUB"}, {"Netflix", "NETFLIX"}, {"Amazon", "AMAZON"}, {"AspectRatio", "ASPECT_RATIO"},
    {"AudioDescription", "AD"}, {"Live", "LIVE_ZOOM"}, {"Recent", "RECENT"}, {"ScreenRemote", "SCREEN_REMOTE"},
    {"0", "0"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"}, {"6", "6"}, {"7", "7"},
    {"8", "8"}, {"9", "9"}, {"Asterisk", "ASTERISK"}, {"Dash", "DASH"}, {NULL, NULL}
};
/* webOS app ids */
static const map_t s_apps[] = {
    {"youtube", "youtube.leanback.v4"}, {"netflix", "netflix"}, {"prime", "amazon"}, {"primevideo", "amazon"},
    {"disney", "com.disney.disneyplus-prod"}, {"disney+", "com.disney.disneyplus-prod"},
    {"spotify", "spotify-beehive"}, {"plex", "cdp-30"}, {"appletv", "com.apple.appletv"},
    {"livetv", "com.webos.app.livetv"}, {"browser", "com.webos.app.browser"}, {"hdmi1", "com.webos.app.hdmi1"},
    {"hdmi2", "com.webos.app.hdmi2"}, {"hdmi3", "com.webos.app.hdmi3"}, {"hdmi4", "com.webos.app.hdmi4"},
    {NULL, NULL}
};

static const char *lookup(const map_t *m, const char *name) {
    for (; m->name; m++) if (plg_streq(m->name, name)) return m->value;
    return NULL;
}

/* A button name as webOS spells it (capitals, digits, '_'): sent as written */
static int is_raw_button(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}

/* Held buttons the TV should repeat: sent again on each repeat. Others act once. */
static int repeats(const char *b) {
    return !strcmp(b, "UP") || !strcmp(b, "DOWN") || !strcmp(b, "LEFT") || !strcmp(b, "RIGHT") ||
           !strcmp(b, "VOLUMEUP") || !strcmp(b, "VOLUMEDOWN") || !strcmp(b, "CHANNELUP") ||
           !strcmp(b, "CHANNELDOWN") || !strcmp(b, "FASTFORWARD") || !strcmp(b, "REWIND");
}

static void handle(const char *buf) {
    plg_msg_t m;
    plg_parse(buf, &m);
    if (plg_streq(m.action, "start_pair")) {
        struct in_addr a;
        if (plg_resolve_ipv4(m.host, &a) != 0) {
            plg_status(ID, 0, "start_pair", "error", "Enter the TV's address (IPv4 or a host name the Hub can resolve)", "");
            return;
        }
        long t = plg_json_int(buf, "timeout_s", 45);
        snprintf(s_pair.host, sizeof(s_pair.host), "%s", m.host);
        s_pair.until_ms = plg_mono_ms() + (uint64_t)(t > 5 ? t - 2 : 3) * 1000;
        tv_close();
        if (tv_connect(m.host) != 0 && s_ws.fd < 0) pair_end(0, "The TV does not answer: turn it on and check the address");
        return;
    }
    if (m.stale || !m.command[0] || !m.host[0]) return;
    const char *cmd = m.command;
    char payload[600], esc[512];

    const char *b = lookup(s_buttons, cmd);
    if (!b && is_raw_button(cmd)) b = cmd;
    if (b) {
        if (m.is_release || (m.is_repeat && !repeats(b))) return;
        if (button(m.host, b) != 0) printf("[-] " ID ": button %s not sent to %s\n", b, m.host);
        return;
    }
    if (!plg_once(&m)) return; /* everything below acts once per button */

    if (plg_streq(cmd, "Disconnect")) {
        if (s_ws.fd >= 0) printf("[*] " ID ": disconnected from %s\n", s_tv.host);
        tv_close();
    } else if (plg_streq(cmd, "Connect")) {
        if (tv_connect(m.host) != 0) printf("[-] " ID ": Connect to %s failed\n", m.host);
    } else if (plg_streq(cmd, "PowerOn") || plg_streq(cmd, "WakeOnLan") || plg_streq(cmd, "PowerToggle")) {
        char mac[32];
        plg_json_str(buf, "mac", mac, sizeof(mac));
        if (!plg_streq(cmd, "WakeOnLan") && tv_connect(m.host) == 0) {
            /* on (or its screen off): PowerOn turns the screen on, PowerToggle turns it off */
            request(m.host, plg_streq(cmd, "PowerOn") ? "com.webos.service.tvpower/power/turnOnScreen" : "system/turnOff", NULL);
        } else if (wol_wake(s_data, m.host, mac, "", 0) != 0) {
            printf("[-] " ID ": %s is off and its MAC is not known: set the MAC Address, or turn the TV on once "
                   "while the plugin runs\n", m.host);
        }
    } else if (plg_streq(cmd, "PowerOff")) {
        if (request(m.host, "system/turnOff", NULL) != 0) printf("[-] " ID ": %s does not answer\n", m.host);
    } else if (plg_streq(cmd, "Volume")) {
        snprintf(payload, sizeof(payload), "{\"volume\":%ld}", m.value < 0 ? 0 : m.value > 100 ? 100 : m.value);
        request(m.host, "audio/setVolume", payload);
    } else if (plg_prefix(cmd, "HDMI")) {
        const char *n = cmd + 4;
        while (*n == ' ' || *n == '_') n++;
        snprintf(payload, sizeof(payload), "{\"inputId\":\"HDMI_%c\"}", *n >= '1' && *n <= '9' ? *n : '1');
        request(m.host, "tv/switchInput", payload);
    } else if (plg_prefix(cmd, "app:") || plg_prefix(cmd, "launch:")) {
        const char *app = strchr(cmd, ':') + 1;
        const char *id = lookup(s_apps, app);
        plg_json_escape(id ? id : app, esc, sizeof(esc));
        snprintf(payload, sizeof(payload), "{\"id\":\"%s\"}", esc);
        if (request(m.host, "system.launcher/launch", payload) != 0) printf("[-] " ID ": app '%s' not launched\n", app);
    } else if (plg_prefix(cmd, "text:")) {
        plg_json_escape(cmd + 5, esc, sizeof(esc));
        snprintf(payload, sizeof(payload), "{\"text\":\"%s\",\"replace\":0}", esc);
        request(m.host, "com.webos.service.ime/insertText", payload);
    } else if (plg_prefix(cmd, "channel:")) {
        plg_json_escape(cmd + 8, esc, sizeof(esc));
        snprintf(payload, sizeof(payload), "{\"channelNumber\":\"%s\"}", esc);
        request(m.host, "tv/openChannel", payload);
    } else if (plg_prefix(cmd, "toast:")) {
        plg_json_escape(cmd + 6, esc, sizeof(esc));
        snprintf(payload, sizeof(payload), "{\"message\":\"%s\"}", esc);
        request(m.host, "system.notifications/createToast", payload);
    } else if (plg_prefix(cmd, "ssap:")) {
        /* ssap:<uri> [<JSON payload>], e.g. ssap:audio/setVolume {"volume":12} */
        char uri[256];
        const char *p = cmd + 5;
        while (*p == '/') p++;
        size_t ul = strcspn(p, " ");
        snprintf(uri, sizeof(uri), "%.*s", (int)(ul < sizeof(uri) ? ul : sizeof(uri) - 1), p);
        request(m.host, uri, p[ul] ? p + ul + 1 : NULL);
    } else {
        printf("[-] " ID ": unknown command '%s'\n", cmd);
    }
}

int main(int argc, char **argv) {
    char path[128];
    int fd = plg_open(argc, argv, ID, path, sizeof(path));
    if (fd < 0) return 1;
    s_data = plg_data_dir(ID);
    char buf[4096];
    while (plg_running) {
        struct pollfd pfd[3] = { { .fd = fd, .events = POLLIN }, { .fd = s_ws.fd, .events = POLLIN },
                                 { .fd = s_pws.fd, .events = POLLIN } };
        int pending = ws_pending(&s_ws) || ws_pending(&s_pws);
        int pr = pending ? 1 : poll(pfd, 3, s_pair.host[0] ? 100 : 1000);
        if (pr < 0 && errno != EINTR) break;
        if (s_ws.fd >= 0 && (ws_pending(&s_ws) || (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)))) pump(&s_ws, 0, 1);
        if (s_pws.fd >= 0 && (ws_pending(&s_pws) || (pfd[2].revents & (POLLIN | POLLHUP | POLLERR)))) pump(&s_pws, 0, 1);
        if (s_pair.host[0] && plg_mono_ms() > s_pair.until_ms) pair_end(0, "No answer on the TV: choose Accept when it asks, then pair again");
        if (pr > 0 && (pfd[0].revents & POLLIN) && plg_recv(fd, buf, sizeof(buf), 0) > 0) handle(buf);
    }
    tv_close();
    close(fd);
    unlink(path);
    printf("[*] " ID ": stopped\n");
    return 0;
}

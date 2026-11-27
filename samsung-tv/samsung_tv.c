/* Samsung Smart TV (Tizen, 2016 and later) over the TV's remote control WebSocket:
 * wss://<tv>:8002 with a token the TV gives once the user allows the Hub on the TV screen
 * (2018 and later), or ws://<tv>:8001 without one (2016, 2017). Keys (tap and hold), app
 * launch, text, power state over http://<tv>:8001/api/v2/, Wake-on-LAN when the TV is off the
 * network. One poll() loop, no threads. Protocol as in samsungtvws (MIT). */
#include "plugin.h"
#include "tls.h"
#include "ws.h"
#include "wol.h"

#define ID "samsung-tv"
#define CONNECT_MS 2000
#define READY_WAIT_MS 2500  /* a connect waits this long for the TV's "connect" event */
#ifndef IDLE_PING_MS
#define IDLE_PING_MS 20000  /* a send after this long without a frame checks the link first */
#endif
/* The name the TV shows in its list of allowed devices: the Hub's name ("hub_name" in every
 * datagram, the Hub Name in its settings), this on firmware that sends none */
#define DEFAULT_HUB_NAME "Harmony-NG Hub"
static char s_hub_name[64] = DEFAULT_HUB_NAME;

static tls_conn_t s_tls;
static ws_t s_ws = { .fd = -1 };
static struct {
    char host[64];
    int port;
    int ready;           /* the TV sent ms.channel.connect: it takes commands */
} s_tv;
static struct { char host[64]; uint64_t until_ms; } s_pair; /* a pairing waiting for Allow */
static struct { char key[48]; uint64_t last_ms; } s_held;   /* key down on the TV (Press sent) */
static const char *s_data;

static void path_for(const char *kind, const char *host, char *out, size_t sz) {
    snprintf(out, sz, "%s/%s-%s", s_data, kind, host);
}

static void tv_close(void) {
    ws_close(&s_ws);
    s_tv.ready = 0;
    s_tv.host[0] = '\0';
}

static void pair_end(int ok, const char *msg) {
    plg_status(ID, ok, "start_pair", ok ? "done" : "error", msg, "");
    printf("[%c] " ID ": %s\n", ok ? '+' : '-', msg);
    s_pair.host[0] = '\0';
}

/* Model year from the TV's info page ("model": "19_MUSEM_QTV" = 2019), 0 when unknown */
static int model_year(const char *host) {
    static char body[4096];
    char model[64] = "";
    if (plg_http(host, 8001, "GET", "/api/v2/", "", NULL, body, sizeof(body), 1500) == 200) {
        plg_json_str(body, "model", model, sizeof(model));
    }
    if (model[0] < '0' || model[0] > '9' || model[1] < '0' || model[1] > '9' || model[2] != '_') return 0;
    return 2000 + (model[0] - '0') * 10 + (model[1] - '0');
}

/* The TV allowed the Hub. TVs from 2018 on ignore the HDMI keys over the network: the pairing
 * answer ticks "HDMI via IR" for them (the device's HDMI commands then go out as infrared from
 * the Hub), and leaves it off for older ones; the user saves the device and may change it. */
static void pair_done(const char *host) {
    int year = model_year(host);
    char msg[160];
    if (year >= 2018) {
        snprintf(msg, sizeof(msg), "Paired: the TV allowed the Hub. A %d model: HDMI inputs go out as infrared (HDMI via IR)", year);
        plg_status(ID, 1, "start_pair", "done", msg, "\"hdmi_via_ir\": true");
    } else if (year > 0) {
        snprintf(msg, sizeof(msg), "Paired: the TV allowed the Hub (a %d model)", year);
        plg_status(ID, 1, "start_pair", "done", msg, "\"hdmi_via_ir\": false");
    } else {
        snprintf(msg, sizeof(msg), "Paired: the TV allowed the Hub");
        plg_status(ID, 1, "start_pair", "done", msg, "");
    }
    printf("[+] " ID ": %s\n", msg);
    s_pair.host[0] = '\0';
}

/* The name as the TV wants it in the query: base64, URL-encoded (as samsungtvws sends it) */
static void client_name_query(char *out, size_t out_sz) {
    char b64[100];
    plg_base64((const unsigned char *)s_hub_name, strlen(s_hub_name), b64, sizeof(b64));
    size_t o = 0;
    for (const char *c = b64; *c && o + 4 < out_sz; c++) {
        if (*c == '+' || *c == '/' || *c == '=') o += (size_t)snprintf(out + o, out_sz - o, "%%%02X", (unsigned char)*c);
        else out[o++] = *c;
    }
    out[o] = '\0';
}

/* The Hub's name of a datagram. A new one closes the connection: the next one registers under it
 * (the TV sees a new client, asks once, then keeps its token). */
static void take_hub_name(const char *buf) {
    char name[64];
    plg_json_str(buf, "hub_name", name, sizeof(name));
    if (!name[0]) snprintf(name, sizeof(name), "%s", DEFAULT_HUB_NAME);
    if (strcmp(name, s_hub_name) == 0) return;
    snprintf(s_hub_name, sizeof(s_hub_name), "%s", name);
    if (s_ws.fd >= 0) {
        printf("[*] " ID ": the Hub is now \"%s\": the TV will ask to allow it once\n", s_hub_name);
        tv_close();
    }
}

/* The token the TV issued to this connection: data.token. Its connect event lists the clients it
 * knows first, each with its own token ("clients":[{"attributes":{"name":...,"token":...}},...]),
 * so the first "token" in the message is another client's: the list is skipped. Empty when the
 * TV issued none (it accepted the token the plugin sent). */
static void issued_token(const char *msg, char *out, size_t out_sz) {
    const char *p = strstr(msg, "\"clients\":");
    if (p) p = strchr(p, '[');
    if (!p) {
        plg_json_str(msg, "token", out, out_sz);
        return;
    }
    int depth = 0, in_str = 0;
    for (; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_str = 0;
        } else if (*p == '"') {
            in_str = 1;
        } else if (*p == '[' || *p == '{') {
            depth++;
        } else if ((*p == ']' || *p == '}') && --depth == 0) {
            p++;
            break;
        }
    }
    plg_json_str(p, "token", out, out_sz);
}

/* An event from the TV */
static void on_msg(const char *msg) {
    char event[64];
    plg_json_str(msg, "event", event, sizeof(event));
    if (strcmp(event, "ms.channel.connect") == 0) {
        s_tv.ready = 1;
        char token[64], path[200];
        issued_token(msg, token, sizeof(token));
        if (token[0]) {
            path_for("token", s_tv.host, path, sizeof(path));
            if (plg_write_file(path, token, 0600) == 0) printf("[*] " ID ": token of %s saved\n", s_tv.host);
        }
        printf("[+] " ID ": connected to %s:%d\n", s_tv.host, s_tv.port);
        wol_learn(s_data, s_tv.host);
        if (s_pair.host[0] && strcmp(s_pair.host, s_tv.host) == 0) pair_done(s_tv.host);
    } else if (strcmp(event, "ms.channel.unauthorized") == 0 || strcmp(event, "ms.channel.timeOut") == 0) {
        printf("[-] " ID ": %s refused the Hub (%s): allow it on the TV, or in its Device Connection Manager\n",
               s_tv.host, event);
        if (s_pair.host[0]) pair_end(0, "The TV refused the Hub (Deny on the TV, or no answer)");
        tv_close();
    }
}

/* Reads every frame waiting (socket and TLS buffer) */
static void pump(int wait_ms) {
    static char msg[8192];
    uint64_t end = plg_mono_ms() + (uint64_t)wait_ms;
    do {
        struct pollfd pfd = { .fd = s_ws.fd, .events = POLLIN };
        int left = (int)(end - plg_mono_ms());
        if (s_ws.fd < 0) return;
        if (!ws_pending(&s_ws) && poll(&pfd, 1, left > 0 ? left : 0) <= 0) {
            if (wait_ms) continue;
            return;
        }
        if (ws_recv(&s_ws, msg, sizeof(msg)) < 0) {
            printf("[*] " ID ": the TV closed the connection\n");
            tv_close();
            return;
        }
        on_msg(msg);
    } while (!s_tv.ready && plg_mono_ms() < end);
}

/* Connection to host (TLS on 8002, plain on 8001), with the saved token. 0 when the TV takes
 * commands; -1 when it is off the network, refuses, or still shows its Allow prompt (the
 * connection stays open for the answer). */
static int tv_connect(const char *host, int port) {
    if (s_ws.fd >= 0 && strcmp(s_tv.host, host) == 0 && s_tv.port == port) {
        if (s_tv.ready && ws_alive(&s_ws, IDLE_PING_MS, 1000, on_msg)) return 0;
        if (!s_tv.ready && s_ws.fd >= 0) return -1; /* the Allow prompt is still up */
    }
    tv_close();
    char token[80] = "", path[512], pin[200];
    path_for("token", host, pin, sizeof(pin));
    plg_read_file(pin, token, sizeof(token));
    int secure = port != 8001;
    char name_q[160];
    client_name_query(name_q, sizeof(name_q));
    snprintf(path, sizeof(path), "/api/v2/channels/samsung.remote.control?name=%s%s%s", name_q,
             secure && token[0] ? "&token=" : "", secure ? token : "");
    path_for("tv", host, pin, sizeof(pin));
    strncat(pin, ".pin", sizeof(pin) - strlen(pin) - 1);
    snprintf(s_tv.host, sizeof(s_tv.host), "%s", host);
    s_tv.port = port;
    if (ws_connect(&s_ws, host, port, path, secure ? &s_tls : NULL, pin, CONNECT_MS) != 0) {
        s_tv.host[0] = '\0';
        return -1;
    }
    pump(READY_WAIT_MS);
    if (s_tv.ready) return 0;
    if (s_ws.fd >= 0) printf("[*] " ID ": %s shows its Allow prompt: accept the Hub on the TV\n", host);
    return -1;
}

static int send_json(const char *host, int port, const char *json) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (tv_connect(host, port) != 0) return -1;
        if (ws_send_text(&s_ws, json) == 0) return 0;
        tv_close(); /* the write failed: one new connection */
    }
    return -1;
}

static int send_key(const char *host, int port, const char *cmd, const char *key) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"method\":\"ms.remote.control\",\"params\":{\"Cmd\":\"%s\",\"DataOfCmd\":\"%s\","
             "\"Option\":\"false\",\"TypeOfRemote\":\"SendRemoteKey\"}}", cmd, key);
    return send_json(host, port, json);
}

static void release_held(void) {
    if (!s_held.key[0]) return;
    send_key(s_tv.host, s_tv.port, "Release", s_held.key);
    s_held.key[0] = '\0';
}

typedef struct { const char *name, *key; } map_t;
static const map_t s_keys[] = {
    {"Up", "KEY_UP"}, {"DirectionUp", "KEY_UP"}, {"Down", "KEY_DOWN"}, {"DirectionDown", "KEY_DOWN"},
    {"Left", "KEY_LEFT"}, {"DirectionLeft", "KEY_LEFT"}, {"Right", "KEY_RIGHT"}, {"DirectionRight", "KEY_RIGHT"},
    {"Select", "KEY_ENTER"}, {"OK", "KEY_ENTER"}, {"Enter", "KEY_ENTER"}, {"Back", "KEY_RETURN"},
    {"Return", "KEY_RETURN"}, {"Exit", "KEY_EXIT"}, {"Home", "KEY_HOME"}, {"Menu", "KEY_MENU"},
    {"Settings", "KEY_MENU"}, {"Tools", "KEY_TOOLS"}, {"Options", "KEY_TOOLS"}, {"Info", "KEY_INFO"},
    {"Guide", "KEY_GUIDE"}, {"Captions", "KEY_CAPTION"}, {"Subtitles", "KEY_CAPTION"},
    {"VolumeUp", "KEY_VOLUP"}, {"VolumeDown", "KEY_VOLDOWN"}, {"VolumeMute", "KEY_MUTE"}, {"Mute", "KEY_MUTE"},
    {"ChannelUp", "KEY_CHUP"}, {"ChannelDown", "KEY_CHDOWN"}, {"LastChannel", "KEY_PRECH"},
    {"ChannelList", "KEY_CH_LIST"}, {"Input", "KEY_SOURCE"}, {"Source", "KEY_SOURCE"},
    {"HDMI", "KEY_HDMI"}, {"HDMI 1", "KEY_HDMI1"}, {"HDMI 2", "KEY_HDMI2"}, {"HDMI 3", "KEY_HDMI3"},
    {"HDMI 4", "KEY_HDMI4"}, {"TV", "KEY_TV"}, {"Play", "KEY_PLAY"}, {"Pause", "KEY_PAUSE"},
    {"PlayPause", "KEY_PLAY_BACK"}, {"Stop", "KEY_STOP"}, {"FastForward", "KEY_FF"}, {"Fwd", "KEY_FF"},
    {"Rewind", "KEY_REWIND"}, {"Rev", "KEY_REWIND"}, {"Record", "KEY_REC"}, {"Next", "KEY_FF"},
    {"Previous", "KEY_REWIND"}, {"Red", "KEY_RED"}, {"Green", "KEY_GREEN"}, {"Yellow", "KEY_YELLOW"},
    {"Blue", "KEY_CYAN"}, {"PowerToggle", "KEY_POWER"}, {"Power", "KEY_POWER"}, {"Ambient", "KEY_AMBIENT"},
    {"PictureMode", "KEY_PMODE"}, {"AspectRatio", "KEY_ASPECT"}, {"Sleep", "KEY_SLEEP"},
    {"0", "KEY_0"}, {"1", "KEY_1"}, {"2", "KEY_2"}, {"3", "KEY_3"}, {"4", "KEY_4"},
    {"5", "KEY_5"}, {"6", "KEY_6"}, {"7", "KEY_7"}, {"8", "KEY_8"}, {"9", "KEY_9"},
    {NULL, NULL}
};

/* Samsung app ids (the TV's apps API) */
static const map_t s_apps[] = {
    {"youtube", "111299001912"}, {"netflix", "3201907018807"}, {"prime", "3201910019365"},
    {"primevideo", "3201910019365"}, {"disney", "3201901017640"}, {"disney+", "3201901017640"},
    {"spotify", "3201606009684"}, {"plex", "3201512006963"}, {"appletv", "3201807016597"},
    {"apple", "3201807016597"}, {"max", "3202301029760"}, {"twitch", "3202203026841"},
    {"browser", "org.tizen.browser"}, {NULL, NULL}
};

static const char *lookup(const map_t *m, const char *name) {
    for (; m->name; m++) if (plg_streq(m->name, name)) return m->key;
    return NULL;
}

/* "KEY_..." passes through; else a friendly name, NULL when unknown */
static const char *map_key(const char *cmd) {
    if (plg_prefix(cmd, "KEY_")) return cmd;
    return lookup(s_keys, cmd);
}

/* "on", "standby", or "" when the TV does not say (before 2018) or does not answer */
static void power_state(const char *host, char *out, size_t sz) {
    static char body[4096];
    out[0] = '\0';
    if (plg_http(host, 8001, "GET", "/api/v2/", "", NULL, body, sizeof(body), 1500) == 200) {
        plg_json_str(body, "PowerState", out, sz);
    }
}

static void do_power(const char *host, int port, const char *cmd, const char *mac) {
    char st[16];
    if (plg_streq(cmd, "PowerOn")) {
        power_state(host, st, sizeof(st));
        if (strcmp(st, "on") == 0) return;
        if (strcmp(st, "standby") == 0) { /* the TV answers in standby: its power key wakes it */
            send_key(host, port, "Click", "KEY_POWER");
            return;
        }
        if (tv_connect(host, port) == 0) return; /* on, an older model that does not report */
        if (wol_wake(s_data, host, mac, "", 0) != 0) {
            printf("[-] " ID ": %s is off and its MAC is not known: set the MAC Address, or turn the TV on once "
                   "while the plugin runs\n", host);
        }
    } else { /* PowerOff: the power key only while the TV is on (it toggles) */
        power_state(host, st, sizeof(st));
        if (strcmp(st, "standby") == 0) return;
        send_key(host, port, "Click", "KEY_POWER");
    }
}

static void launch_app(const char *host, int port, const char *app) {
    const char *id = lookup(s_apps, app);
    char esc[160], json[400];
    plg_json_escape(id ? id : app, esc, sizeof(esc));
    snprintf(json, sizeof(json),
             "{\"method\":\"ms.channel.emit\",\"params\":{\"event\":\"ed.apps.launch\",\"to\":\"host\","
             "\"data\":{\"appId\":\"%s\",\"action_type\":\"%s\"}}}",
             esc, strchr(esc, '.') ? "NATIVE_LAUNCH" : "DEEP_LINK");
    if (send_json(host, port, json) != 0) printf("[-] " ID ": app '%s' not launched\n", app);
}

static void send_text(const char *host, int port, const char *text) {
    char b64[700], json[900];
    plg_base64((const unsigned char *)text, strlen(text), b64, sizeof(b64));
    snprintf(json, sizeof(json),
             "{\"method\":\"ms.remote.control\",\"params\":{\"Cmd\":\"%s\",\"DataOfCmd\":\"base64\","
             "\"TypeOfRemote\":\"SendInputString\"}}", b64);
    if (send_json(host, port, json) != 0) printf("[-] " ID ": text not sent\n");
}

static void handle(const char *buf) {
    plg_msg_t m;
    plg_parse(buf, &m);
    take_hub_name(buf);
    int port = (int)plg_json_int(buf, "port", 8002);
    if (port <= 0 || port > 65535) port = 8002;

    if (plg_streq(m.action, "start_pair")) {
        struct in_addr a;
        if (plg_resolve_ipv4(m.host, &a) != 0) {
            plg_status(ID, 0, "start_pair", "error", "Enter the TV's address (IPv4 or a host name the Hub can resolve)", "");
            return;
        }
        long t = plg_json_int(buf, "timeout_s", 45);
        snprintf(s_pair.host, sizeof(s_pair.host), "%s", m.host);
        s_pair.until_ms = plg_mono_ms() + (uint64_t)(t > 5 ? t - 2 : 3) * 1000;
        tv_close(); /* a new connection: the TV shows the prompt to a client that has no token */
        if (tv_connect(m.host, port) != 0 && s_ws.fd < 0) pair_end(0, "The TV does not answer: turn it on and check the address");
        return;
    }
    if (m.stale || !m.command[0] || !m.host[0]) return;
    const char *cmd = m.command;

    if (plg_streq(cmd, "Connect") || plg_streq(cmd, "Disconnect")) {
        if (!plg_once(&m)) return;
        if (plg_streq(cmd, "Disconnect")) {
            release_held();
            if (s_ws.fd >= 0) printf("[*] " ID ": disconnected from %s\n", s_tv.host);
            tv_close();
        } else if (tv_connect(m.host, port) != 0) {
            printf("[-] " ID ": Connect to %s failed\n", m.host);
        }
        return;
    }
    if (plg_streq(cmd, "PowerOn") || plg_streq(cmd, "PowerOff") || plg_streq(cmd, "WakeOnLan")) {
        if (!plg_once(&m)) return;
        char mac[32];
        plg_json_str(buf, "mac", mac, sizeof(mac));
        release_held();
        if (plg_streq(cmd, "WakeOnLan")) wol_wake(s_data, m.host, mac, "", 0);
        else do_power(m.host, port, cmd, mac);
        return;
    }
    if (plg_prefix(cmd, "app:") || plg_prefix(cmd, "launch:") || plg_prefix(cmd, "text:")) {
        if (!plg_once(&m)) return;
        release_held();
        if (plg_prefix(cmd, "text:")) send_text(m.host, port, cmd + 5);
        else launch_app(m.host, port, strchr(cmd, ':') + 1);
        return;
    }

    const char *key = map_key(cmd);
    if (!key) {
        printf("[-] " ID ": unknown command '%s'\n", cmd);
        return;
    }
    int same = s_held.key[0] && strcmp(s_held.key, key) == 0 && strcmp(s_tv.host, m.host) == 0;
    if (m.is_release) {
        if (same) release_held(); /* else the hold deadline released it already */
        return;
    }
    if (m.is_repeat && same) { /* the TV repeats a held key itself */
        s_held.last_ms = plg_mono_ms();
        return;
    }
    release_held();
    if (m.is_press || m.is_repeat) { /* a repeat without a held key: its press was lost */
        if (send_key(m.host, port, "Press", key) == 0) {
            snprintf(s_held.key, sizeof(s_held.key), "%s", key);
            s_held.last_ms = plg_mono_ms();
        }
    } else if (send_key(m.host, port, "Click", key) != 0) {
        printf("[-] " ID ": %s not sent to %s\n", key, m.host);
    }
}

int main(int argc, char **argv) {
    char path[128];
    int fd = plg_open(argc, argv, ID, path, sizeof(path));
    if (fd < 0) return 1;
    s_data = plg_data_dir(ID);
    char buf[4096];
    while (plg_running) {
        struct pollfd pfd[2] = { { .fd = fd, .events = POLLIN }, { .fd = s_ws.fd, .events = POLLIN } };
        int busy = s_held.key[0] || s_pair.host[0];
        int pr = ws_pending(&s_ws) ? 1 : poll(pfd, s_ws.fd >= 0 ? 2 : 1, busy ? 100 : 1000);
        if (pr < 0 && errno != EINTR) break;
        if (s_ws.fd >= 0 && (ws_pending(&s_ws) || (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)))) pump(0);
        if (s_held.key[0] && plg_mono_ms() - s_held.last_ms > PLG_HOLD_DEADLINE_MS) {
            printf("[-] " ID ": %s had no repeat or release for %d ms, releasing it\n", s_held.key, PLG_HOLD_DEADLINE_MS);
            release_held();
        }
        if (s_pair.host[0] && plg_mono_ms() > s_pair.until_ms) pair_end(0, "No answer on the TV: press Allow when it asks, then pair again");
        if (pr > 0 && (pfd[0].revents & POLLIN) && plg_recv(fd, buf, sizeof(buf), 0) > 0) handle(buf);
    }
    release_held();
    tv_close();
    close(fd);
    unlink(path);
    printf("[*] " ID ": stopped\n");
    return 0;
}

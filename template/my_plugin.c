/* Template of a Harmony-NG plugin: copy this folder, rename it and ID, replace send_key().
 *
 * What the Hub sends: one JSON datagram per key event on the socket (argv[1]), with the
 * device's config fields. A remote button gives press, repeat(s) and release; the web UI and
 * MQTT give tap. This template shows the three kinds of command a plugin has:
 *   - one-shot (power, launch, text): act on tap or press only (plg_once);
 *   - repeating (volume, arrows): act on press and on each repeat;
 *   - held on the device (a key down until release): see samsung-tv for the hold deadline.
 * And a pairing (manifest "pairing"), answered with plg_status().
 *
 * Rules of the Hub (docs/PLUGINS.md in harmony-ng): stay in the foreground, one poll() loop,
 * keep the receive loop short (bounded network timeouts), state that must survive an update in
 * plg_data_dir(). */
#include "plugin.h"
#include "wol.h"

#define ID "my-plugin"
#define HTTP_MS 1500

static const char *s_data;

/* The device's API: here one HTTP POST per key (http://<host>:<port>/key/<name>) */
static int send_key(const char *host, int port, const char *key) {
    char path[300];
    snprintf(path, sizeof(path), "/key/%s", key);
    int code = plg_http(host, port, "POST", path, "", "", NULL, 0, HTTP_MS);
    if (code != 200) printf("[-] " ID ": %s answered %d to %s\n", host, code, path);
    else wol_learn(s_data, host); /* the device answers: remember its MAC for PowerOn */
    return code == 200 ? 0 : -1;
}

static int repeats(const char *cmd) {
    return plg_streq(cmd, "VolumeUp") || plg_streq(cmd, "VolumeDown") || plg_streq(cmd, "Up") ||
           plg_streq(cmd, "Down") || plg_streq(cmd, "Left") || plg_streq(cmd, "Right");
}

static void handle(const char *buf) {
    plg_msg_t m;
    plg_parse(buf, &m);
    int port = (int)plg_json_int(buf, "port", 8080);

    if (plg_streq(m.action, "start_pair")) {
        /* the manifest's pairing button: check the device, report what was found */
        int ok = plg_http(m.host, port, "GET", "/", "", NULL, NULL, 0, HTTP_MS) > 0;
        plg_status(ID, ok, "start_pair", ok ? "done" : "error", ok ? "Device found" : "No answer from the device", "");
        return;
    }
    if (m.stale || !m.command[0] || !m.host[0]) return;

    if (plg_streq(m.command, "PowerOn")) {
        if (!plg_once(&m)) return;
        char mac[32];
        plg_json_str(buf, "mac", mac, sizeof(mac));
        if (send_key(m.host, port, "PowerOn") != 0) wol_wake(s_data, m.host, mac, "", 0);
        return;
    }
    if (m.is_release || (m.is_repeat && !repeats(m.command))) return;
    send_key(m.host, port, m.command);
}

int main(int argc, char **argv) {
    char path[128];
    int fd = plg_open(argc, argv, ID, path, sizeof(path));
    if (fd < 0) return 1;
    s_data = plg_data_dir(ID);
    char buf[4096];
    while (plg_running) {
        int r = plg_recv(fd, buf, sizeof(buf), 1000); /* the timeout: room for timers */
        if (r < 0) break;
        if (r > 0) handle(buf);
    }
    close(fd);
    unlink(path);
    printf("[*] " ID ": stopped\n");
    return 0;
}

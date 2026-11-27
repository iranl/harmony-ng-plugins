/* Wake-on-LAN plugin: wakes a PC, NAS, TV or console with the magic packet.
 *  - a "Wake-on-LAN" device: PowerOn (or Wake) sends to its "mac" config field;
 *  - any device: a command with keyCode PLUGIN:wol:<MAC> (or PLUGIN:wol:<MAC>@<broadcast>)
 *    wakes that MAC, no Wake-on-LAN device needed;
 *  - "Find MAC" (pairing): the device's IP address in, its MAC out (the Hub's ARP table; the
 *    device must be on). */
#include "plugin.h"
#include "wol.h"

#define ID "wol"
#define FIND_MS 3000

/* The running "Find MAC": its address and when it gives up */
static struct { char ip[16]; char host[64]; uint64_t until_ms, next_probe_ms; } s_find;

static void find_start(const char *ip) {
    char addr[16];
    /* a host name is resolved once here: the probes and ARP reads then use the address */
    if (plg_resolve_ipv4_text(ip, addr) != 0) {
        plg_status(ID, 0, "start_pair", "error", "Enter the device's address (e.g. 192.168.1.20, or a host name the Hub can resolve)", "");
        return;
    }
    snprintf(s_find.ip, sizeof(s_find.ip), "%s", addr);
    snprintf(s_find.host, sizeof(s_find.host), "%s", ip);
    s_find.until_ms = plg_mono_ms() + FIND_MS;
    s_find.next_probe_ms = 0;
    printf("[*] " ID ": looking up the MAC of %s\n", ip);
}

/* One step of a running Find: done, failed, or probe again */
static void find_step(void) {
    uint64_t now = plg_mono_ms();
    char mac[18];
    if (wol_arp_lookup(s_find.ip, mac) == 0) {
        char cfg[64];
        snprintf(cfg, sizeof(cfg), "\"mac\": \"%s\"", mac);
        char msg[128];
        snprintf(msg, sizeof(msg), "Found %s at %s", mac, s_find.host);
        plg_status(ID, 1, "start_pair", "done", msg, cfg);
        printf("[+] " ID ": %s\n", msg);
        s_find.ip[0] = '\0';
    } else if (now >= s_find.until_ms) {
        char msg[160];
        snprintf(msg, sizeof(msg), "No answer from %s: turn the device on, check the address, and try again", s_find.host);
        plg_status(ID, 0, "start_pair", "error", msg, "");
        printf("[-] " ID ": %s\n", msg);
        s_find.ip[0] = '\0';
    } else if (now >= s_find.next_probe_ms) {
        wol_arp_probe(s_find.ip);
        s_find.next_probe_ms = now + 500;
    }
}

/* "<MAC>" or "<MAC>@<broadcast>" as the command: 0 when it was one (and was sent) */
static int wake_inline(const char *cmd) {
    char mac_s[32], bcast[64] = "";
    snprintf(mac_s, sizeof(mac_s), "%s", cmd);
    char *at = strchr(mac_s, '@');
    if (at) {
        *at = '\0';
        snprintf(bcast, sizeof(bcast), "%s", strchr(cmd, '@') + 1);
    }
    unsigned char mac[6];
    if (wol_parse_mac(mac_s, mac) != 0) return -1;
    wol_wake("", "", mac_s, bcast, 0);
    return 0;
}

static void handle(const char *buf) {
    plg_msg_t m;
    plg_parse(buf, &m);
    if (plg_streq(m.action, "start_pair")) {
        find_start(m.host);
        return;
    }
    if (!plg_once(&m) || !m.command[0]) return;
    if (wake_inline(m.command) == 0) return;

    if (plg_streq(m.command, "PowerOn") || plg_streq(m.command, "Wake") || plg_streq(m.command, "WakeOnLan") ||
        plg_streq(m.command, "PowerToggle")) {
        char mac[32], bcast[64];
        plg_json_str(buf, "mac", mac, sizeof(mac));
        plg_json_str(buf, "broadcast", bcast, sizeof(bcast));
        if (!mac[0] && !m.host[0]) {
            printf("[-] " ID ": the device has no MAC address set\n");
            return;
        }
        wol_wake(plg_data_dir(ID), m.host, mac, bcast, (int)plg_json_int(buf, "port", 0));
        return;
    }
    printf("[-] " ID ": unknown command '%s' (PowerOn, or a MAC address)\n", m.command);
}

int main(int argc, char **argv) {
    char path[128];
    int fd = plg_open(argc, argv, ID, path, sizeof(path));
    if (fd < 0) return 1;
    char buf[4096];
    while (plg_running) {
        int r = plg_recv(fd, buf, sizeof(buf), s_find.ip[0] ? 100 : -1);
        if (r < 0) break;
        if (r > 0) handle(buf);
        if (s_find.ip[0]) find_step();
    }
    close(fd);
    unlink(path);
    printf("[*] " ID ": stopped\n");
    return 0;
}

/* Wake-on-LAN for the Harmony-NG plugins: the magic packet, MAC parsing, and the MAC of a LAN
 * address from the Hub's ARP table (a plugin learns the MAC of its device while it is on, and
 * can wake it later without the user typing it). Header only, needs plugin.h. */
#ifndef HNG_WOL_H
#define HNG_WOL_H

#include "plugin.h"

#define WOL_PORT 9

/* "AA:BB:CC:DD:EE:FF", "AA-BB-CC-DD-EE-FF" or "AABBCCDDEEFF" (any case). 0 when valid and not
 * all zero or broadcast. */
PLG_FN int wol_parse_mac(const char *s, unsigned char mac[6]) {
    if (!s) return -1;
    int any = 0, all_ff = 1;
    for (int i = 0; i < 6; i++) {
        int hi = plg_hex_nibble(s[0]), lo = hi < 0 ? -1 : plg_hex_nibble(s[1]);
        if (lo < 0) return -1;
        mac[i] = (unsigned char)(hi << 4 | lo);
        any |= mac[i];
        all_ff &= mac[i] == 0xFF;
        s += 2;
        if (i < 5 && (*s == ':' || *s == '-')) s++;
    }
    return *s || !any || all_ff ? -1 : 0;
}

PLG_FN void wol_format_mac(const unsigned char mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* The magic packet (6 x FF, then the MAC 16 times) as a UDP broadcast to bcast (NULL or "":
 * 255.255.255.255) on port (0: 9). Sent 3 times: a lone UDP packet gets lost, and a sleeping
 * NIC may miss the first one. 0 when sent. */
PLG_FN int wol_send(const unsigned char mac[6], const char *bcast, int port) {
    unsigned char pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) memcpy(pkt + 6 + i * 6, mac, 6);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)(port > 0 && port < 65536 ? port : WOL_PORT));
    if (plg_parse_ipv4(bcast && bcast[0] ? bcast : "255.255.255.255", &sin.sin_addr) != 0) return -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    int sent = 0;
    for (int i = 0; i < 3; i++) {
        if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&sin, sizeof(sin)) == (ssize_t)sizeof(pkt)) sent++;
        if (i < 2) usleep(20000);
    }
    close(fd);
    return sent ? 0 : -1;
}

/* The MAC the Hub's ARP table holds for host (an address or a host name; a complete entry),
 * "" when none */
PLG_FN int wol_arp_lookup(const char *host, char mac_out[18]) {
    mac_out[0] = '\0';
    char ip[16];
    if (plg_resolve_ipv4_text(host, ip) != 0) return -1;
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return -1;
    char line[256];
    /* "IP address  HW type  Flags  HW address  Mask  Device" */
    while (fgets(line, sizeof(line), f)) {
        char a[64], hw[32];
        unsigned type, flags;
        unsigned char mac[6];
        if (sscanf(line, "%63s %x %x %31s", a, &type, &flags, hw) == 4 && strcmp(a, ip) == 0 &&
            (flags & 0x2) && wol_parse_mac(hw, mac) == 0) {
            wol_format_mac(mac, mac_out);
            break;
        }
    }
    fclose(f);
    return mac_out[0] ? 0 : -1;
}

/* An ARP entry for host (an address or a host name): a UDP datagram to a closed port makes
 * the kernel resolve the address */
PLG_FN void wol_arp_probe(const char *host) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(9);
    if (plg_resolve_ipv4(host, &sin.sin_addr) != 0) return;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    sendto(fd, "", 0, 0, (struct sockaddr *)&sin, sizeof(sin));
    close(fd);
}

/* The MAC learned for a host as configured (address or name), in <data_dir>/mac-<host>: a
 * device set by name keeps its MAC when its address changes */
PLG_FN void wol_mac_path(const char *data_dir, const char *ip, char *out, size_t sz) {
    snprintf(out, sz, "%s/mac-%s", data_dir, ip);
}

/* Remembers the MAC of ip, an address or a host name (from the ARP table) while the device answers: call after a
 * successful connection. Written only when it changed. */
PLG_FN void wol_learn(const char *data_dir, const char *ip) {
    char mac[18], path[200];
    if (wol_arp_lookup(ip, mac) != 0) return;
    wol_mac_path(data_dir, ip, path, sizeof(path));
    char old[32];
    if (plg_read_file(path, old, sizeof(old)) >= 0 && strcmp(old, mac) == 0) return;
    if (plg_write_file(path, mac, 0644) == 0) printf("[*] learned MAC %s of %s for Wake-on-LAN\n", mac, ip);
}

/* Wakes the device at ip (an address or a host name): the configured MAC (config_mac, may be empty), else the one learned.
 * bcast and port as wol_send. 0 when a packet went out. */
PLG_FN int wol_wake(const char *data_dir, const char *ip, const char *config_mac, const char *bcast, int port) {
    unsigned char mac[6];
    char learned[32] = "", path[200];
    if (wol_parse_mac(config_mac, mac) != 0) {
        if (!ip || !ip[0]) return -1;
        wol_mac_path(data_dir, ip, path, sizeof(path));
        if (plg_read_file(path, learned, sizeof(learned)) < 0 && wol_arp_lookup(ip, learned) != 0) return -1;
        if (wol_parse_mac(learned, mac) != 0) return -1;
    }
    char s[18];
    wol_format_mac(mac, s);
    if (wol_send(mac, bcast, port) != 0) {
        printf("[-] Wake-on-LAN to %s not sent\n", s);
        return -1;
    }
    printf("[*] Wake-on-LAN sent to %s\n", s);
    return 0;
}

#endif

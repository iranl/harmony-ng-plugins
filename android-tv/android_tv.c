#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "bearssl.h"
#include "wol.h"

#define ATV_PORT_CONTROL 6466
#define ATV_PORT_PAIR    6467

/* The TV pings about every 5 s: a session with nothing from the TV for this long is dead (a
 * TV off the network still takes writes into the socket buffer, and the command is lost) */
#ifndef ATV_RX_STALE_MS
#define ATV_RX_STALE_MS       15000
#endif
/* After a failed connect, commands fail at once for this long instead of each waiting for
 * its own connect timeout */
#define ATV_CONNECT_BACKOFF_MS 3000
/* A held key with no repeat or release for this long is released (a lost release kept the
 * key down on the TV); the hub's IR hold deadline is the same */
#define ATV_HOLD_DEADLINE_MS   600
/* A repeat that waited this long in the queue is dropped: replayed late, it overshoots */
#define ATV_STALE_REPEAT_MS    1000

#ifndef ATV_DATA_DIR
#define ATV_DATA_DIR "/data/harmony/plugin-data/android-tv"
#endif

static volatile int s_running = 1;

static int s_debug = 0;

static void sig_handler(int sig) {
    (void)sig;
    s_running = 0;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* =========================================================================
 * Google TV Remote v2 (Android TV) Keycodes
 * Matching Home Assistant components/androidtv_remote and TvKeys
 * ========================================================================= */
#define KEYCODE_UNKNOWN              0
#define KEYCODE_HOME                 3
#define KEYCODE_BACK                 4
#define KEYCODE_0                    7
#define KEYCODE_1                    8
#define KEYCODE_2                    9
#define KEYCODE_3                   10
#define KEYCODE_4                   11
#define KEYCODE_5                   12
#define KEYCODE_6                   13
#define KEYCODE_7                   14
#define KEYCODE_8                   15
#define KEYCODE_9                   16
#define KEYCODE_DPAD_UP             19
#define KEYCODE_DPAD_DOWN           20
#define KEYCODE_DPAD_LEFT           21
#define KEYCODE_DPAD_RIGHT          22
#define KEYCODE_DPAD_CENTER         23
#define KEYCODE_VOLUME_UP           24
#define KEYCODE_VOLUME_DOWN         25
#define KEYCODE_POWER               26
#define KEYCODE_ENTER               66
#define KEYCODE_DEL                 67
#define KEYCODE_MENU                82
#define KEYCODE_NOTIFICATION       83
#define KEYCODE_SEARCH              84
#define KEYCODE_MEDIA_PLAY_PAUSE    85
#define KEYCODE_MEDIA_STOP          86
#define KEYCODE_MEDIA_NEXT          87
#define KEYCODE_MEDIA_PREVIOUS      88
#define KEYCODE_MEDIA_REWIND        89
#define KEYCODE_MEDIA_FAST_FORWARD  90
#define KEYCODE_MUTE                91
#define KEYCODE_VOLUME_MUTE        164
#define KEYCODE_CHANNEL_UP         166
#define KEYCODE_CHANNEL_DOWN       167
#define KEYCODE_MEDIA_PLAY         126
#define KEYCODE_MEDIA_PAUSE        127
#define KEYCODE_MEDIA_RECORD       130
#define KEYCODE_INFO               165
#define KEYCODE_TV                 170
#define KEYCODE_GUIDE              172
#define KEYCODE_DVR                173
#define KEYCODE_CAPTIONS           175
#define KEYCODE_SETTINGS           176
#define KEYCODE_TV_POWER           177
#define KEYCODE_TV_INPUT           178
#define KEYCODE_PROG_RED           183
#define KEYCODE_PROG_GREEN         184
#define KEYCODE_PROG_YELLOW        185
#define KEYCODE_PROG_BLUE          186
#define KEYCODE_APP_SWITCH         187
#define KEYCODE_ASSIST             219
#define KEYCODE_SLEEP              223
#define KEYCODE_WAKEUP             224
#define KEYCODE_PAIRING            225
#define KEYCODE_LAST_CHANNEL       229
#define KEYCODE_TV_INPUT_HDMI_1    243
#define KEYCODE_TV_INPUT_HDMI_2    244
#define KEYCODE_TV_INPUT_HDMI_3    245
#define KEYCODE_TV_INPUT_HDMI_4    246
#define KEYCODE_TV_INPUT_COMPOSITE_1 247
#define KEYCODE_TV_INPUT_COMPOSITE_2 248
#define KEYCODE_TV_INPUT_COMPONENT_1 249
#define KEYCODE_TV_INPUT_COMPONENT_2 250
#define KEYCODE_TV_INPUT_VGA_1     251

#define DIR_START_LONG 1
#define DIR_END_LONG   2
#define DIR_SHORT      3

typedef struct {
    const char *name;
    int keycode;
} key_mapping_t;

static const key_mapping_t s_key_map[] = {
    {"Up", KEYCODE_DPAD_UP},
    {"DirectionUp", KEYCODE_DPAD_UP},
    {"Down", KEYCODE_DPAD_DOWN},
    {"DirectionDown", KEYCODE_DPAD_DOWN},
    {"Left", KEYCODE_DPAD_LEFT},
    {"DirectionLeft", KEYCODE_DPAD_LEFT},
    {"Right", KEYCODE_DPAD_RIGHT},
    {"DirectionRight", KEYCODE_DPAD_RIGHT},
    {"Select", KEYCODE_DPAD_CENTER},
    {"OK", KEYCODE_DPAD_CENTER},
    {"Center", KEYCODE_DPAD_CENTER},
    {"Enter", KEYCODE_ENTER},
    {"Home", KEYCODE_HOME},
    {"Back", KEYCODE_BACK},
    {"Menu", KEYCODE_MENU},
    {"Power", KEYCODE_POWER},
    {"PowerOn", KEYCODE_WAKEUP},
    {"PowerOff", KEYCODE_SLEEP},
    {"Wakeup", KEYCODE_WAKEUP},
    {"Sleep", KEYCODE_SLEEP},
    {"PowerToggle", KEYCODE_POWER},
    {"TvPower", KEYCODE_TV_POWER},
    {"VolumeUp", KEYCODE_VOLUME_UP},
    {"VolumeDown", KEYCODE_VOLUME_DOWN},
    {"Mute", KEYCODE_VOLUME_MUTE},
    {"MuteToggle", KEYCODE_VOLUME_MUTE},
    {"VolumeMute", KEYCODE_VOLUME_MUTE},
    {"MicMute", KEYCODE_MUTE},
    {"Play", KEYCODE_MEDIA_PLAY},
    {"Pause", KEYCODE_MEDIA_PAUSE},
    {"PlayPause", KEYCODE_MEDIA_PLAY_PAUSE},
    {"Stop", KEYCODE_MEDIA_STOP},
    {"NextTrack", KEYCODE_MEDIA_NEXT},
    {"Next", KEYCODE_MEDIA_NEXT},
    {"SkipForward", KEYCODE_MEDIA_NEXT},
    {"PreviousTrack", KEYCODE_MEDIA_PREVIOUS},
    {"Previous", KEYCODE_MEDIA_PREVIOUS},
    {"Prev", KEYCODE_MEDIA_PREVIOUS},
    {"SkipBack", KEYCODE_MEDIA_PREVIOUS},
    {"Rewind", KEYCODE_MEDIA_REWIND},
    {"Rev", KEYCODE_MEDIA_REWIND},
    {"FastForward", KEYCODE_MEDIA_FAST_FORWARD},
    {"Fwd", KEYCODE_MEDIA_FAST_FORWARD},
    {"Record", KEYCODE_MEDIA_RECORD},
    {"ChannelUp", KEYCODE_CHANNEL_UP},
    {"ChannelDown", KEYCODE_CHANNEL_DOWN},
    {"PrevChannel", KEYCODE_LAST_CHANNEL},
    {"LastChannel", KEYCODE_LAST_CHANNEL},
    {"Search", KEYCODE_SEARCH},
    {"Assist", KEYCODE_ASSIST},
    {"Settings", KEYCODE_SETTINGS},
    {"Input", KEYCODE_TV_INPUT},
    {"Source", KEYCODE_TV_INPUT},
    {"InputNext", KEYCODE_TV_INPUT},
    {"HDMI", KEYCODE_TV_INPUT_HDMI_1},
    {"HDMI1", KEYCODE_TV_INPUT_HDMI_1},
    {"HDMI2", KEYCODE_TV_INPUT_HDMI_2},
    {"HDMI3", KEYCODE_TV_INPUT_HDMI_3},
    {"HDMI4", KEYCODE_TV_INPUT_HDMI_4},
    {"InputHdmi", KEYCODE_TV_INPUT_HDMI_1},
    {"InputHdmi1", KEYCODE_TV_INPUT_HDMI_1},
    {"InputHdmi2", KEYCODE_TV_INPUT_HDMI_2},
    {"InputHdmi3", KEYCODE_TV_INPUT_HDMI_3},
    {"InputHdmi4", KEYCODE_TV_INPUT_HDMI_4},
    {"InputHDMI", KEYCODE_TV_INPUT_HDMI_1},
    {"InputHDMI1", KEYCODE_TV_INPUT_HDMI_1},
    {"InputHDMI2", KEYCODE_TV_INPUT_HDMI_2},
    {"InputHDMI3", KEYCODE_TV_INPUT_HDMI_3},
    {"InputHDMI4", KEYCODE_TV_INPUT_HDMI_4},
    {"Composite", KEYCODE_TV_INPUT_COMPOSITE_1},
    {"Composite1", KEYCODE_TV_INPUT_COMPOSITE_1},
    {"InputComposite", KEYCODE_TV_INPUT_COMPOSITE_1},
    {"Component", KEYCODE_TV_INPUT_COMPONENT_1},
    {"Component1", KEYCODE_TV_INPUT_COMPONENT_1},
    {"InputComponent", KEYCODE_TV_INPUT_COMPONENT_1},
    {"VGA", KEYCODE_TV_INPUT_VGA_1},
    {"InputVGA", KEYCODE_TV_INPUT_VGA_1},
    {"TV", KEYCODE_TV},
    {"DVR", KEYCODE_DVR},
    {"Guide", KEYCODE_GUIDE},
    {"Info", KEYCODE_INFO},
    {"Captions", KEYCODE_CAPTIONS},
    {"Subtitles", KEYCODE_CAPTIONS},
    {"Red", KEYCODE_PROG_RED},
    {"Green", KEYCODE_PROG_GREEN},
    {"Yellow", KEYCODE_PROG_YELLOW},
    {"Blue", KEYCODE_PROG_BLUE},
    {"AppSwitcher", KEYCODE_APP_SWITCH},
    {"AppSwitch", KEYCODE_APP_SWITCH},
    {"0", KEYCODE_0},
    {"Number0", KEYCODE_0},
    {"1", KEYCODE_1},
    {"Number1", KEYCODE_1},
    {"2", KEYCODE_2},
    {"Number2", KEYCODE_2},
    {"3", KEYCODE_3},
    {"Number3", KEYCODE_3},
    {"4", KEYCODE_4},
    {"Number4", KEYCODE_4},
    {"5", KEYCODE_5},
    {"Number5", KEYCODE_5},
    {"6", KEYCODE_6},
    {"Number6", KEYCODE_6},
    {"7", KEYCODE_7},
    {"Number7", KEYCODE_7},
    {"8", KEYCODE_8},
    {"Number8", KEYCODE_8},
    {"9", KEYCODE_9},
    {"Number9", KEYCODE_9},
    {"Delete", KEYCODE_DEL},
    {"Backspace", KEYCODE_DEL},
    {"Notifications", KEYCODE_NOTIFICATION},
    /* Names the README documents (they were not mapped and were dropped as unknown) */
    {"Return", KEYCODE_BACK},
    {"VoiceAssist", KEYCODE_ASSIST},
    {"TvInput", KEYCODE_TV_INPUT},
    {"AllApps", 284},
    {"AudioTrack", 222},
    {"TopMenu", 226},
    {"SkipBackward", 273},
    {"Netflix", 289},
    {"YouTube", 290},
    {"PrimeVideo", 291},
    {"Prime", 291},
    {"Disney", 292},
    {"Disney+", 292},
    {"Button1", 188},
    {"Button2", 189},
    {"Button3", 190},
    {"Button4", 191},
    {"Pair", KEYCODE_PAIRING},
    {NULL, 0}
};

/* ASCII only, whatever the C locale */
static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* Names match without case, spaces, '_' or '-': "HDMI 1", "hdmi_1" and "HDMI1" are one key */
static int key_name_eq(const char *a, const char *b) {
    for (;;) {
        while (*a == ' ' || *a == '_' || *a == '-') a++;
        while (*b == ' ' || *b == '_' || *b == '-') b++;
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return 0;
        if (!*a) return 1;
        a++;
        b++;
    }
}

static int map_google_tv_keycode(const char *cmd) {
    if (!cmd || !cmd[0]) return KEYCODE_UNKNOWN;

    if (strncasecmp(cmd, "KEY_", 4) == 0) cmd += 4;

    for (int i = 0; s_key_map[i].name != NULL; i++) {
        if (key_name_eq(cmd, s_key_map[i].name)) {
            return s_key_map[i].keycode;
        }
    }

    /* Raw Android keycode: "keycode:NN" (a bare leading digit made "3D" a HOME key) */
    if (strncasecmp(cmd, "keycode:", 8) == 0 && cmd[8] >= '0' && cmd[8] <= '9') {
        return atoi(cmd + 8);
    }

    return KEYCODE_UNKNOWN;
}

/* =========================================================================
 * Protobuf Varint and Framing Utilities
 * ========================================================================= */
static size_t encode_varint(unsigned char *buf, uint64_t val) {
    size_t len = 0;
    while (val >= 0x80) {
        buf[len++] = (unsigned char)((val & 0x7F) | 0x80);
        val >>= 7;
    }
    buf[len++] = (unsigned char)(val & 0x7F);
    return len;
}

/* Build RemoteKeyInject (Field 10) */
static size_t build_remote_key_inject(unsigned char *out, size_t out_sz, int keycode, int direction) {
    unsigned char payload[32];
    size_t plen = 0;

    payload[plen++] = 0x08;
    plen += encode_varint(&payload[plen], (uint64_t)keycode);

    payload[plen++] = 0x10;
    plen += encode_varint(&payload[plen], (uint64_t)direction);

    size_t total = 0;
    out[total++] = 0x52;
    total += encode_varint(&out[total], plen);
    if (total + plen > out_sz) return 0;
    memcpy(&out[total], payload, plen);
    total += plen;
    return total;
}

/* Build RemoteAppLinkLaunchRequest (Field 90) */
static size_t build_remote_app_link(unsigned char *out, size_t out_sz, const char *app_link) {
    size_t link_len = strlen(app_link);
    unsigned char payload[512];
    size_t plen = 0;

    payload[plen++] = 0x0A;
    plen += encode_varint(&payload[plen], link_len);
    memcpy(&payload[plen], app_link, link_len);
    plen += link_len;

    size_t total = 0;
    out[total++] = 0xD2;
    out[total++] = 0x05;
    total += encode_varint(&out[total], plen);
    if (total + plen > out_sz) return 0;
    memcpy(&out[total], payload, plen);
    total += plen;
    return total;
}

/* Build RemoteImeBatchEdit (Field 21) for Text Input */
static size_t build_remote_ime_text(unsigned char *out, size_t out_sz, const char *text) {
    size_t tlen = strlen(text);
    unsigned char ime_obj[512];
    size_t obj_len = 0;

    ime_obj[obj_len++] = 0x08;
    obj_len += encode_varint(&ime_obj[obj_len], tlen);
    ime_obj[obj_len++] = 0x10;
    obj_len += encode_varint(&ime_obj[obj_len], tlen);
    ime_obj[obj_len++] = 0x1A;
    obj_len += encode_varint(&ime_obj[obj_len], tlen);
    memcpy(&ime_obj[obj_len], text, tlen);
    obj_len += tlen;

    unsigned char edit_info[512];
    size_t edit_len = 0;
    edit_info[edit_len++] = 0x08;
    edit_len += encode_varint(&edit_info[edit_len], 1);
    edit_info[edit_len++] = 0x12;
    edit_len += encode_varint(&edit_info[edit_len], obj_len);
    memcpy(&edit_info[edit_len], ime_obj, obj_len);
    edit_len += obj_len;

    unsigned char batch[1024];
    size_t batch_len = 0;
    batch[batch_len++] = 0x08;
    batch_len += encode_varint(&batch[batch_len], 1);
    batch[batch_len++] = 0x10;
    batch_len += encode_varint(&batch[batch_len], 1);
    batch[batch_len++] = 0x1A;
    batch_len += encode_varint(&batch[batch_len], edit_len);
    memcpy(&batch[batch_len], edit_info, edit_len);
    batch_len += edit_len;

    size_t total = 0;
    out[total++] = 0xAA;
    out[total++] = 0x01;
    total += encode_varint(&out[total], batch_len);
    if (total + batch_len > out_sz) return 0;
    memcpy(&out[total], batch, batch_len);
    total += batch_len;
    return total;
}

/* Build RemotePingResponse (Field 9) */
static size_t build_ping_response(unsigned char *out, size_t out_sz, uint64_t val1) {
    unsigned char inner[16];
    size_t ilen = 0;
    inner[ilen++] = 0x08;
    ilen += encode_varint(&inner[ilen], val1);

    unsigned char proto[32];
    size_t plen = 0;
    proto[plen++] = 0x4A;
    plen += encode_varint(&proto[plen], ilen);
    memcpy(&proto[plen], inner, ilen);
    plen += ilen;

    size_t flen = encode_varint(out, plen);
    if (flen + plen > out_sz) return 0;
    memcpy(out + flen, proto, plen);
    return flen + plen;
}

/* =========================================================================
 * Client identity: an RSA key and self-signed certificate made on this Hub
 * The key used to be compiled in and published, so every install shared it.
 * ========================================================================= */
#define ATV_KEY_BITS 2048

static unsigned char s_cert[1536];
static size_t s_cert_len = 0;
static unsigned char s_key[2048];
static size_t s_key_len = 0;

static size_t der_len_size(size_t len) {
    return len < 0x80 ? 1 : (len < 0x100 ? 2 : 3);
}

/* Writes tag + length + content at out; content may already sit at out + header size */
static size_t der_put(unsigned char *out, unsigned char tag, const void *content, size_t len) {
    size_t h = 1 + der_len_size(len);
    memmove(out + h, content, len);
    out[0] = tag;
    if (len < 0x80) {
        out[1] = (unsigned char)len;
    } else if (len < 0x100) {
        out[1] = 0x81;
        out[2] = (unsigned char)len;
    } else {
        out[1] = 0x82;
        out[2] = (unsigned char)(len >> 8);
        out[3] = (unsigned char)len;
    }
    return h + len;
}

/* DER INTEGER of an unsigned big-endian number */
static size_t der_uint(unsigned char *out, const unsigned char *num, size_t len) {
    while (len > 1 && num[0] == 0) {
        num++;
        len--;
    }
    unsigned char tmp[520];
    size_t n = 0;
    if (num[0] & 0x80) tmp[n++] = 0;
    memcpy(tmp + n, num, len);
    return der_put(out, 0x02, tmp, n + len);
}

static size_t der_name(unsigned char *out, const char *cn) {
    static const unsigned char oid_cn[] = { 0x06, 0x03, 0x55, 0x04, 0x03 };
    unsigned char atv[96];
    memcpy(atv, oid_cn, sizeof(oid_cn));
    size_t n = sizeof(oid_cn) + der_put(atv + sizeof(oid_cn), 0x0C, cn, strlen(cn));
    unsigned char seq[100];
    size_t sl = der_put(seq, 0x30, atv, n);
    unsigned char set[104];
    size_t tl = der_put(set, 0x31, seq, sl);
    return der_put(out, 0x30, set, tl);
}

/* Self-signed X.509 v3 certificate for pk, signed with sk (sha256WithRSAEncryption).
 * Returns its length, 0 on error. */
static size_t build_self_signed_cert(unsigned char *out, size_t out_sz, const br_rsa_private_key *sk,
                                     const br_rsa_public_key *pk, const unsigned char serial[8]) {
    static const unsigned char version[] = { 0xA0, 0x03, 0x02, 0x01, 0x02 };
    static const unsigned char alg_sig[] = { 0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                             0x0D, 0x01, 0x01, 0x0B, 0x05, 0x00 };
    static const unsigned char alg_rsa[] = { 0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                             0x0D, 0x01, 0x01, 0x01, 0x05, 0x00 };
    /* A fixed past start: the Hub clock may be unset at boot */
    static const char not_before[] = "200101000000Z";
    static const char not_after[] = "491231235959Z";

    unsigned char tbs[1100];
    size_t t = 0;
    memcpy(tbs + t, version, sizeof(version));
    t += sizeof(version);
    t += der_uint(tbs + t, serial, 8);
    memcpy(tbs + t, alg_sig, sizeof(alg_sig));
    t += sizeof(alg_sig);
    t += der_name(tbs + t, "atvremote");
    unsigned char val[40];
    size_t vl = der_put(val, 0x17, not_before, 13);
    vl += der_put(val + vl, 0x17, not_after, 13);
    t += der_put(tbs + t, 0x30, val, vl);
    t += der_name(tbs + t, "atvremote");

    unsigned char rsapub[540];
    size_t rl = der_uint(rsapub, pk->n, pk->nlen);
    rl += der_uint(rsapub + rl, pk->e, pk->elen);
    unsigned char bits[560];
    bits[0] = 0;
    size_t bl = 1 + der_put(bits + 1, 0x30, rsapub, rl);
    unsigned char spki[600];
    memcpy(spki, alg_rsa, sizeof(alg_rsa));
    size_t sl = sizeof(alg_rsa) + der_put(spki + sizeof(alg_rsa), 0x03, bits, bl);
    t += der_put(tbs + t, 0x30, spki, sl);

    unsigned char tbs_der[1110];
    size_t tbs_len = der_put(tbs_der, 0x30, tbs, t);

    unsigned char hash[32];
    br_sha256_context hc;
    br_sha256_init(&hc);
    br_sha256_update(&hc, tbs_der, tbs_len);
    br_sha256_out(&hc, hash);
    unsigned char sig[ATV_KEY_BITS / 8 + 1];
    sig[0] = 0;
    if (!br_rsa_i31_pkcs1_sign(BR_HASH_OID_SHA256, hash, sizeof(hash), sk, sig + 1)) return 0;

    unsigned char body[1500];
    size_t b = 0;
    memcpy(body, tbs_der, tbs_len);
    b += tbs_len;
    memcpy(body + b, alg_sig, sizeof(alg_sig));
    b += sizeof(alg_sig);
    b += der_put(body + b, 0x03, sig, sizeof(sig));
    if (b + 4 > out_sz) return 0;
    return der_put(out, 0x30, body, b);
}

static int read_urandom(void *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (unsigned char *)buf + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    return 0;
}

/* Makes a new key pair and certificate into s_key / s_cert. Returns 0 on success. */
static int generate_identity(void) {
    unsigned char seed[48];
    if (read_urandom(seed, sizeof(seed)) != 0) return -1;
    br_hmac_drbg_context rng;
    br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, sizeof(seed));

    static unsigned char kbuf_priv[BR_RSA_KBUF_PRIV_SIZE(ATV_KEY_BITS)];
    static unsigned char kbuf_pub[BR_RSA_KBUF_PUB_SIZE(ATV_KEY_BITS)];
    br_rsa_private_key sk;
    br_rsa_public_key pk;
    if (!br_rsa_i31_keygen(&rng.vtable, &sk, kbuf_priv, &pk, kbuf_pub, ATV_KEY_BITS, 65537)) return -1;

    unsigned char d[ATV_KEY_BITS / 8];
    size_t dlen = br_rsa_i31_compute_privexp(d, &sk, 65537);
    if (dlen == 0) return -1;
    if (br_encode_rsa_raw_der(NULL, &sk, &pk, d, dlen) > sizeof(s_key)) return -1;
    s_key_len = br_encode_rsa_raw_der(s_key, &sk, &pk, d, dlen);

    unsigned char serial[8];
    br_hmac_drbg_generate(&rng, serial, sizeof(serial));
    serial[0] = (unsigned char)((serial[0] & 0x7F) | 0x01);
    s_cert_len = build_self_signed_cert(s_cert, sizeof(s_cert), &sk, &pk, serial);
    memset(d, 0, sizeof(d));
    memset(kbuf_priv, 0, sizeof(kbuf_priv));
    return (s_key_len > 0 && s_cert_len > 0) ? 0 : -1;
}

static int read_blob(const char *path, unsigned char *buf, size_t cap, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (n == 0 || more) return -1;
    *len = n;
    return 0;
}

static int write_blob(const char *path, const unsigned char *buf, size_t len, mode_t mode) {
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    ssize_t w = write(fd, buf, len);
    fsync(fd);
    close(fd);
    if (w != (ssize_t)len || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void make_data_dir(void) {
    mkdir("/data/harmony", 0755);
    mkdir("/data/harmony/plugin-data", 0755);
    mkdir(ATV_DATA_DIR, 0700);
}

/* Loads this Hub's identity, or makes one on the first start (a minute or more on the Hub).
 * Returns 1 when a new identity was made (TVs paired before must pair again), 0 when loaded,
 * -1 on error. */
static int load_identity(void) {
    if (read_blob(ATV_DATA_DIR "/key.der", s_key, sizeof(s_key), &s_key_len) == 0 &&
        read_blob(ATV_DATA_DIR "/cert.der", s_cert, sizeof(s_cert), &s_cert_len) == 0) {
        return 0;
    }
    printf("[*] android-tv: generating this Hub's %d-bit client key (one time, may take minutes)...\n", ATV_KEY_BITS);
    if (generate_identity() != 0) {
        printf("[-] android-tv: key generation failed\n");
        return -1;
    }
    make_data_dir();
    if (write_blob(ATV_DATA_DIR "/key.der", s_key, s_key_len, 0600) != 0 ||
        write_blob(ATV_DATA_DIR "/cert.der", s_cert, s_cert_len, 0644) != 0) {
        printf("[-] android-tv: cannot save the client key to %s\n", ATV_DATA_DIR);
        return -1;
    }
    printf("[+] android-tv: client key saved\n");
    return 1;
}

/* =========================================================================
 * BearSSL Permissive X.509 Validator & TLS Engine
 * ========================================================================= */
typedef struct {
    const br_x509_class *vtable;
    br_x509_decoder_context dec;
    br_x509_pkey pkey;
    int first_cert;
    uint8_t rsa_n[512];
    size_t rsa_nlen;
    uint8_t rsa_e[16];
    size_t rsa_elen;
} permissive_x509_context;

static void px509_start_chain(const br_x509_class **ctx, const char *server_name) {
    permissive_x509_context *pc = (permissive_x509_context *)ctx;
    (void)server_name;
    pc->first_cert = 1;
}

static void px509_start_cert(const br_x509_class **ctx, uint32_t length) {
    permissive_x509_context *pc = (permissive_x509_context *)ctx;
    (void)length;
    if (pc->first_cert) {
        br_x509_decoder_init(&pc->dec, 0, 0);
    }
}

static void px509_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    permissive_x509_context *pc = (permissive_x509_context *)ctx;
    if (pc->first_cert) {
        br_x509_decoder_push(&pc->dec, buf, len);
    }
}

static void px509_end_cert(const br_x509_class **ctx) {
    permissive_x509_context *pc = (permissive_x509_context *)ctx;
    if (pc->first_cert) {
        if (br_x509_decoder_last_error(&pc->dec) == 0) {
            const br_x509_pkey *pk = br_x509_decoder_get_pkey(&pc->dec);
            if (pk) {
                pc->pkey = *pk;
                if (pk->key_type == BR_KEYTYPE_RSA) {
                    if (pk->key.rsa.n && pk->key.rsa.nlen > 0 && pk->key.rsa.nlen <= sizeof(pc->rsa_n)) {
                        memcpy(pc->rsa_n, pk->key.rsa.n, pk->key.rsa.nlen);
                        pc->rsa_nlen = pk->key.rsa.nlen;
                    }
                    if (pk->key.rsa.e && pk->key.rsa.elen > 0 && pk->key.rsa.elen <= sizeof(pc->rsa_e)) {
                        memcpy(pc->rsa_e, pk->key.rsa.e, pk->key.rsa.elen);
                        pc->rsa_elen = pk->key.rsa.elen;
                    }
                }
            }
        }
        pc->first_cert = 0;
    }
}

static unsigned px509_end_chain(const br_x509_class **ctx) {
    (void)ctx;
    return 0;
}

static const br_x509_pkey *px509_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    permissive_x509_context *pc = (permissive_x509_context *)ctx;
    if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return &pc->pkey;
}

static const br_x509_class px509_vtable = {
    sizeof(permissive_x509_context),
    px509_start_chain,
    px509_start_cert,
    px509_append,
    px509_end_cert,
    px509_end_chain,
    px509_get_pkey
};

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t rlen = recv(fd, buf, len, 0);
        if (rlen <= 0) {
            if (rlen < 0 && errno == EINTR) continue;
            return -1;
        }
        return (int)rlen;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t wlen = send(fd, buf, len, MSG_NOSIGNAL);
        if (wlen <= 0) {
            if (wlen < 0 && errno == EINTR) continue;
            return -1;
        }
        return (int)wlen;
    }
}

/* Returns the message length, 0 for a message larger than out (read and dropped: failing
 * left its bytes in the stream and the session lost sync), -1 on error */
static int read_delimited_msg(br_sslio_context *ioc, unsigned char *out, size_t out_max) {
    uint64_t msg_len = 0;
    int shift = 0;
    for (;;) {
        unsigned char b;
        if (br_sslio_read_all(ioc, &b, 1) != 0) return -1;
        msg_len |= (uint64_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
        if (shift > 35) return -1;
    }
    if (msg_len == 0) return 0;
    if (msg_len > out_max) {
        if (msg_len > 65536) return -1;
        unsigned char sink[256];
        size_t left = (size_t)msg_len;
        while (left > 0) {
            size_t n = left > sizeof(sink) ? sizeof(sink) : left;
            if (br_sslio_read_all(ioc, sink, n) != 0) return -1;
            left -= n;
        }
        printf("[-] android-tv: dropped a %u-byte message from the TV (buffer %u)\n",
               (unsigned)msg_len, (unsigned)out_max);
        return 0;
    }
    if (br_sslio_read_all(ioc, out, (size_t)msg_len) != 0) return -1;
    return (int)msg_len;
}

/* =========================================================================
 * Control Channel (Port 6466)
 * ========================================================================= */
static const uint8_t s_remote_configure[] = {
    0x40, 0x0a, 0x3e, 0x08, 0xee, 0x04, 0x12, 0x39,
    0x0a, 0x0b, 'H', 'a', 'r', 'm', 'o', 'n', 'y', ' ', 'H', 'u', 'b',
    0x12, 0x08, 'L', 'o', 'g', 'i', 't', 'e', 'c', 'h',
    0x18, 0x01,
    0x22, 0x01, '1',
    0x2a, 0x14, 'c', 'o', 'm', '.', 'l', 'o', 'g', 'i', 't', 'e', 'c', 'h', '.', 'h', 'a', 'r', 'm', 'o', 'n', 'y',
    0x32, 0x05, '1', '.', '0', '.', '0'
};

static const uint8_t s_remote_set_active[] = {
    0x05, 0x12, 0x03, 0x08, 0xee, 0x04
};

typedef struct {
    int fd;
    br_ssl_client_context sc;
    br_sslio_context ioc;
    permissive_x509_context px509;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    char host[64];
    int connected;
    uint64_t last_rx_ms;  /* last message from the TV */
} ctl_session_t;

static ctl_session_t s_ctl;

static void ctl_disconnect(void) {
    if (s_ctl.connected || s_ctl.fd >= 0) {
        br_ssl_engine_close(&s_ctl.sc.eng);
        if (s_ctl.fd >= 0) close(s_ctl.fd);
        s_ctl.fd = -1;
        s_ctl.connected = 0;
        s_ctl.host[0] = '\0';
    }
}

static void init_bearssl_client_trimmed(br_ssl_client_context *cc) {
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_RSA_WITH_AES_128_GCM_SHA256
    };
    br_ssl_client_zero(cc);
    br_ssl_engine_set_versions(&cc->eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&cc->eng, suites, (sizeof suites) / (sizeof suites[0]));
    br_ssl_client_set_default_rsapub(cc);
    br_ssl_engine_set_default_rsavrfy(&cc->eng);
    br_ssl_engine_set_ec(&cc->eng, &br_ec_prime_i31);
    br_ssl_engine_set_default_aes_gcm(&cc->eng);
    br_ssl_engine_set_hash(&cc->eng, br_sha1_ID, &br_sha1_vtable);
    br_ssl_engine_set_hash(&cc->eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_prf_sha256(&cc->eng, &br_tls12_sha256_prf);
}

/* The TV's key, pinned on the first control session after a pairing (trust on first use).
 * The certificate check accepts any TV certificate, so without the pin anyone on the LAN
 * answering on the TV's address got every command. */
static void tv_pin_path(const char *host, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/tv-%s.pin", ATV_DATA_DIR, host);
}

static int check_tv_pin(const char *host, const permissive_x509_context *px) {
    if (px->rsa_nlen == 0) return 0;  /* not an RSA key: nothing to pin */
    unsigned char hash[32];
    br_sha256_context hc;
    br_sha256_init(&hc);
    br_sha256_update(&hc, px->rsa_n, px->rsa_nlen);
    br_sha256_out(&hc, hash);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);

    char path[160];
    tv_pin_path(host, path, sizeof(path));
    char pinned[80] = {0};
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(pinned, sizeof(pinned), f)) pinned[0] = '\0';
        fclose(f);
        pinned[strcspn(pinned, "\r\n")] = '\0';
    }
    if (!pinned[0]) {
        make_data_dir();
        if (write_blob(path, (const unsigned char *)hex, 64, 0644) == 0) {
            printf("[*] android-tv: pinned the key of the TV at %s\n", host);
        }
        return 0;
    }
    if (strcmp(pinned, hex) != 0) {
        printf("[-] android-tv: the TV at %s shows a different key than when first connected: "
               "refusing the session (pair again to trust the new key)\n", host);
        return -1;
    }
    return 0;
}

static int ctl_connect_once(const char *host) {

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(ATV_PORT_CONTROL);
    if (inet_pton(AF_INET, host, &sin.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    /* Non-blocking connect with 1.5s timeout */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int cr = connect(fd, (struct sockaddr *)&sin, sizeof(sin));
    if (cr != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (cr != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            close(fd);
            return -1;
        }
        int so_err = 0;
        socklen_t slen = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen);
        if (so_err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);

    struct timeval rtv = { .tv_sec = 1, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    memset(&s_ctl.px509, 0, sizeof(s_ctl.px509));
    s_ctl.px509.vtable = &px509_vtable;

    br_x509_certificate chain[1];
    chain[0].data = s_cert;
    chain[0].data_len = s_cert_len;

    br_skey_decoder_context skdc;
    br_skey_decoder_init(&skdc);
    br_skey_decoder_push(&skdc, s_key, s_key_len);

    init_bearssl_client_trimmed(&s_ctl.sc);
    br_ssl_engine_set_x509(&s_ctl.sc.eng, &s_ctl.px509.vtable);
    br_ssl_engine_set_buffer(&s_ctl.sc.eng, s_ctl.iobuf, sizeof(s_ctl.iobuf), 1);

    if (br_skey_decoder_last_error(&skdc) == 0 && br_skey_decoder_key_type(&skdc) == BR_KEYTYPE_RSA) {
        const br_rsa_private_key *sk = br_skey_decoder_get_rsa(&skdc);
        br_ssl_client_set_single_rsa(&s_ctl.sc, chain, 1, sk, br_rsa_pkcs1_sign_get_default());
    }

    s_ctl.fd = fd;
    br_ssl_client_reset(&s_ctl.sc, host, 0);
    br_sslio_init(&s_ctl.ioc, &s_ctl.sc.eng, sock_read, &s_ctl.fd, sock_write, &s_ctl.fd);

    /* Handshake on 6466:
     * 1. Read RemoteConfigure from TV
     * 2. Send our RemoteConfigure
     * 3. Read RemoteSetActive from TV
     * 4. Send our RemoteSetActive
     */
    unsigned char tmp[2048];
    int r = read_delimited_msg(&s_ctl.ioc, tmp, sizeof(tmp));
    if (r < 0) {
        ctl_disconnect();
        return -1;
    }

    if (br_sslio_write_all(&s_ctl.ioc, s_remote_configure, sizeof(s_remote_configure)) != 0 ||
        br_sslio_flush(&s_ctl.ioc) != 0) {
        ctl_disconnect();
        return -1;
    }

    r = read_delimited_msg(&s_ctl.ioc, tmp, sizeof(tmp));
    if (r < 0) {
        ctl_disconnect();
        return -1;
    }

    if (br_sslio_write_all(&s_ctl.ioc, s_remote_set_active, sizeof(s_remote_set_active)) != 0 ||
        br_sslio_flush(&s_ctl.ioc) != 0) {
        ctl_disconnect();
        return -1;
    }

    if (check_tv_pin(host, &s_ctl.px509) != 0) {
        ctl_disconnect();
        return -1;
    }

    s_ctl.connected = 1;
    s_ctl.last_rx_ms = mono_ms();
    strncpy(s_ctl.host, host, sizeof(s_ctl.host) - 1);
    /* The TV's MAC, for Wake-on-LAN when it is off the network later */
    make_data_dir();
    wol_learn(ATV_DATA_DIR, host);
    printf("[+] android-tv: Control session connected to %s:%d\n", host, ATV_PORT_CONTROL);
    return 0;
}

static int ctl_connect(const char *host) {
    static uint64_t s_fail_ms = 0;
    static char s_fail_host[64] = {0};
    if (!host || !host[0]) return -1;
    if (s_ctl.connected && strcmp(s_ctl.host, host) == 0) return 0;
    ctl_disconnect();
    uint64_t now = mono_ms();
    if (s_fail_ms && strcmp(s_fail_host, host) == 0 && now - s_fail_ms < ATV_CONNECT_BACKOFF_MS) {
        return -1;
    }
    if (ctl_connect_once(host) == 0) {
        s_fail_ms = 0;
        return 0;
    }
    printf("[-] android-tv: cannot reach %s:%d (TV off the network, or not paired with this Hub)\n",
           host, ATV_PORT_CONTROL);
    s_fail_ms = mono_ms();
    snprintf(s_fail_host, sizeof(s_fail_host), "%s", host);
    return -1;
}

/* Handle incoming packets on s_ctl (e.g. RemotePingRequest) */
static int handle_ctl_read(void) {
    if (!s_ctl.connected || s_ctl.fd < 0) return -1;

    unsigned char buf[2048];
    int rlen = read_delimited_msg(&s_ctl.ioc, buf, sizeof(buf));
    if (rlen < 0) return -1;
    s_ctl.last_rx_ms = mono_ms();
    if (rlen == 0) return 0;

    /* Parse RemoteMessage fields */
    size_t i = 0;
    while (i < (size_t)rlen) {
        uint64_t tag = 0;
        int shift = 0;
        while (i < (size_t)rlen) {
            unsigned char b = buf[i++];
            tag |= (uint64_t)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (s_debug && field != 8) printf("[d] android-tv: TV message field %d (%d bytes)\n", field, rlen);
        /* RemoteError: the TV refused the last message. Start a clean session for the next one. */
        if (field == 3) {
            printf("[-] android-tv: TV reported an error for the last command, reconnecting\n");
            return -1;
        }

        if (wire == 0) {
            while (i < (size_t)rlen && (buf[i++] & 0x80) != 0);
        } else if (wire == 2) {
            uint64_t len = 0;
            shift = 0;
            while (i < (size_t)rlen) {
                unsigned char b = buf[i++];
                len |= (uint64_t)(b & 0x7F) << shift;
                if ((b & 0x80) == 0) break;
                shift += 7;
            }
            if (field == 8 && i + len <= (size_t)rlen) {
                /* RemotePingRequest (Field 8): parse val1 (Field 1, tag 0x08) */
                size_t p = i;
                uint64_t val1 = 0;
                while (p < i + len && (buf[p++] & 0x80) != 0);
                shift = 0;
                while (p < i + len) {
                    unsigned char b = buf[p++];
                    val1 |= (uint64_t)(b & 0x7F) << shift;
                    if ((b & 0x80) == 0) break;
                    shift += 7;
                }
                /* Echo back RemotePingResponse (Field 9) */
                unsigned char resp[32];
                size_t rlen_resp = build_ping_response(resp, sizeof(resp), val1);
                if (rlen_resp > 0) {
                    br_sslio_write_all(&s_ctl.ioc, resp, rlen_resp);
                    br_sslio_flush(&s_ctl.ioc);
                }
            }
            i += len;
        } else {
            break;
        }
    }
    return 0;
}

static int transmit_payload(const char *host, const unsigned char *payload, size_t payload_len) {
    if (!host || !host[0] || !payload || payload_len == 0) return -1;

    if (s_ctl.connected && mono_ms() - s_ctl.last_rx_ms > ATV_RX_STALE_MS) {
        printf("[-] android-tv: nothing from %s for %u ms, reconnecting before the send\n",
               s_ctl.host, (unsigned)(mono_ms() - s_ctl.last_rx_ms));
        ctl_disconnect();
    }

    unsigned char frame[2048];
    size_t hdr_len = encode_varint(frame, payload_len);
    if (hdr_len + payload_len > sizeof(frame)) return -1;
    memcpy(&frame[hdr_len], payload, payload_len);
    size_t frame_total = hdr_len + payload_len;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!s_ctl.connected || strcmp(s_ctl.host, host) != 0) {
            if (ctl_connect(host) != 0) return -1;
        }

        if (br_sslio_write_all(&s_ctl.ioc, frame, frame_total) == 0 &&
            br_sslio_flush(&s_ctl.ioc) == 0) {
            return 0;
        }

        printf("[-] android-tv: Control write failed, reconnecting...\n");
        ctl_disconnect();
    }
    return -1;
}

static int send_google_tv_key(const char *host, int keycode, int direction) {
    unsigned char buf[128];
    size_t len = build_remote_key_inject(buf, sizeof(buf), keycode, direction);
    if (len == 0) return -1;
    return transmit_payload(host, buf, len);
}

static int send_google_tv_text(const char *host, const char *text) {
    unsigned char buf[1024];
    size_t len = build_remote_ime_text(buf, sizeof(buf), text);
    if (len == 0) return -1;
    return transmit_payload(host, buf, len);
}

/* Short app aliases and their packages. A deep link (from the Home Assistant androidtv_remote
 * docs) is sent when the app has one: a Google Play change broke market://launch?id= for many
 * apps, and the TV answered it with an error (Netflix, Prime Video, Spotify, Kodi on a Google TV). */
typedef struct { const char *alias, *alias2, *pkg, *link; } app_alias_t;
static const app_alias_t s_app_aliases[] = {
    {"youtube", NULL, "com.google.android.youtube.tv", "https://www.youtube.com"},
    {"netflix", NULL, "com.netflix.ninja", "https://www.netflix.com/title"},
    {"prime", "primevideo", "com.amazon.amazonvideo.livingroom", "https://app.primevideo.com"},
    {"disney", "disney+", "com.disney.disneyplus", "https://www.disneyplus.com"},
    {"spotify", NULL, "com.spotify.tv.android", "spotify://"},
    {"plex", NULL, "com.plexapp.android", "plex://"},
    {"twitch", NULL, "tv.twitch.android.app", "twitch://home"},
    /* No deep link: these only start where market://launch still works */
    {"kodi", NULL, "org.xbmc.kodi", NULL},
    {"vlc", NULL, "org.videolan.vlc", NULL},
    {NULL, NULL, NULL, NULL}
};

static const app_alias_t *find_app_alias(const char *name) {
    for (int i = 0; s_app_aliases[i].alias; i++) {
        const app_alias_t *a = &s_app_aliases[i];
        if (strcasecmp(name, a->alias) == 0 || (a->alias2 && strcasecmp(name, a->alias2) == 0) ||
            strcasecmp(name, a->pkg) == 0) {
            return a;
        }
    }
    return NULL;
}

/* The link the TV resolves: URLs and other "scheme://" links pass through. An alias or a known
 * package goes out as its deep link. Any other package goes out as market://launch?id=<pkg> (as
 * androidtvremote2 sends it): an android-app://<pkg> link has no data, and the launcher
 * activity of the app does not resolve it. */
static int resolve_app_link(const char *link, char *out, size_t out_sz) {
    if (!link || !link[0]) return -1;
    int n;
    if (strncasecmp(link, "android-app://", 14) == 0 && link[14] && !strchr(link + 14, '/')) {
        link += 14;
    } else if (strstr(link, "://") != NULL) {
        n = snprintf(out, out_sz, "%s", link);
        return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
    }
    const app_alias_t *a = find_app_alias(link);
    if (a && a->link) n = snprintf(out, out_sz, "%s", a->link);
    else n = snprintf(out, out_sz, "market://launch?id=%s", a ? a->pkg : link);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

static int send_google_tv_app_link(const char *host, const char *link) {
    char full_link[384];
    if (resolve_app_link(link, full_link, sizeof(full_link)) != 0) return -1;

    unsigned char buf[1024];
    size_t len = build_remote_app_link(buf, sizeof(buf), full_link);
    if (len == 0) return -1;
    return transmit_payload(host, buf, len);
}

static int send_channel_digits(const char *host, const char *digits) {
    if (!digits) return -1;
    for (size_t i = 0; digits[i]; i++) {
        char d = digits[i];
        if (d >= '0' && d <= '9') {
            int kcode = KEYCODE_0 + (d - '0');
            send_google_tv_key(host, kcode, DIR_SHORT);
            usleep(100000);
        }
    }
    return 0;
}

/* =========================================================================
 * State persistence
 * ========================================================================= */
static char s_paired_host[64] = {0};
static int s_is_paired = 0;

static void load_state(void) {
    FILE *f = fopen(ATV_DATA_DIR "/state.txt", "r");
    if (!f) f = fopen("/data/harmony/plugins/android-tv/state.txt", "r");
    if (!f) f = fopen("/tmp/plugins/android-tv.state", "r");
    if (f) {
        if (fscanf(f, "%63s %d", s_paired_host, &s_is_paired) == 2) {
            printf("[*] android-tv: Loaded state: host=%s paired=%d\n", s_paired_host, s_is_paired);
        }
        fclose(f);
    }
}

static void save_state(const char *host, int paired) {
    strncpy(s_paired_host, host, sizeof(s_paired_host) - 1);
    s_is_paired = paired;
    make_data_dir();
    FILE *f = fopen(ATV_DATA_DIR "/state.txt", "w");
    if (!f) f = fopen("/tmp/plugins/android-tv.state", "w");
    if (f) {
        fprintf(f, "%s %d\n", s_paired_host, s_is_paired);
        fclose(f);
    }
}

static void write_status_file(const char *action, const char *stage, const char *msg, int ok, int paired, const char *host) {
    /* Written whole, then renamed: the Hub polls this file and read half-written ones */
    mkdir("/tmp/plugins", 0755);
    FILE *f = fopen("/tmp/plugins/android-tv.status.tmp", "w");
    if (!f) return;
    fprintf(f, "{\n"
               "  \"ok\": %s,\n"
               "  \"action\": \"%s\",\n"
               "  \"stage\": \"%s\",\n"
               "  \"message\": \"%s\",\n"
               "  \"paired\": %s,\n"
               "  \"host\": \"%s\",\n"
               "  \"port\": %d\n"
               "}\n",
               ok ? "true" : "false",
               action ? action : "",
               stage ? stage : "",
               msg ? msg : "",
               paired ? "true" : "false",
               host ? host : "",
               ATV_PORT_CONTROL);
    fclose(f);
    rename("/tmp/plugins/android-tv.status.tmp", "/tmp/plugins/android-tv.status");
}

/* =========================================================================
 * POLO Pairing Implementation (Single-Threaded, No Pthreads)
 * Wire format matches Google TV / Android TV Remote Protocol v2 (Port 6467)
 * ========================================================================= */
static const uint8_t s_polo_pairing_request[] = {
    0x1f,
    0x08, 0x02,
    0x10, 0xc8, 0x01,
    0x52, 0x18,
    0x0a, 0x09, 'a','t','v','r','e','m','o','t','e',
    0x12, 0x0b, 'H','a','r','m','o','n','y',' ','H','u','b'
};

static const uint8_t s_polo_options[] = {
    0x10,
    0x08, 0x02,
    0x10, 0xc8, 0x01,
    0xa2, 0x01, 0x08,
    0x0a, 0x04, 0x08, 0x03, 0x10, 0x06,
    0x18, 0x01
};

static const uint8_t s_polo_configuration[] = {
    0x10,
    0x08, 0x02,
    0x10, 0xc8, 0x01,
    0xf2, 0x01, 0x08,
    0x0a, 0x04, 0x08, 0x03, 0x10, 0x06,
    0x10, 0x01
};

typedef struct {
    int fd;
    br_ssl_client_context sc;
    br_sslio_context ioc;
    permissive_x509_context px509;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    char host[64];
    time_t started_at;
    int active;
} polo_session_t;

static polo_session_t s_polo;

static void polo_close(void) {
    if (s_polo.active || s_polo.fd >= 0) {
        br_ssl_engine_close(&s_polo.sc.eng);
        if (s_polo.fd >= 0) close(s_polo.fd);
        s_polo.fd = -1;
        s_polo.active = 0;
        s_polo.host[0] = '\0';
    }
}

static const uint8_t *strip_leading_zero(const uint8_t *data, size_t *len) {
    while (*len > 1 && *data == 0x00) {
        data++;
        (*len)--;
    }
    return data;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_polo_status_ok(const unsigned char *buf, size_t len) {
    if (len < 5) return 0;
    for (size_t i = 0; i + 2 < len; i++) {
        if (buf[i] == 0x10 && buf[i+1] == 0xC8 && buf[i+2] == 0x01) {
            return 1;
        }
    }
    return 0;
}

static void do_start_pair(const char *host) {
    polo_close();

    if (!host || !host[0]) {
        write_status_file("start", "error", "Host IP required", 0, 0, "");
        return;
    }

    printf("[*] android-tv: Initiating POLO pairing with %s:%d...\n", host, ATV_PORT_PAIR);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        write_status_file("start", "error", "Failed to create TCP socket", 0, 0, host);
        return;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(ATV_PORT_PAIR);
    if (inet_pton(AF_INET, host, &sin.sin_addr) <= 0) {
        write_status_file("start", "error", "Invalid IP address", 0, 0, host);
        close(fd);
        return;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int cr = connect(fd, (struct sockaddr *)&sin, sizeof(sin));
    if (cr != 0 && errno != EINPROGRESS) {
        write_status_file("start", "error", "Failed to connect to device", 0, 0, host);
        close(fd);
        return;
    }
    if (cr != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            write_status_file("start", "error", "Connection timed out", 0, 0, host);
            close(fd);
            return;
        }
        int so_err = 0;
        socklen_t slen = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen);
        if (so_err != 0) {
            write_status_file("start", "error", "Failed to connect to device", 0, 0, host);
            close(fd);
            return;
        }
    }
    fcntl(fd, F_SETFL, flags);

    struct timeval rtv = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    memset(&s_polo.px509, 0, sizeof(s_polo.px509));
    s_polo.px509.vtable = &px509_vtable;

    br_x509_certificate chain[1];
    chain[0].data = s_cert;
    chain[0].data_len = s_cert_len;

    br_skey_decoder_context skdc;
    br_skey_decoder_init(&skdc);
    br_skey_decoder_push(&skdc, s_key, s_key_len);

    init_bearssl_client_trimmed(&s_polo.sc);
    br_ssl_engine_set_x509(&s_polo.sc.eng, &s_polo.px509.vtable);
    br_ssl_engine_set_buffer(&s_polo.sc.eng, s_polo.iobuf, sizeof(s_polo.iobuf), 1);

    if (br_skey_decoder_last_error(&skdc) == 0 && br_skey_decoder_key_type(&skdc) == BR_KEYTYPE_RSA) {
        const br_rsa_private_key *sk = br_skey_decoder_get_rsa(&skdc);
        br_ssl_client_set_single_rsa(&s_polo.sc, chain, 1, sk, br_rsa_pkcs1_sign_get_default());
    }

    s_polo.fd = fd;
    br_ssl_client_reset(&s_polo.sc, host, 0);
    br_sslio_init(&s_polo.ioc, &s_polo.sc.eng, sock_read, &s_polo.fd, sock_write, &s_polo.fd);

    /* 1. PairingRequest */
    printf("[*] android-tv: Sending PairingRequest...\n");
    if (br_sslio_write_all(&s_polo.ioc, s_polo_pairing_request, sizeof(s_polo_pairing_request)) != 0 ||
        br_sslio_flush(&s_polo.ioc) != 0) {
        write_status_file("start", "error", "Failed to write PairingRequest", 0, 0, host);
        polo_close();
        return;
    }

    unsigned char msg_buf[512];
    int rlen = read_delimited_msg(&s_polo.ioc, msg_buf, sizeof(msg_buf));
    if (rlen <= 0 || !is_polo_status_ok(msg_buf, (size_t)rlen)) {
        write_status_file("start", "error", "PairingRequest rejected by device", 0, 0, host);
        polo_close();
        return;
    }

    /* 2. Options */
    printf("[*] android-tv: Sending Options...\n");
    if (br_sslio_write_all(&s_polo.ioc, s_polo_options, sizeof(s_polo_options)) != 0 ||
        br_sslio_flush(&s_polo.ioc) != 0) {
        write_status_file("start", "error", "Failed to write Options", 0, 0, host);
        polo_close();
        return;
    }

    rlen = read_delimited_msg(&s_polo.ioc, msg_buf, sizeof(msg_buf));
    if (rlen <= 0 || !is_polo_status_ok(msg_buf, (size_t)rlen)) {
        write_status_file("start", "error", "Options rejected by device", 0, 0, host);
        polo_close();
        return;
    }

    /* 3. Configuration */
    printf("[*] android-tv: Sending Configuration...\n");
    if (br_sslio_write_all(&s_polo.ioc, s_polo_configuration, sizeof(s_polo_configuration)) != 0 ||
        br_sslio_flush(&s_polo.ioc) != 0) {
        write_status_file("start", "error", "Failed to write Configuration", 0, 0, host);
        polo_close();
        return;
    }

    rlen = read_delimited_msg(&s_polo.ioc, msg_buf, sizeof(msg_buf));
    if (rlen <= 0 || !is_polo_status_ok(msg_buf, (size_t)rlen)) {
        write_status_file("start", "error", "Configuration rejected by device", 0, 0, host);
        polo_close();
        return;
    }

    printf("[+] android-tv: ConfigurationAck received! PIN should now be visible on TV screen.\n");
    s_polo.active = 1;
    s_polo.started_at = time(NULL);
    strncpy(s_polo.host, host, sizeof(s_polo.host) - 1);
    write_status_file("start", "need_pin", "PIN displayed on TV screen", 1, 0, host);
}

static void do_finish_pair(const char *pin_code) {
    if (!s_polo.active || s_polo.fd < 0) {
        printf("[-] android-tv: No pairing session active\n");
        write_status_file("pin", "error", "No pairing session active", 0, 0, s_paired_host);
        return;
    }

    if (time(NULL) - s_polo.started_at > 90) {
        printf("[-] android-tv: Pairing session expired\n");
        write_status_file("pin", "error", "Pairing session expired", 0, 0, s_polo.host);
        polo_close();
        return;
    }

    char clean_pin[32] = {0};
    size_t cpi = 0;
    for (size_t i = 0; pin_code[i] && cpi < sizeof(clean_pin) - 1; i++) {
        if (!strchr(" \t\r\n\v\f", pin_code[i])) {
            clean_pin[cpi++] = pin_code[i];
        }
    }
    clean_pin[cpi] = '\0';

    if (strlen(clean_pin) != 6) {
        printf("[-] android-tv: Invalid PIN length %zu\n", strlen(clean_pin));
        write_status_file("pin", "error", "PIN must be exactly 6 characters", 0, 0, s_polo.host);
        polo_close();
        return;
    }

    for (int i = 0; i < 6; i++) {
        if (hex_nibble(clean_pin[i]) < 0) {
            printf("[-] android-tv: Invalid non-hex character in PIN '%s'\n", clean_pin);
            write_status_file("pin", "error", "PIN must be hexadecimal", 0, 0, s_polo.host);
            polo_close();
            return;
        }
    }

    /* Decode client certificate public key */
    br_x509_decoder_context dec;
    br_x509_decoder_init(&dec, 0, 0);
    br_x509_decoder_push(&dec, s_cert, s_cert_len);
    const br_x509_pkey *cli_pk = br_x509_decoder_get_pkey(&dec);
    if (!cli_pk || cli_pk->key_type != BR_KEYTYPE_RSA) {
        write_status_file("pin", "error", "Client key is not RSA", 0, 0, s_polo.host);
        polo_close();
        return;
    }

    size_t cli_nlen = cli_pk->key.rsa.nlen;
    const uint8_t *cli_n = strip_leading_zero(cli_pk->key.rsa.n, &cli_nlen);
    size_t cli_elen = cli_pk->key.rsa.elen;
    const uint8_t *cli_e = strip_leading_zero(cli_pk->key.rsa.e, &cli_elen);

    size_t srv_nlen = s_polo.px509.rsa_nlen;
    const uint8_t *srv_n = strip_leading_zero(s_polo.px509.rsa_n, &srv_nlen);
    size_t srv_elen = s_polo.px509.rsa_elen;
    const uint8_t *srv_e = strip_leading_zero(s_polo.px509.rsa_e, &srv_elen);

    if (srv_nlen == 0 || srv_elen == 0) {
        write_status_file("pin", "error", "Server certificate key missing", 0, 0, s_polo.host);
        polo_close();
        return;
    }

    uint8_t pin_bytes[2];
    pin_bytes[0] = (uint8_t)((hex_nibble(clean_pin[2]) << 4) | hex_nibble(clean_pin[3]));
    pin_bytes[1] = (uint8_t)((hex_nibble(clean_pin[4]) << 4) | hex_nibble(clean_pin[5]));
    uint8_t expected_first = (uint8_t)((hex_nibble(clean_pin[0]) << 4) | hex_nibble(clean_pin[1]));

    br_sha256_context hc;
    br_sha256_init(&hc);
    br_sha256_update(&hc, cli_n, cli_nlen);
    br_sha256_update(&hc, cli_e, cli_elen);
    br_sha256_update(&hc, srv_n, srv_nlen);
    br_sha256_update(&hc, srv_e, srv_elen);
    br_sha256_update(&hc, pin_bytes, 2);
    uint8_t hash[32];
    br_sha256_out(&hc, hash);

    if (hash[0] != expected_first) {
        printf("[-] android-tv: Secret hash mismatch: got 0x%02X expected 0x%02X\n", hash[0], expected_first);
        write_status_file("pin", "error", "Incorrect PIN code entered", 0, 0, s_polo.host);
        polo_close();
        return;
    }

    uint8_t secret_frame[43];
    secret_frame[0] = 42;
    secret_frame[1] = 0x08;
    secret_frame[2] = 0x02;
    secret_frame[3] = 0x10;
    secret_frame[4] = 0xc8;
    secret_frame[5] = 0x01;
    secret_frame[6] = 0xc2;
    secret_frame[7] = 0x02;
    secret_frame[8] = 0x22;
    secret_frame[9] = 0x0a;
    secret_frame[10] = 0x20;
    memcpy(&secret_frame[11], hash, 32);

    printf("[*] android-tv: Sending SecretMessage with verified hash...\n");
    if (br_sslio_write_all(&s_polo.ioc, secret_frame, sizeof(secret_frame)) != 0 ||
        br_sslio_flush(&s_polo.ioc) != 0) {
        write_status_file("pin", "error", "Failed to transmit secret challenge", 0, 0, s_polo.host);
        polo_close();
        return;
    }

    unsigned char msg_buf[512];
    int rlen = read_delimited_msg(&s_polo.ioc, msg_buf, sizeof(msg_buf));
    if (rlen <= 0 || !is_polo_status_ok(msg_buf, (size_t)rlen)) {
        write_status_file("pin", "error", "Device rejected PIN or Secret challenge", 0, 0, s_polo.host);
        polo_close();
        return;
    }

    char target_host[64];
    strncpy(target_host, s_polo.host, sizeof(target_host) - 1);
    target_host[sizeof(target_host) - 1] = '\0';
    polo_close();

    printf("[+] android-tv: Pairing successful with %s!\n", target_host);
    save_state(target_host, 1);
    /* A new pairing trusts the TV's key again: the next session pins it */
    char pin_path[160];
    tv_pin_path(target_host, pin_path, sizeof(pin_path));
    unlink(pin_path);
    write_status_file("pin", "done", "Pairing completed successfully", 1, 1, target_host);

    ctl_disconnect();
    ctl_connect(target_host);
}

/* =========================================================================
 * JSON Helpers
 * ========================================================================= */
/* Value of the first "key": in buf, as text. A string value is read to its closing quote with
 * its escapes undone: stopping at the first ',' cut URLs that hold one. */
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

/* =========================================================================
 * Main Daemon Loop
 * ========================================================================= */
int main(int argc, char **argv) {
    s_ctl.fd = -1;
    s_polo.fd = -1;
    (void)argc;
    (void)argv;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IONBF, 0);

    const char *sock_path = getenv("HARMONY_SOCKET");
    if (!sock_path || !sock_path[0]) {
        if (argc > 1 && argv[1] && argv[1][0]) sock_path = argv[1];
        else sock_path = "/tmp/plugins/android-tv.sock";
    }

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

    s_debug = getenv("ATV_DEBUG") != NULL;
    load_state();
    int ident = load_identity();
    if (ident < 0) {
        close(sfd);
        unlink(sock_path);
        return 1;
    }
    if (ident == 1 && s_is_paired) {
        printf("[*] android-tv: new client key, pair the TV again\n");
        save_state(s_paired_host, 0);
    }
    printf("[*] android-tv (Google TV Remote v2) daemon ready on %s\n", sock_path);

    /* Pre-connect to paired host if configured */
    if (s_is_paired && s_paired_host[0]) {
        ctl_connect(s_paired_host);
    }

    /* The key down on the TV (START_LONG sent, END_LONG not yet) */
    struct { int keycode; char host[64]; uint64_t last_ms; } held = { 0, "", 0 };

    char buf[4096];
    struct pollfd pfd[2];
    while (s_running) {
        int nfds = 1;
        pfd[0].fd = sfd;
        pfd[0].events = POLLIN;

        if (s_ctl.connected && s_ctl.fd >= 0) {
            pfd[1].fd = s_ctl.fd;
            pfd[1].events = POLLIN;
            nfds = 2;
        }

        int pr = poll(pfd, nfds, held.keycode ? 100 : 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (held.keycode && mono_ms() - held.last_ms > ATV_HOLD_DEADLINE_MS) {
            printf("[-] android-tv: key %d had no repeat or release for %d ms, releasing it\n",
                   held.keycode, ATV_HOLD_DEADLINE_MS);
            int k = held.keycode;
            held.keycode = 0;
            send_google_tv_key(held.host, k, DIR_END_LONG);
        }

        /* 1. Handle incoming message or disconnect from TV (pings, etc.) */
        if (nfds > 1 && (pfd[1].revents & (POLLIN | POLLERR | POLLHUP))) {
            /* Read every message BearSSL already holds: poll() sees only the socket, and a
             * ping left in the buffer went unanswered until the TV dropped the session */
            do {
                if (handle_ctl_read() != 0) {
                    printf("[*] android-tv: Control session closed, will reconnect on next command\n");
                    ctl_disconnect();
                    break;
                }
            } while (s_ctl.connected && (br_ssl_engine_current_state(&s_ctl.sc.eng) & BR_SSL_RECVAPP));
        }

        /* 2. Handle incoming datagram from Hub daemon */
        if (!(pfd[0].revents & POLLIN)) continue;

        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        char host[64] = {0};
        extract_json_str(buf, "host", host, sizeof(host));
        if (!host[0]) extract_json_str(buf, "ip_address", host, sizeof(host));
        if (!host[0] && s_paired_host[0]) strcpy(host, s_paired_host);

        char raw_cmd[256] = {0};
        extract_json_str(buf, "command", raw_cmd, sizeof(raw_cmd));

        char action[32] = "tap";
        extract_json_str(buf, "action", action, sizeof(action));
        if (!action[0]) strcpy(action, "tap");

        char pin[32] = {0};
        extract_json_str(buf, "pin", pin, sizeof(pin));

        char ts_str[24] = {0};
        extract_json_str(buf, "ts", ts_str, sizeof(ts_str));
        uint64_t ts = strtoull(ts_str, NULL, 10);
        if (ts && strcmp(action, "repeat") == 0 && mono_ms() > ts + ATV_STALE_REPEAT_MS) {
            if (s_debug) printf("[d] android-tv: dropped a repeat %u ms old\n", (unsigned)(mono_ms() - ts));
            continue;
        }

        /* 1. Pairing: PIN Verification / Finish */
        if (strcasecmp(action, "finish_pair") == 0 || strcasecmp(action, "pin") == 0) {
            printf("[*] android-tv: Received PIN '%s' for pairing...\n", pin);
            do_finish_pair(pin);
            continue;
        }

        /* 2. Pairing: Start */
        if (strcasecmp(action, "start_pair") == 0 || strcasecmp(action, "start") == 0 ||
            strcasecmp(raw_cmd, "Pair") == 0) {
            do_start_pair(host);
            continue;
        }

        if (!host[0]) continue;

        /* Home Assistant direction prefix: SHORT:, START_LONG:, END_LONG: */
        int direction = DIR_SHORT;
        if (strcmp(action, "press") == 0 || strcmp(action, "repeat") == 0) direction = DIR_START_LONG;
        else if (strcmp(action, "release") == 0) direction = DIR_END_LONG;

        const char *cmd = raw_cmd;
        if (strncasecmp(cmd, "SHORT:", 6) == 0) {
            direction = DIR_SHORT;
            cmd += 6;
        } else if (strncasecmp(cmd, "START_LONG:", 11) == 0) {
            direction = DIR_START_LONG;
            cmd += 11;
        } else if (strncasecmp(cmd, "END_LONG:", 9) == 0) {
            direction = DIR_END_LONG;
            cmd += 9;
        }

        /* App links, text and channel digits act once per button: on a remote they came on
         * press, on each repeat and on release, and the app was launched 2 or more times */
        int is_conn = strcasecmp(cmd, "Connect") == 0 || strcasecmp(cmd, "Disconnect") == 0;
        int is_wake = strcasecmp(cmd, "PowerOn") == 0 || strcasecmp(cmd, "WakeOnLan") == 0;
        int once_only = (is_conn || is_wake || strncasecmp(cmd, "text:", 5) == 0 || strncasecmp(cmd, "app:", 4) == 0 ||
                         strncasecmp(cmd, "launch:", 7) == 0 || strncasecmp(cmd, "channel:", 8) == 0 ||
                         strncasecmp(cmd, "http://", 7) == 0 || strncasecmp(cmd, "https://", 8) == 0);
        if (once_only && (strcmp(action, "repeat") == 0 || strcmp(action, "release") == 0)) continue;

        /* The control session alone, no key: Connect opens it (the next key goes out without a
         * handshake), Disconnect closes it until the next command */
        if (is_conn) {
            if (strcasecmp(cmd, "Disconnect") == 0) {
                if (s_ctl.connected) printf("[*] android-tv: Disconnected from %s\n", s_ctl.host);
                ctl_disconnect();
            } else if (ctl_connect(host) != 0) {
                printf("[-] android-tv: Connect to %s failed\n", host);
            }
            continue;
        }

        /* PowerOn: the wake key when the TV answers, else Wake-on-LAN to its MAC (the "mac"
         * config field, else the one learned while it was on). WakeOnLan: the packet only. */
        if (is_wake) {
            char mac[32] = {0};
            extract_json_str(buf, "mac", mac, sizeof(mac));
            if (strcasecmp(cmd, "PowerOn") == 0 && ctl_connect(host) == 0) {
                if (send_google_tv_key(host, KEYCODE_WAKEUP, DIR_SHORT) != 0) printf("[-] android-tv: wake key not sent\n");
            } else if (wol_wake(ATV_DATA_DIR, host, mac, "", 0) != 0) {
                printf("[-] android-tv: %s is off the network and its MAC is not known: set the device's "
                       "MAC Address, or turn the TV on once while the plugin runs\n", host);
            }
            continue;
        }

        /* Text Entry: text:<string> */
        if (strncasecmp(cmd, "text:", 5) == 0) {
            const char *text = cmd + 5;
            printf("[*] android-tv: Sending text '%s' to %s:%d\n", text, host, ATV_PORT_CONTROL);
            if (send_google_tv_text(host, text) != 0) printf("[-] android-tv: text not sent\n");
            continue;
        }

        /* App Launch / Deep link */
        if (strncasecmp(cmd, "app:", 4) == 0) {
            const char *app = cmd + 4;
            printf("[*] android-tv: Launching app '%s' on %s:%d\n", app, host, ATV_PORT_CONTROL);
            if (send_google_tv_app_link(host, app) != 0) printf("[-] android-tv: app launch not sent\n");
            continue;
        }
        if (strncasecmp(cmd, "launch:", 7) == 0) {
            const char *app = cmd + 7;
            printf("[*] android-tv: Launching app '%s' on %s:%d\n", app, host, ATV_PORT_CONTROL);
            if (send_google_tv_app_link(host, app) != 0) printf("[-] android-tv: app launch not sent\n");
            continue;
        }
        if (strncasecmp(cmd, "http://", 7) == 0 || strncasecmp(cmd, "https://", 8) == 0) {
            printf("[*] android-tv: Launching URL '%s' on %s:%d\n", cmd, host, ATV_PORT_CONTROL);
            if (send_google_tv_app_link(host, cmd) != 0) printf("[-] android-tv: URL not sent\n");
            continue;
        }

        /* Channel Digits: channel:<digits> */
        if (strncasecmp(cmd, "channel:", 8) == 0) {
            const char *ch = cmd + 8;
            printf("[*] android-tv: Switching channel '%s' on %s:%d\n", ch, host, ATV_PORT_CONTROL);
            send_channel_digits(host, ch);
            continue;
        }

        /* Keycode command */
        int kcode = map_google_tv_keycode(cmd);
        if (kcode == KEYCODE_UNKNOWN) {
            printf("[-] android-tv: Unknown/unsupported command '%s', ignoring\n", cmd);
            continue;
        }
        if (s_debug) printf("[d] android-tv: Keycode %d ('%s', dir=%d) -> %s:%d\n", kcode, cmd, direction, host, ATV_PORT_CONTROL);
        if (direction == DIR_START_LONG) {
            if (held.keycode && held.keycode != kcode) send_google_tv_key(held.host, held.keycode, DIR_END_LONG);
            held.keycode = kcode;
            snprintf(held.host, sizeof(held.host), "%s", host);
            held.last_ms = mono_ms();
        } else if (direction == DIR_END_LONG && held.keycode == kcode) {
            held.keycode = 0;
        }
        if (send_google_tv_key(host, kcode, direction) != 0) printf("[-] android-tv: key %d not sent\n", kcode);
    }

    polo_close();
    ctl_disconnect();
    close(sfd);
    unlink(sock_path);
    printf("[*] android-tv daemon stopped\n");
    return 0;
}

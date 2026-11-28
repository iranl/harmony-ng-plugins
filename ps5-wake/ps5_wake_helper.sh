#!/bin/sh
# Cold wake of a PlayStation over Bluetooth: the CSR8811 is restarted with the paired
# controller's BD_ADDR, the console is paged from that address, then the Hub's own BD_ADDR
# is restored. Usage: ps5_wake_helper.sh <host_mac> <controller_mac>

HOST_MAC="$1"
CTRL_MAC="$2"
LOCK=/tmp/hub_btstack.lock
SPOOF_PSR=/tmp/ps5_spoof.psr

is_mac() {
    echo "$1" | grep -q '^[0-9A-Fa-f][0-9A-Fa-f]\(:[0-9A-Fa-f][0-9A-Fa-f]\)\{5\}$'
}
if ! is_mac "$HOST_MAC" || ! is_mac "$CTRL_MAC"; then
    echo "[-] ps5-wake: usage: $0 <host_mac> <controller_mac>" >&2
    exit 1
fi

PSR_BASE="/tmp/pskeys.psr"
[ -f "$PSR_BASE" ] || PSR_BASE="/lib/firmware/csr8811-a08.psr"
[ -f "$PSR_BASE" ] || PSR_BASE="/lib/firmware/csr8811-a06.psr"
if [ ! -f "$PSR_BASE" ]; then
    echo "[-] ps5-wake: no CSR8811 PS key file, wake skipped" >&2
    exit 1
fi

# One radio task at a time. The lock is created only when absent (noclobber), and while it
# exists the init.sh supervisor does not restart hub_btstack.
if ! ( set -C; : > "$LOCK" ) 2>/dev/null; then
    echo "[-] ps5-wake: radio busy ($LOCK exists), wake skipped" >&2
    exit 1
fi

RADIO_TAKEN=0

# Load a PS key file and restart the CSR8811 with it. The keys go over HCI (bccmd -t hci, about
# 0.3 s) while hci0 is attached; the chip's warm reset ends that BCSP link, so hciattach starts
# a new one (it returns once hci0 is up). bccmd over the bare UART after killing hciattach
# (BCSP, 3-5 s) failed often on Linux 6.18 and left the chip unreachable until a reboot; it
# stays only for a radio with no hci0.
reload() {
    if /usr/sbin/hciconfig hci0 >/dev/null 2>&1; then
        /usr/sbin/bccmd -t hci -d hci0 psload -r "$1" >/dev/null || return 1
        killall -9 hciattach 2>/dev/null
        sleep 1
    else
        killall -9 hciattach 2>/dev/null
        sleep 1
        /usr/sbin/bccmd -t bcsp -d /dev/ttyS0 -b 115200 psload -r "$1" >/dev/null || return 1
    fi
    /usr/sbin/hciattach ttyS0 bcsp >/dev/null 2>&1
    /usr/sbin/hciconfig hci0 up 2>/dev/null
    /usr/sbin/hciconfig hci0 sspmode 1 2>/dev/null
}

# Runs on every exit once the lock is held: a step that failed used to leave hub_btstack
# stopped, the spoofed address on air and the supervisor paused until a reboot
cleanup() {
    if [ "$RADIO_TAKEN" = 1 ]; then
        reload "$PSR_BASE" || echo "[!] ps5-wake: restoring the Hub BD_ADDR failed" >&2
    fi
    rm -f "$SPOOF_PSR" "$LOCK"
    # The supervisor starts hub_btstack again now that the lock is gone
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP

# CSR PSKey &0x0001 mapping: m[3], m[4]m[5], m[2], m[0]m[1]
PSR_VAL=$(echo "$CTRL_MAC" | tr 'A-Z' 'a-z' | sed 's/\([0-9a-f]\{2\}\):\([0-9a-f]\{2\}\):\([0-9a-f]\{2\}\):\([0-9a-f]\{2\}\):\([0-9a-f]\{2\}\):\([0-9a-f]\{2\}\)/0x\4 0x\5\6 0x\3 0x\1\2/')
grep -v '&0x0001' "$PSR_BASE" > "$SPOOF_PSR"
echo "&0x0001 = $PSR_VAL" >> "$SPOOF_PSR"

# Stop hub_btstack (its HCI reset would abort the page)
RADIO_TAKEN=1
killall -9 hub_btstack 2>/dev/null

# Restart the CSR8811 with the controller's BD_ADDR
if ! reload "$SPOOF_PSR"; then
    echo "[-] ps5-wake: loading the controller BD_ADDR failed" >&2
    exit 1
fi

# Page the console with R2 page scan and role switch
BIN_DIR=$(cd "$(dirname "$0")" && pwd)
"$BIN_DIR/ps5_page" "$HOST_MAC" 2 10
RC=$?
echo "[*] ps5-wake: ps5_page exited with $RC"
sleep 1
exit "$RC"

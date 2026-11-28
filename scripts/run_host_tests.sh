#!/bin/bash
# Host tests of the plugins (x86 gcc, no Hub needed):
#  - device templates: checked against the Hub's limits, devices/index.json up to date
#  - gen_repository.py: the legacy release (last one for older Hubs)
#  - android-tv key names, app links and generated client certificate (test_android_tv.c)
#  - android-tv against a fake TV over TLS (test_fake_tv.py: holds, stale repeats, errors,
#    dead sessions)
# Needs gcc, python3, openssl, and BearSSL sources in BEARSSL_DIR. Host BearSSL is built once
# into $WORK/bearssl_host.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

echo "[*] device templates (devices/*.json and devices/index.json)"
python3 "$ROOT/scripts/device_templates.py" "$ROOT"
python3 "$ROOT/scripts/test_device_templates.py"
echo "[*] repository.json generator (last release for older Hubs)"
python3 "$ROOT/scripts/test_gen_repository.py"
: "${BEARSSL_DIR:?set BEARSSL_DIR to the BearSSL source tree}"
WORK="${WORK:-$ROOT/build/host_tests}"
LIB="$WORK/bearssl_host/libbearssl.a"
mkdir -p "$WORK"

if [ ! -f "$LIB" ]; then
    echo "[*] Building BearSSL for the host..."
    mkdir -p "$WORK/bearssl_host/o"
    (cd "$BEARSSL_DIR" && for f in $(find src -name '*.c'); do
        gcc -O2 -Iinc -Isrc -c "$f" -o "$WORK/bearssl_host/o/$(echo "$f" | tr / _).o"
    done)
    ar rcs "$LIB" "$WORK"/bearssl_host/o/*.o
fi

echo "[*] android-tv self-check"
gcc -Wall -Werror -Wno-unused-function -I"$BEARSSL_DIR/inc" -I"$ROOT/common" "$ROOT/android-tv/test_android_tv.c" "$LIB" -o "$WORK/atv_selfcheck"
"$WORK/atv_selfcheck"

echo "[*] android-tv fake TV"
E2E="$WORK/atv_e2e"
rm -rf "$E2E" && mkdir -p "$E2E"
gcc -Wall -Werror -DATV_RX_STALE_MS=2500 -DATV_DATA_DIR="\"$E2E/data\"" -I"$BEARSSL_DIR/inc" -I"$ROOT/common" \
    "$ROOT/android-tv/android_tv.c" "$LIB" -o "$E2E/android_tv"
timeout 180 python3 "$ROOT/android-tv/test_fake_tv.py" "$E2E/android_tv" "$E2E" || {
    echo "--- plugin log ---"; cat "$E2E/plugin.log"; exit 1; }

echo "[*] roku-ecp fake Roku"
gcc -Wall -Werror "$ROOT/roku-ecp/roku_ecp.c" -o "$WORK/roku_ecp"
timeout 60 python3 "$ROOT/roku-ecp/test_fake_roku.py" "$WORK/roku_ecp" "$WORK/roku_e2e" || {
    echo "--- plugin log ---"; cat "$WORK/roku_e2e/plugin.log"; exit 1; }

echo "[*] ps5-wake self-check"
gcc -Wall -Werror -Wno-unused-function "$ROOT/ps5-wake/test_ps5_wake.c" -o "$WORK/ps5_selfcheck"
"$WORK/ps5_selfcheck"
gcc -Wall -Werror "$ROOT/ps5-wake/ps5_page.c" -o "$WORK/ps5_page"
sh -n "$ROOT/ps5-wake/ps5_wake_helper.sh"

echo "[*] switch2-wake self-check"
gcc -Wall -Werror -Wno-unused-function "$ROOT/switch2-wake/test_switch2_wake.c" -o "$WORK/switch2_selfcheck"
"$WORK/switch2_selfcheck"
echo "[*] switch2-wake fake hub_btstack"
mkdir -p "$WORK/switch2_e2e"
gcc -Wall -Werror -DHUB_BTSTACK_SOCK="\"$WORK/switch2_e2e/bt.sock\"" "$ROOT/switch2-wake/switch2_wake.c" -o "$WORK/switch2_wake"
timeout 60 python3 "$ROOT/switch2-wake/test_fake_btstack.py" "$WORK/switch2_wake" "$WORK/switch2_e2e"

echo "[*] wol"
gcc -Wall -Werror -I"$ROOT/common" "$ROOT/wol/wol.c" -o "$WORK/wol"
timeout 60 python3 "$ROOT/wol/test_wol.py" "$WORK/wol" "$WORK/wol_e2e" || {
    echo "--- plugin log ---"; cat "$WORK/wol_e2e/plugin.log"; exit 1; }

# Plugins on common/ against fake devices (scripts/fake_ws.py): build_tv <dir> <source> <test> [cflags]
build_tv() {
    local d=$1 src=$2 t=$3
    shift 3
    echo "[*] $d against a fake device"
    gcc -Wall -Werror -I"$ROOT/common" -I"$BEARSSL_DIR/inc" "$@" "$ROOT/$d/$src" "$LIB" -o "$WORK/$d"
    rm -rf "$WORK/${d}_e2e"
    timeout 120 python3 "$ROOT/$d/$t" "$WORK/$d" "$WORK/${d}_e2e" || {
        echo "--- plugin log ---"; cat "$WORK/${d}_e2e/plugin.log"; exit 1; }
}
build_tv sony-bravia sony_bravia.c test_fake_bravia.py -DBRAVIA_PORT=18080
build_tv samsung-tv samsung_tv.c test_fake_samsung.py -DIDLE_PING_MS=1500
build_tv lg-webos lg_webos.c test_fake_lg.py -DIDLE_PING_MS=1500
build_tv template my_plugin.c test_my_plugin.py

echo "[+] Host tests passed"

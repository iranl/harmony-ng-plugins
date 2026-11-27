#!/bin/bash
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# Toolchain and BearSSL: TOOLCHAIN_BIN and BEARSSL_DIR when set (CI), else from the
# harmony-hub-control checkout next to this repo (or HARMONY_REPO_ROOT)
REPO_ROOT="${HARMONY_REPO_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)/harmony-hub-control}"
if [ -z "${TOOLCHAIN_BIN:-}" ]; then
    for t in "$REPO_ROOT"/build/toolchains/mips32--uclibc--stable*/bin; do
        [ -d "$t" ] && TOOLCHAIN_BIN="$t" && break
    done
fi
if [ -n "${TOOLCHAIN_BIN:-}" ] && [ -d "$TOOLCHAIN_BIN" ]; then
    export PATH="$TOOLCHAIN_BIN:$PATH"
fi

CC="mips-buildroot-linux-uclibc-gcc"
AR="mips-buildroot-linux-uclibc-ar"
STRIP="mips-buildroot-linux-uclibc-strip"
# Unused functions and data dropped at link time (BearSSL is large, the plugins use little of it)
CFLAGS="-Wall -Os -s -ffunction-sections -fdata-sections -Wl,--gc-sections"

PLUGINS_ROOT="$SCRIPT_DIR"
BEARSSL_DIR="${BEARSSL_DIR:-$REPO_ROOT/build/tmp/bearssl}"
if [ ! -d "$BEARSSL_DIR/src" ]; then
    echo "[!] BearSSL sources not found in $BEARSSL_DIR (set BEARSSL_DIR)" >&2
    exit 1
fi

echo "=== Building Harmony-NG Plugins for MIPS32 ==="
echo "Toolchain: $CC"
echo "Plugins directory: $PLUGINS_ROOT"

# 1. BearSSL static library (rebuilt when the stamp of its build flags differs)
BEARSSL_FLAGS="-Os -fPIC -ffunction-sections -fdata-sections"
if [ ! -f "$BEARSSL_DIR/libbearssl.a" ] || [ "$(cat "$BEARSSL_DIR/libbearssl.flags" 2>/dev/null)" != "$BEARSSL_FLAGS" ]; then
    echo "[*] Compiling BearSSL library for MIPS..."
    (
        cd "$BEARSSL_DIR"
        rm -f libbearssl.a ./*.o
        C_FILES=$(find src -name '*.c' ! -name 'brssl.c' ! -name 'test_*.c')
        $CC $BEARSSL_FLAGS -Iinc -Isrc -c $C_FILES
        $AR rcs libbearssl.a ./*.o
        rm -f ./*.o
        echo "$BEARSSL_FLAGS" > libbearssl.flags
        echo "[+] libbearssl.a built: $(ls -lh libbearssl.a | awk '{print $5}')"
    )
fi

# The Hub's /data/harmony/lib/libbearssl.so.0 (firmware with shared BearSSL): same commit, same
# exported names. The plugins link it instead of a static copy (android-tv: 36 KB, not 100 KB),
# and list it in "requires_libs". Only for linking: the archive ships no library.
BEARSSL_SO_DIR="$PLUGINS_ROOT/build/bearssl_so"
mkdir -p "$BEARSSL_SO_DIR"
{ echo "{ global:"; grep -v '^#' "$PLUGINS_ROOT/scripts/bearssl_exports.txt" | sed '/^$/d; s/$/;/'; echo "local: *; };"; } > "$BEARSSL_SO_DIR/exports.map"
$CC -shared -s -Wl,--gc-sections -Wl,--version-script="$BEARSSL_SO_DIR/exports.map" \
    -Wl,-soname,libbearssl.so.0 -o "$BEARSSL_SO_DIR/libbearssl.so.0" \
    -Wl,--whole-archive "$BEARSSL_DIR/libbearssl.a" -Wl,--no-whole-archive
BEARSSL_LINK="-I$BEARSSL_DIR/inc -L$BEARSSL_SO_DIR -l:libbearssl.so.0"

# 2. roku-ecp
echo "[*] Compiling roku-ecp..."
mkdir -p "$PLUGINS_ROOT/roku-ecp/bin"
$CC $CFLAGS "$PLUGINS_ROOT/roku-ecp/roku_ecp.c" -o "$PLUGINS_ROOT/roku-ecp/bin/roku_ecp"
$STRIP "$PLUGINS_ROOT/roku-ecp/bin/roku_ecp"
echo "[+] roku_ecp binary built: $(ls -lh "$PLUGINS_ROOT/roku-ecp/bin/roku_ecp" | awk '{print $5}')"

# 3. ps5-wake
echo "[*] Compiling ps5-wake..."
mkdir -p "$PLUGINS_ROOT/ps5-wake/bin"
$CC $CFLAGS "$PLUGINS_ROOT/ps5-wake/ps5_wake.c" -o "$PLUGINS_ROOT/ps5-wake/bin/ps5_wake"
$STRIP "$PLUGINS_ROOT/ps5-wake/bin/ps5_wake"
$CC $CFLAGS "$PLUGINS_ROOT/ps5-wake/ps5_page.c" -o "$PLUGINS_ROOT/ps5-wake/bin/ps5_page"
$STRIP "$PLUGINS_ROOT/ps5-wake/bin/ps5_page"
cp "$PLUGINS_ROOT/ps5-wake/ps5_wake_helper.sh" "$PLUGINS_ROOT/ps5-wake/bin/ps5_wake_helper.sh"
chmod +x "$PLUGINS_ROOT/ps5-wake/bin/ps5_wake_helper.sh"
echo "[+] ps5_wake binary built: $(ls -lh "$PLUGINS_ROOT/ps5-wake/bin/ps5_wake" | awk '{print $5}')"

# 4. switch2-wake
echo "[*] Compiling switch2-wake..."
mkdir -p "$PLUGINS_ROOT/switch2-wake/bin"
$CC $CFLAGS "$PLUGINS_ROOT/switch2-wake/switch2_wake.c" -o "$PLUGINS_ROOT/switch2-wake/bin/switch2_wake"
$STRIP "$PLUGINS_ROOT/switch2-wake/bin/switch2_wake"
echo "[+] switch2_wake binary built: $(ls -lh "$PLUGINS_ROOT/switch2-wake/bin/switch2_wake" | awk '{print $5}')"

# 5. Plugins on the shared headers in common/: build_c <dir> <source> <binary> [link flags]
build_c() {
    echo "[*] Compiling $1..."
    mkdir -p "$PLUGINS_ROOT/$1/bin"
    $CC $CFLAGS -I"$PLUGINS_ROOT/common" "$PLUGINS_ROOT/$1/$2" -o "$PLUGINS_ROOT/$1/bin/$3" ${4:-}
    $STRIP "$PLUGINS_ROOT/$1/bin/$3"
    echo "[+] $3 binary built: $(ls -lh "$PLUGINS_ROOT/$1/bin/$3" | awk '{print $5}')"
}
build_c wol wol.c wol
build_c sony-bravia sony_bravia.c sony_bravia
build_c samsung-tv samsung_tv.c samsung_tv "$BEARSSL_LINK"
build_c lg-webos lg_webos.c lg_webos "$BEARSSL_LINK"

# 6. android-tv
echo "[*] Compiling android-tv with the Hub's BearSSL..."
mkdir -p "$PLUGINS_ROOT/android-tv/bin"
$CC $CFLAGS -I"$PLUGINS_ROOT/common" "$PLUGINS_ROOT/android-tv/android_tv.c" -o "$PLUGINS_ROOT/android-tv/bin/android_tv" $BEARSSL_LINK
$STRIP "$PLUGINS_ROOT/android-tv/bin/android_tv"
echo "[+] android_tv binary built: $(ls -lh "$PLUGINS_ROOT/android-tv/bin/android_tv" | awk '{print $5}')"

# The plugins run on the Linux 6.18 Hub system (shared uClibc-ng 1.0.24, the toolchain's own C
# library): the stock uClibc 0.9.30 limits (no <ctype.h>, __ctype_*_loc) are gone
for b in roku-ecp/bin/roku_ecp ps5-wake/bin/ps5_wake ps5-wake/bin/ps5_page switch2-wake/bin/switch2_wake wol/bin/wol \
         sony-bravia/bin/sony_bravia samsung-tv/bin/samsung_tv lg-webos/bin/lg_webos android-tv/bin/android_tv; do
    # A library other than libc must be in the manifest's requires_libs: the Hub then refuses
    # the install on firmware without it, instead of a plugin that cannot start
    for lib in $(mips-buildroot-linux-uclibc-readelf -d "$PLUGINS_ROOT/$b" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
        [ "$lib" = "libc.so.0" ] && continue
        if ! python3 -c 'import json,sys; sys.exit(sys.argv[2] not in json.load(open(sys.argv[1], encoding="utf-8-sig")).get("requires_libs", []))' \
            "$PLUGINS_ROOT/${b%%/*}/manifest.json" "$lib"; then
            echo "[!] $b needs $lib, which ${b%%/*}/manifest.json does not list in requires_libs" >&2
            exit 1
        fi
    done
done

# 7. Package into dist/. The archives are reproducible (sorted names, no times or owners), so
# the same binaries always give the same SHA-256.
mkdir -p "$PLUGINS_ROOT/dist"
for p in roku-ecp ps5-wake switch2-wake wol sony-bravia samsung-tv lg-webos android-tv; do
    echo "[*] Packaging $p..."
    tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
        -cf - -C "$PLUGINS_ROOT/${p}" manifest.json bin/ | gzip -n -9 > "$PLUGINS_ROOT/dist/${p}-plugin.tar.gz"
    echo "[+] Created dist/${p}-plugin.tar.gz ($(ls -lh "$PLUGINS_ROOT/dist/${p}-plugin.tar.gz" | awk '{print $5}'))"
done

# 8. repository.json from the manifests, with the SHA-256 of each archive just built (for local
# testing: the release workflow rewrites it from the published release assets)
python3 "$PLUGINS_ROOT/scripts/gen_repository.py" "$PLUGINS_ROOT" "$PLUGINS_ROOT/dist"
echo "=== All plugins compiled and packaged successfully! ==="

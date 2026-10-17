#!/bin/bash
# Cross-build the Volumio plugin zip. The Pi never compiles anything.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CROSS="${CROSS:-/tmp/ampdac-cross}"
ZIG="${ZIG:-}"
if [ -z "$ZIG" ]; then
    ZIG="$(find "$CROSS" -maxdepth 2 -type f -name zig -executable | head -1)"
fi
if [ -z "$ZIG" ] || [ ! -x "$ZIG" ]; then
    echo "zig 0.14.1 is not unpacked in $CROSS" >&2
    exit 1
fi

LIBUSB_SRC="$CROSS/libusb-1.0.27"
if [ ! -d "$LIBUSB_SRC" ]; then
    echo "libusb sources missing at $LIBUSB_SRC" >&2
    exit 1
fi

CFG="$CROSS/libusb-config"
mkdir -p "$CFG"
cat > "$CFG/config.h" << 'EOF'
#define HAVE_SYS_TIME_H 1
#define HAVE_EVENTFD 1
#define HAVE_TIMERFD 1
#define HAVE_CLOCK_GETTIME 1
#define PLATFORM_POSIX 1
#define DEFAULT_VISIBILITY __attribute__((visibility("hidden")))
#define POLL_NFDS_TYPE nfds_t
EOF

LIBUSB_SRCS=(
    core.c descriptor.c hotplug.c io.c strerror.c sync.c
    os/linux_usbfs.c os/linux_netlink.c os/events_posix.c os/threads_posix.c
)

ENGINE_SRCS=(
    src/ampdac.cpp
    src/ampdac_android_log.cpp
    src/ampdac_kernel.cpp
    native/usb_session_transport.cpp
    native/usb_teardown.cpp
    native/uac2_descriptor_parser.cpp
    native/uac2_dsd_detector.cpp
    native/uac2_clock_control.cpp
    native/uac2_feature_volume.cpp
    native/usb_device_controller.cpp
    native/usb_iso_transfer_pool.cpp
    native/spsc_ring_buffer.cpp
    native/pcm_wire_formatter.cpp
    native/usb_playback_state.cpp
    native/usb_producer_coordinator.cpp
    native/dop_formatter.cpp
    native/native_dsd_formatter.cpp
    native/dsd_playback_manager.cpp
    native/libusb_event_thread.cpp
    native/decoder_to_ring_bridge.cpp
)

build_libusb() {
    local target="$1" out="$2"
    mkdir -p "$out"
    local src obj
    for src in "${LIBUSB_SRCS[@]}"; do
        obj="$out/$(echo "$src" | tr '/' '_').o"
        "$ZIG" cc -target "$target" -fPIC -O2 -std=gnu11 -D_GNU_SOURCE \
            -I"$CFG" -I"$LIBUSB_SRC/libusb" -I"$LIBUSB_SRC/libusb/os" \
            -c "$LIBUSB_SRC/libusb/$src" -o "$obj"
    done
    "$ZIG" ar rcs "$out/libusb.a" "$out"/*.o
}

# Hide libusb symbols inside libampdac.so; the public API stays default.
link_shared() {
    local target="$1"
    shift
    "$ZIG" c++ -target "$target" -shared -fPIC \
        -Wl,-soname,libampdac.so \
        -Wl,-Bstatic -lc++ -lc++abi -lunwind -Wl,-Bdynamic \
        -lpthread -lm -ldl \
        "$@"
}

build_arch() {
    local target="$1" name="$2" sysroot="$3" mcpu="${4:-}"
    local out="$CROSS/out/$name"
    local cpu=()
    if [ -n "$mcpu" ]; then
        cpu=(-mcpu="$mcpu")
    fi
    rm -rf "$out"
    mkdir -p "$out/obj"
    echo "== libusb $name =="
    build_libusb "$target" "$out/libusb"

    local cxx=("$ZIG" c++ -target "$target" "${cpu[@]}" -std=c++17 -fPIC -O2 \
        -Wall -Wextra -Werror=return-type -Wno-unused-parameter -D_GNU_SOURCE \
        -I"$ROOT/include" -I"$ROOT/compat" -I"$ROOT/third_party" -I"$ROOT/native")
    local src obj objs=()
    for src in "${ENGINE_SRCS[@]}"; do
        obj="$out/obj/$(basename "${src%.cpp}.o")"
        "${cxx[@]}" -c "$ROOT/$src" -o "$obj"
        objs+=("$obj")
    done
    echo "== libampdac.so $name =="
    link_shared "$target" "${cpu[@]}" -o "$out/libampdac.so" "${objs[@]}" "$out/libusb/libusb.a"

    echo "== ampdacd $name =="
    "${cxx[@]}" -I"$ROOT/include" -o "$out/ampdacd" "$ROOT/src/ampdacd.cpp" \
        -L"$out" -lampdac -Wl,-rpath,'$ORIGIN' -Wl,-rpath,'$ORIGIN/../lib' -pthread \
        -Wl,-Bstatic -lc++ -lc++abi -lunwind -Wl,-Bdynamic

    local alsa_inc="$sysroot/usr/include"
    local alsa_lib
    alsa_lib="$(find "$sysroot" -name 'libasound.so.2' | head -1)"
    echo "== ALSA plugin $name =="
    "$ZIG" cc -target "$target" "${cpu[@]}" -std=c11 -fPIC -O2 -D_GNU_SOURCE \
        -I"$ROOT/include" -I"$alsa_inc" \
        -c "$ROOT/src/ampdac_client.c" -o "$out/obj/ampdac_client.o"
    "$ZIG" cc -target "$target" "${cpu[@]}" -std=c11 -fPIC -DPIC -O2 -D_GNU_SOURCE \
        -I"$ROOT/include" -I"$alsa_inc" \
        -c "$ROOT/alsa/pcm_ampdac.c" -o "$out/obj/pcm_ampdac.o"
    "$ZIG" cc -target "$target" "${cpu[@]}" -shared -fPIC \
        -o "$out/libasound_module_pcm_ampdac.so" \
        "$out/obj/pcm_ampdac.o" "$out/obj/ampdac_client.o" "$alsa_lib"

    local bin="$ROOT/volumio/ampdac/bin/$name"
    mkdir -p "$bin"
    install -m 755 "$out/ampdacd" "$out/libampdac.so" "$out/libasound_module_pcm_ampdac.so" "$bin/"
    echo "== glibc symbols $name =="
    python3 - "$bin" << 'PY'
import subprocess, sys, os, re
bad = []
for name in os.listdir(sys.argv[1]):
    path = os.path.join(sys.argv[1], name)
    out = subprocess.check_output(["readelf", "-V", path], text=True, errors="replace")
    for ver in re.findall(r"GLIBC_([0-9]+)\.([0-9]+)", out):
        major, minor = int(ver[0]), int(ver[1])
        if (major, minor) > (2, 28):
            bad.append(f"{name} GLIBC_{major}.{minor}")
    needed = subprocess.check_output(["readelf", "-d", path], text=True, errors="replace")
    for lib in ("libusb", "libstdc++", "libc++"):
        if lib in needed:
            bad.append(f"{name} needs {lib}")
    for line in needed.splitlines():
        if "NEEDED" in line and "/" in line:
            bad.append(f"{name} has an absolute NEEDED: {line.strip()}")
if bad:
    print("\n".join(bad))
    sys.exit(1)
print("ok", sys.argv[1])
PY
}

build_arch arm-linux-gnueabihf.2.28 armv7l "$CROSS/sysroot/armhf" "generic+v6+vfp2"
build_arch aarch64-linux-gnu.2.28 aarch64 "$CROSS/sysroot/arm64"

STAGE="$CROSS/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a "$ROOT/volumio/ampdac/." "$STAGE/"
find "$STAGE" -name '*.sh' -exec chmod 755 {} +
mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/ampdac-volumio.zip"
rm -f "$OUT"
( cd "$STAGE" && zip -r "$OUT" . )
echo "wrote $OUT"

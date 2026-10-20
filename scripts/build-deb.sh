#!/bin/bash
# Build dist/ampdac_<version>_amd64.deb from the desktop Makefile artifacts.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.3.19
debver=${version}-1
arch=amd64
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

make -C "$root" -j"$(nproc)" \
    build/libampdac.so build/ampdacd build/ampdac-cli \
    build/libasound_module_pcm_ampdac.so

multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
libdir="$stage/usr/lib/$multiarch"
install -d \
    "$stage/usr/bin" \
    "$stage/usr/lib" \
    "$libdir/alsa-lib" \
    "$stage/usr/share/alsa/alsa.conf.d" \
    "$stage/etc/alsa/conf.d" \
    "$stage/usr/lib/systemd/system" \
    "$stage/usr/lib/udev/rules.d" \
    "$stage/usr/share/wireplumber/wireplumber.conf.d" \
    "$stage/DEBIAN" \
    "$root/dist"

install -m 755 "$root/build/ampdacd" "$stage/usr/bin/ampdacd"
install -m 755 "$root/build/ampdac-cli" "$stage/usr/bin/ampdac-cli"
install -m 755 "$root/volumio/ampdac/ampdac" "$stage/usr/bin/ampdac"
# ampdacd is linked with RPATH $ORIGIN/../lib, i.e. /usr/lib next to /usr/bin.
install -m 755 "$root/build/libampdac.so" "$stage/usr/lib/libampdac.so"
install -m 755 "$root/build/libasound_module_pcm_ampdac.so" \
    "$libdir/alsa-lib/libasound_module_pcm_ampdac.so"
install -m 644 "$root/alsa/50-ampdac.conf" \
    "$stage/usr/share/alsa/alsa.conf.d/50-ampdac.conf"
ln -s /usr/share/alsa/alsa.conf.d/50-ampdac.conf \
    "$stage/etc/alsa/conf.d/50-ampdac.conf"
install -m 644 "$root/systemd/ampdac.service" "$root/systemd/ampdac.socket" \
    "$stage/usr/lib/systemd/system/"
install -m 644 "$root/udev/99-ampdac.rules" \
    "$stage/usr/lib/udev/rules.d/99-ampdac.rules"
install -m 644 "$root/wireplumber/50-ampdac.conf" \
    "$stage/usr/share/wireplumber/wireplumber.conf.d/50-ampdac.conf"

install -m 755 "$root/packaging/deb/postinst" "$stage/DEBIAN/postinst"
install -m 755 "$root/packaging/deb/prerm" "$stage/DEBIAN/prerm"
install -m 755 "$root/packaging/deb/postrm" "$stage/DEBIAN/postrm"

shlib_log=$(mktemp)
mkdir -p "$root/debian"
if [ ! -f "$root/debian/control" ]; then
    printf 'Source: ampdac\n\nPackage: ampdac\nArchitecture: any\n' > "$root/debian/control"
    remove_control=1
else
    remove_control=0
fi
shlibs=$(cd "$root" && dpkg-shlibdeps -O \
    "$stage/usr/bin/ampdacd" \
    "$stage/usr/bin/ampdac-cli" \
    "$stage/usr/lib/libampdac.so" \
    "$libdir/alsa-lib/libasound_module_pcm_ampdac.so" \
    2>"$shlib_log" | sed -n 's/^shlibs:Depends=//p')
cat "$shlib_log" >&2 || true
rm -f "$shlib_log" "$root/debian/substvars"
if [ "$remove_control" -eq 1 ]; then
    rm -f "$root/debian/control"
    rmdir "$root/debian" 2>/dev/null || true
fi
if [ -z "$shlibs" ]; then
    echo "dpkg-shlibdeps produced no Depends line" >&2
    exit 1
fi

size=$(du -sk "$stage" | awk '{print $1}')
cat > "$stage/DEBIAN/control" << EOF
Package: ampdac
Version: $debver
Architecture: $arch
Maintainer: Peter <petergross82@users.noreply.github.com>
Section: sound
Priority: optional
Depends: $shlibs, python3, systemd
Recommends: ffmpeg
Installed-Size: $size
Description: userspace UAC2 USB DAC transport
 Bit-perfect PCM, native DSD and DoP. ampdacd holds the DAC; players open
 the ALSA device ampdac. Includes ampdac show status and ampdac show transport.
EOF

out="$root/dist/ampdac_${debver}_${arch}.deb"
fakeroot dpkg-deb --root-owner-group --build "$stage" "$out"
dpkg-deb -I "$out"
dpkg-deb -c "$out"

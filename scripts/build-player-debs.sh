#!/bin/bash
# Build the Volumio and moOde packages from the prebuilt player binaries.
# The Pi images do not compile. moOde is 64-bit only: that tree has no armhf build.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=0.3.21
debver=${version}-1
mkdir -p "$root/dist"

copy_tree() {
    local src="$1" dest="$2"
    mkdir -p "$dest"
    tar -C "$src" \
        --exclude bin \
        --exclude '__pycache__' \
        --exclude '*.pyc' \
        -cf - . | tar -C "$dest" -xf -
}

write_control() {
    local stage="$1" package="$2" arch="$3" description="$4"
    local size
    size=$(du -sk "$stage" | awk '{print $1}')
    cat > "$stage/DEBIAN/control" << EOF
Package: $package
Version: $debver
Architecture: $arch
Maintainer: Peter <petergross82@users.noreply.github.com>
Section: sound
Priority: optional
Depends: systemd
Installed-Size: $size
Description: $description
EOF
}

build_one() {
    local stage="$1" out="$2"
    mkdir -p "$stage/DEBIAN"
    fakeroot dpkg-deb --root-owner-group --build "$stage" "$out"
    dpkg-deb -I "$out"
    echo "built $out"
}

# Volumio: one package per image architecture. install.sh picks bin/$(uname -m).
build_volumio() {
    local debarch="$1" binarch="$2"
    local stage out
    stage=$(mktemp -d)
    out="$root/dist/ampdac-volumio_${debver}_${debarch}.deb"
    copy_tree "$root/volumio/ampdac" "$stage/usr/share/ampdac/volumio"
    mkdir -p "$stage/usr/share/ampdac/volumio/bin/$binarch"
    cp -a "$root/volumio/ampdac/bin/$binarch/." \
        "$stage/usr/share/ampdac/volumio/bin/$binarch/"
    chmod 755 "$stage/usr/share/ampdac/volumio/bin/$binarch/"*
    mkdir -p "$stage/DEBIAN"
    install -m 755 "$root/packaging/deb/volumio-postinst" "$stage/DEBIAN/postinst"
    install -m 755 "$root/packaging/deb/volumio-prerm" "$stage/DEBIAN/prerm"
    write_control "$stage" ampdac-volumio "$debarch" \
        "Volumio plugin for the libampdac UAC2 transport
 Playback Options lists the kernel card and libampdac for the same DAC.
 Switching during playback reopens the output and keeps the queue.
 Remove this package with: dpkg -r ampdac-volumio"
    build_one "$stage" "$out"
    rm -rf "$stage"
}

build_moode() {
    local stage out
    stage=$(mktemp -d)
    out="$root/dist/ampdac-moode_${debver}_arm64.deb"
    copy_tree "$root/moode/ampdac" "$stage/usr/share/ampdac/moode"
    mkdir -p "$stage/usr/share/ampdac/moode/bin/aarch64"
    cp -a "$root/moode/ampdac/bin/aarch64/." \
        "$stage/usr/share/ampdac/moode/bin/aarch64/"
    chmod 755 "$stage/usr/share/ampdac/moode/bin/aarch64/"*
    mkdir -p "$stage/DEBIAN"
    install -m 755 "$root/packaging/deb/moode-postinst" "$stage/DEBIAN/postinst"
    install -m 755 "$root/packaging/deb/moode-prerm" "$stage/DEBIAN/prerm"
    write_control "$stage" ampdac-moode arm64 \
        "moOde integration for the libampdac UAC2 transport
 Audio Output device lists the kernel card and libampdac for the same DAC.
 64-bit moOde only. Remove this package with: dpkg -r ampdac-moode"
    build_one "$stage" "$out"
    rm -rf "$stage"
}

build_volumio armhf armv7l
build_volumio arm64 aarch64
build_moode

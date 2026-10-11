#!/bin/sh
# Install udev rules and grant the current usbfs node so ampdac-cli can
# libusb_open a plugged DAC without waiting for a replug.
set -e
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
RULES_SRC="$ROOT/udev/99-ampdac.rules"
RULES_DST=/etc/udev/rules.d/99-ampdac.rules

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -- "$0" "$@"
fi

install -m 0644 "$RULES_SRC" "$RULES_DST"
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --action=add

# Already-plugged devices: chmod the usbfs node if any interface is Audio (0x01).
for sys in /sys/bus/usb/devices/[0-9]*; do
    [ -f "$sys/busnum" ] && [ -f "$sys/devnum" ] || continue
    audio=0
    for iface in "$sys"/*/bInterfaceClass; do
        [ -f "$iface" ] || continue
        if [ "$(cat "$iface")" = "01" ]; then
            audio=1
            break
        fi
    done
    [ "$audio" = 1 ] || continue
    bus=$(printf '%03d' "$(cat "$sys/busnum")")
    dev=$(printf '%03d' "$(cat "$sys/devnum")")
    node="/dev/bus/usb/$bus/$dev"
    [ -e "$node" ] || continue
    chmod 0664 "$node"
    chgrp plugdev "$node" 2>/dev/null || true
    echo "granted $node ($(cat "$sys/idVendor" 2>/dev/null):$(cat "$sys/idProduct" 2>/dev/null))"
done

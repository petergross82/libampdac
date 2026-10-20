#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$HERE/patch-volumio-ui.py" ]; then
    python3 "$HERE/patch-volumio-ui.py" --remove || true
fi
bash "$HERE/unpatch-mpd.sh" || true
systemctl disable --now ampdac-asound.path ampdac-asound.service >/dev/null 2>&1 || true
rm -f /var/lib/ampdac.route
systemctl disable --now ampdac.socket ampdac.service || true
rm -f /usr/bin/ampdac /usr/bin/ampdacd /usr/bin/libampdac.so /usr/lib/libampdac.so \
    /usr/bin/libampdac_compat.so /usr/lib/libampdac_compat.so /lib/libampdac_compat.so \
    /lib/arm-linux-gnueabihf/libampdac_compat.so /usr/lib/arm-linux-gnueabihf/libampdac_compat.so \
    /lib/aarch64-linux-gnu/libampdac_compat.so /usr/lib/aarch64-linux-gnu/libampdac_compat.so \
    /lib/libampdac.so \
    /lib/arm-linux-gnueabihf/libampdac.so /usr/lib/arm-linux-gnueabihf/libampdac.so \
    /lib/aarch64-linux-gnu/libampdac.so /usr/lib/aarch64-linux-gnu/libampdac.so \
    /usr/lib/arm-linux-gnueabihf/alsa-lib/libasound_module_pcm_ampdac.so \
    /usr/lib/aarch64-linux-gnu/alsa-lib/libasound_module_pcm_ampdac.so \
    /usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_pcm_ampdac.so \
    /usr/lib/alsa-lib/libasound_module_pcm_ampdac.so \
    /usr/share/alsa/alsa.conf.d/50-ampdac.conf \
    /etc/alsa/conf.d/50-ampdac.conf \
    /etc/systemd/system/ampdac.service \
    /etc/systemd/system/ampdac.socket \
    /etc/udev/rules.d/99-ampdac.rules
ldconfig || true
rm -f /etc/sudoers.d/ampdac /etc/sudoers.d/zzzz-ampdac /var/lib/ampdac.enabled
rm -f /usr/local/sbin/ampdac-patch-mpd /usr/local/sbin/ampdac-unpatch-mpd \
    /usr/local/sbin/ampdac-asound
rm -rf /usr/local/share/ampdac
rm -f /etc/systemd/system/ampdac-mpd.service /etc/systemd/system/ampdac-mpd.path \
    /etc/systemd/system/ampdac-asound.service /etc/systemd/system/ampdac-asound.path
rm -f /etc/systemd/system/mpd.service.d/ampdac.conf
rmdir /etc/systemd/system/mpd.service.d 2>/dev/null || true
systemctl daemon-reload || true
echo "plugininstallend"
exit 0

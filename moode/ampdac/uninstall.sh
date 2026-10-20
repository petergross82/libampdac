#!/bin/bash
# Remove the moOde libampdac page and restore the kernel route.
if [ -z "${BASH_VERSION:-}" ]; then
    exec /bin/bash "$0" "$@"
fi
set -eu

if [ -x /usr/local/sbin/ampdac-moode ] && [ -f /var/lib/ampdac.route ]; then
    /usr/local/sbin/ampdac-moode kernel || true
fi

systemctl disable --now ampdac-moode.path >/dev/null 2>&1 || true
systemctl disable ampdac-moode-prime.service >/dev/null 2>&1 || true
systemctl disable ampdac-moode-settle.service >/dev/null 2>&1 || true
systemctl disable --now ampdac.socket >/dev/null 2>&1 || true
systemctl stop ampdac.service >/dev/null 2>&1 || true

rm -f /etc/systemd/system/ampdac.service \
    /etc/systemd/system/ampdac.socket \
    /etc/systemd/system/ampdac-moode.service \
    /etc/systemd/system/ampdac-moode.path \
    /etc/systemd/system/ampdac-moode-prime.service \
    /etc/systemd/system/ampdac-moode-settle.service \
    /usr/local/sbin/ampdac-moode \
    /usr/bin/ampdacd \
    /usr/bin/ampdac \
    /etc/sudoers.d/ampdac-moode \
    /etc/udev/rules.d/99-ampdac.rules \
    /usr/share/alsa/alsa.conf.d/50-ampdac.conf \
    /etc/alsa/conf.d/50-ampdac.conf \
    /var/www/ampdac-config.php \
    /var/lib/ampdac.route \
    /var/lib/ampdac.kernel \
    /usr/lib/libampdac.so \
    /usr/lib/aarch64-linux-gnu/libampdac.so \
    /usr/lib/arm-linux-gnueabihf/libampdac.so \
    /usr/lib/aarch64-linux-gnu/alsa-lib/libasound_module_pcm_ampdac.so \
    /usr/lib/arm-linux-gnueabihf/alsa-lib/libasound_module_pcm_ampdac.so \
    /usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_pcm_ampdac.so \
    /usr/lib/alsa-lib/libasound_module_pcm_ampdac.so

python3 - /var/www/footer.min.php << 'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
if not path.exists():
    raise SystemExit(0)
text = path.read_text(encoding="utf-8")
start = text.find("<!-- ampdac-config -->")
end = text.find("<!-- /ampdac-config -->")
if start >= 0 and end > start:
    end += len("<!-- /ampdac-config -->")
    path.write_text(text[:start] + text[end:], encoding="utf-8")
PY

if [ -f /usr/local/share/ampdac/patch-moode-ui.py ]; then
    python3 /usr/local/share/ampdac/patch-moode-ui.py remove || true
fi
rm -f /usr/local/share/ampdac/patch-moode-ui.py
systemctl daemon-reload || true
udevadm control --reload-rules || true
ldconfig || true
echo "ampdac: удалено"
exit 0

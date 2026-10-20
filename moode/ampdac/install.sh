#!/bin/bash
# Install libampdac on moOde and add a Configure page that switches the output.
# moOde runs this as root: sudo ./install.sh
if [ -z "${BASH_VERSION:-}" ]; then
    exec /bin/bash "$0" "$@"
fi
set -eu

log() { echo "$*"; }

HERE="$(cd "$(dirname "$0")" && pwd)"
ARCH="$(uname -m)"
case "$ARCH" in
    armv7l|armv6l) BINARCH=armv7l ;;
    aarch64) BINARCH=aarch64 ;;
    x86_64) BINARCH=x86_64 ;;
    *)
        log "ampdac: архитектура $ARCH не поддерживается"
        exit 1
        ;;
esac

BIN="$HERE/bin/$BINARCH"
if [ ! -f "$BIN/ampdacd" ] || [ ! -f "$BIN/libampdac.so" ] || [ ! -f "$BIN/libasound_module_pcm_ampdac.so" ]; then
    log "ampdac: нет готовых файлов для $ARCH ($BIN)"
    exit 1
fi
chmod 755 "$BIN/ampdacd" "$BIN/libampdac.so" "$BIN/libasound_module_pcm_ampdac.so"

if ! ldconfig -p 2>/dev/null | grep -q 'libasound\.so\.2'; then
    log "ampdac: ставлю libasound"
    apt-get update
    apt-get install -y libasound2 || apt-get install -y libasound2t64
fi

install -d /usr/lib /usr/bin /etc/systemd/system /etc/udev/rules.d \
    /usr/share/alsa/alsa.conf.d /etc/alsa/conf.d /usr/local/sbin \
    /var/www/templates

install -m 755 "$BIN/ampdacd" /usr/bin/ampdacd
for d in /usr/lib/arm-linux-gnueabihf /usr/lib/aarch64-linux-gnu /usr/lib; do
    [ -d "$d" ] || continue
    install -m 755 "$BIN/libampdac.so" "$d/libampdac.so"
done
ldconfig || true
if ! /usr/bin/ampdacd --help >/tmp/ampdac-start.err 2>&1; then
    log "ampdac: ampdacd не запускается"
    cat /tmp/ampdac-start.err || true
    exit 1
fi

ALSA_DIR=""
for d in /usr/lib/arm-linux-gnueabihf/alsa-lib \
         /usr/lib/aarch64-linux-gnu/alsa-lib \
         /usr/lib/x86_64-linux-gnu/alsa-lib \
         /usr/lib/alsa-lib; do
    if [ -d "$d" ]; then
        ALSA_DIR="$d"
        break
    fi
done
if [ -z "$ALSA_DIR" ]; then
    ASOUND="$(ldconfig -p | awk '/libasound\.so\.2 / { print $NF; exit }')"
    ALSA_DIR="$(dirname "$ASOUND")/alsa-lib"
    install -d "$ALSA_DIR"
fi
install -m 755 "$BIN/libasound_module_pcm_ampdac.so" "$ALSA_DIR/libasound_module_pcm_ampdac.so"
install -m 644 "$HERE/alsa/50-ampdac.conf" /usr/share/alsa/alsa.conf.d/50-ampdac.conf
ln -sfn /usr/share/alsa/alsa.conf.d/50-ampdac.conf /etc/alsa/conf.d/50-ampdac.conf

install -m 644 "$HERE/systemd/ampdac.service" /etc/systemd/system/ampdac.service
install -m 644 "$HERE/systemd/ampdac.socket" /etc/systemd/system/ampdac.socket
install -m 644 "$HERE/systemd/ampdac-moode.service" /etc/systemd/system/ampdac-moode.service
install -m 644 "$HERE/systemd/ampdac-moode.path" /etc/systemd/system/ampdac-moode.path
install -m 644 "$HERE/systemd/ampdac-moode-prime.service" /etc/systemd/system/ampdac-moode-prime.service
install -m 644 "$HERE/systemd/ampdac-moode-settle.service" /etc/systemd/system/ampdac-moode-settle.service
install -m 755 "$HERE/ampdac-moode.sh" /usr/local/sbin/ampdac-moode
install -m 755 "$HERE/ampdac" /usr/bin/ampdac
install -m 644 "$HERE/udev/99-ampdac.rules" /etc/udev/rules.d/99-ampdac.rules
udevadm control --reload-rules || true
udevadm trigger --subsystem-match=usb || true

if id mpd >/dev/null 2>&1; then
    usermod -aG audio mpd || true
fi

install -m 644 "$HERE/www/ampdac-config.php" /var/www/ampdac-config.php

FOOTER=/var/www/footer.min.php
python3 - "$FOOTER" << 'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
marker = "<!-- ampdac-config -->"
end = "<!-- /ampdac-config -->"
icon = (
    '<span style="display:inline-block;box-sizing:content-box;'
    'font-size:20px;font-weight:600;letter-spacing:.06em;'
    'line-height:36px;padding:9px">LaD</span>'
)
tile = (
    marker
    + '<li><a href="ampdac-config.php" class="btn btn-large">'
    + icon
    + '<br>libampdac</a></li>'
    + end
)
if marker in text:
    start_at = text.find(marker)
    end_at = text.find(end, start_at)
    if end_at < 0:
        raise SystemExit("ampdac: маркер плитки обрезан")
    block = text[start_at:end_at]
    old = '<i class="fa-solid fa-sharp fa-usb"></i>'
    if old not in block and "LaD" in block:
        raise SystemExit(0)
    block = block.replace(old, icon)
    path.write_text(text[:start_at] + block + text[end_at:], encoding="utf-8")
    raise SystemExit(0)
anchor = '<a href="snd-config.php" class="btn btn-large"><i class="fa-solid fa-sharp fa-volume-up"></i><br>Audio</a></li>'
at = text.find(anchor)
if at < 0:
    print("ampdac: пункт Configure не найден, страница доступна как /ampdac-config.php")
    raise SystemExit(0)
insert = at + len(anchor)
path.write_text(text[:insert] + tile + text[insert:], encoding="utf-8")
PY

install -d /etc/sudoers.d
tmp="$(mktemp)"
cat > "$tmp" << 'EOF'
www-data ALL=(ALL) NOPASSWD: /usr/local/sbin/ampdac-moode
www-data ALL=(ALL) NOPASSWD: /usr/local/sbin/ampdac-moode *
EOF
install -m 440 "$tmp" /etc/sudoers.d/ampdac-moode
rm -f "$tmp"
visudo -cf /etc/sudoers.d/ampdac-moode >/dev/null

systemctl daemon-reload
systemctl enable ampdac.socket >/dev/null
systemctl restart ampdac.socket
systemctl enable ampdac-moode.path >/dev/null || true
systemctl enable ampdac-moode-prime.service >/dev/null || true
systemctl enable ampdac-moode-settle.service >/dev/null || true
systemctl start ampdac-moode.path >/dev/null || true
# The daemon must not start at boot: it would hide the USB card before moOde sees it.
systemctl disable ampdac.service >/dev/null 2>&1 || true

install -d /usr/local/share/ampdac
install -m 755 "$HERE/patch-moode-ui.py" /usr/local/share/ampdac/patch-moode-ui.py
python3 /usr/local/share/ampdac/patch-moode-ui.py install
log "ampdac: установлено. Вкладка ampdac вверху страниц настройки."
exit 0

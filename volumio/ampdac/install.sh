#!/bin/bash
# Volumio runs this as: sh install.sh > /tmp/installog
# A non-zero exit is reported to the user as "Error".
if [ -z "${BASH_VERSION:-}" ]; then
    exec /bin/bash "$0" "$@"
fi
set -eu

log() {
    echo "$*"
}

trap 'log "ampdac: установка прервалась, строка $LINENO"; exit 1' ERR

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
    log "ampdac: в плагине нет готовых файлов для $ARCH ($BIN)"
    exit 1
fi
chmod 755 "$BIN/ampdacd" "$BIN/libampdac.so" "$BIN/libasound_module_pcm_ampdac.so"
log "ampdac: файлы для $ARCH на месте"

if ! ldconfig -p 2>/dev/null | grep -q 'libasound\.so\.2'; then
    log "ampdac: ставлю libasound"
    apt-get update
    apt-get install -y libasound2 || apt-get install -y libasound2t64
fi

install -d /usr/lib /usr/bin /etc/systemd/system /etc/udev/rules.d \
    /usr/share/alsa/alsa.conf.d /etc/alsa/conf.d
install -m 755 "$BIN/ampdacd" /usr/bin/ampdacd
# Older installs copied the library into every ldconfig directory, including
# /opt/vc/lib and the NEON hwcap folders. Those copies confuse the loader.
rm -f /opt/vc/lib/libampdac.so /opt/vc/lib/libampdac_compat.so \
    /usr/local/lib/libampdac.so /usr/local/lib/libampdac_compat.so \
    /usr/lib/arm-linux-gnueabihf/neon/libampdac.so \
    /usr/lib/arm-linux-gnueabihf/neon/libampdac_compat.so \
    /usr/lib/arm-linux-gnueabihf/neon/vfp/libampdac.so \
    /usr/lib/arm-linux-gnueabihf/neon/vfp/libampdac_compat.so \
    /usr/lib/aarch64-linux-gnu/neon/libampdac.so \
    /usr/lib/aarch64-linux-gnu/neon/libampdac_compat.so
# The binaries carry an absolute RPATH to these directories. Buster's
# ldconfig does not cache libampdac.so, so do not treat that as failure.
for d in /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf \
    /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu /usr/lib /lib /usr/bin; do
    [ -d "$d" ] || continue
    install -m 755 "$BIN/libampdac.so" "$d/libampdac.so"
    if [ -f "$BIN/libampdac_compat.so" ]; then
        install -m 755 "$BIN/libampdac_compat.so" "$d/libampdac_compat.so"
    fi
done
ldconfig || true
if ! /usr/bin/ampdacd --help >/tmp/ampdac-start.err 2>&1; then
    log "ampdac: ampdacd не запускается"
    cat /tmp/ampdac-start.err || true
    exit 1
fi
log "ampdac: демон установлен"

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
log "ampdac: устройство ALSA ampdac установлено"

install -m 644 "$HERE/systemd/ampdac.service" /etc/systemd/system/ampdac.service
install -m 644 "$HERE/systemd/ampdac.socket" /etc/systemd/system/ampdac.socket
install -m 644 "$HERE/systemd/ampdac-asound.service" /etc/systemd/system/ampdac-asound.service
install -m 644 "$HERE/systemd/ampdac-asound.path" /etc/systemd/system/ampdac-asound.path
# Older builds watched mpd.conf and disabled Volumio's own output. That is
# not how Volumio chooses a card; drop the watcher if it is still there.
systemctl disable --now ampdac-mpd.path >/dev/null 2>&1 || true
rm -f /etc/systemd/system/ampdac-mpd.service /etc/systemd/system/ampdac-mpd.path
install -d /usr/local/sbin
install -m 755 "$HERE/patch-mpd.sh" /usr/local/sbin/ampdac-patch-mpd
install -m 755 "$HERE/unpatch-mpd.sh" /usr/local/sbin/ampdac-unpatch-mpd
install -m 755 "$HERE/ampdac-asound.sh" /usr/local/sbin/ampdac-asound
install -m 755 "$HERE/ampdac" /usr/bin/ampdac
install -m 644 "$HERE/udev/99-ampdac.rules" /etc/udev/rules.d/99-ampdac.rules
udevadm control --reload-rules || true
udevadm trigger --subsystem-match=usb || true

if id mpd >/dev/null 2>&1; then
    usermod -aG audio mpd || true
fi
if id volumio >/dev/null 2>&1; then
    usermod -aG audio volumio || true
fi

systemctl daemon-reload
systemctl enable ampdac.socket >/dev/null 2>&1 || true
systemctl disable ampdac.service >/dev/null 2>&1 || true
systemctl reset-failed ampdac.service ampdac.socket >/dev/null 2>&1 || true
if ! systemctl restart ampdac.socket; then
    systemctl status ampdac.socket --no-pager || true
    log "ampdac: не удалось запустить ampdac.socket"
    exit 1
fi
if ! systemctl restart ampdac.service; then
    systemctl status ampdac.service --no-pager || true
    log "ampdac: не удалось запустить ampdac.service"
    exit 1
fi
if [ ! -S /run/ampdac.sock ]; then
    systemctl status ampdac.socket ampdac.service --no-pager || true
    log "ampdac: /run/ampdac.sock не появился"
    exit 1
fi
chmod 666 /run/ampdac.sock || true
log "ampdac: сокет запущен"

# Last file in sudoers.d. The ticket from this install dies on reboot;
# without NOPASSWD, "sudo -n" then asks for a password and uninstall fails.
/usr/local/sbin/ampdac-asound sudoers
systemctl enable ampdac-asound.service >/dev/null 2>&1 || true

# Drop a leftover MPD block and turn the stock output back on.
bash "$HERE/unpatch-mpd.sh"
# If ampdac is already the saved output, put pcm.volumio back on the plugin
# now. Volumio's own startup rewrite leaves card "K7" in asound.conf.
if [ -x /usr/local/sbin/ampdac-asound ]; then
    /usr/local/sbin/ampdac-asound sync || true
fi
if [ -f "$HERE/patch-volumio-ui.py" ]; then
    python3 "$HERE/patch-volumio-ui.py" || log "ampdac: кнопку Info в списке плагинов поставить не удалось"
fi
log "ampdac: установлено. В параметрах воспроизведения выберите «libampdac · имя ЦАПа»."
log "ampdac: проверка маршрута: ampdac show status"
echo "plugininstallend"
exit 0

#!/bin/bash
# Older builds appended an MPD output and set the stock one to enabled "no".
# Volumio does not play that output: it always opens PCM "volumio".
# Put the stock output back. The second device is an ALSA card, not an MPD block.
set -eu
trap 'echo "ampdac: не удалось поправить MPD, строка $LINENO"; exit 1' ERR

CONF=/etc/mpd.conf
TEMPLATE=/volumio/app/plugins/music_service/mpd/mpd.conf.tmpl
LOCK=/run/ampdac-mpd.lock

exec 9>"$LOCK"
flock 9

# The watcher re-applied the old disable. Turn it off before editing the file.
systemctl disable --now ampdac-mpd.path >/dev/null 2>&1 || true
rm -f /etc/systemd/system/mpd.service.d/ampdac.conf
rmdir /etc/systemd/system/mpd.service.d 2>/dev/null || true

strip_markers() {
    local conf="$1"
    [ -f "$conf" ] && [ -w "$conf" ] || return 0
    sed -i '/# ampdac-output begin/,/# ampdac-output end/d' "$conf"
    sed -i '/# ampdac-disabled$/d' "$conf"
}

strip_markers "$TEMPLATE"

if [ ! -f "$CONF" ] || [ ! -w "$CONF" ]; then
    echo "ampdac: нет доступа к $CONF"
    exit 0
fi

WORK="$(mktemp)"
cp "$CONF" "$WORK"
strip_markers "$WORK"
# A stopped plugin callback makes Volumio append the word "undefined".
# MPD then exits: unrecognized parameter: undefined.
sed -i '/^[[:space:]]*undefined[[:space:]]*$/d' "$WORK"

# The stock output was left at enabled "no" after the marker comment was lost.
# Drop that line so MPD's default (enabled) applies. Leave multiroom alone.
awk '
    /^[[:space:]]*audio_output[[:space:]]*\{/ {
        inblk = 1
        n = 0
        lines[n++] = $0
        is_volumio = 0
        next
    }
    inblk {
        if ($0 ~ /device[[:space:]]+"volumio"/) is_volumio = 1
        lines[n++] = $0
        if ($0 ~ /^[[:space:]]*}/) {
            for (i = 0; i < n; i++) {
                if (is_volumio && lines[i] ~ /^[[:space:]]*enabled[[:space:]]+"no"/) continue
                print lines[i]
            }
            inblk = 0
        }
        next
    }
    { print }
' "$WORK" > "$WORK.2"
mv "$WORK.2" "$WORK"

if cmp -s "$CONF" "$WORK"; then
    rm -f "$WORK"
    echo "ampdac: mpd.conf уже в порядке"
    exit 0
fi

cat "$WORK" > "$CONF"
rm -f "$WORK"
echo "ampdac: штатный выход MPD снова включён"
systemctl restart mpd || true
exit 0

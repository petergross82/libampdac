#!/bin/bash
# Re-apply the ampdac MPD output. Volumio regenerates mpd.conf from its template.
set -e
MARK="# ampdac-output begin"
SNIPPET="$(dirname "$0")/mpd-output.conf"
TARGETS=(
    /etc/mpd.conf
    /volumio/app/plugins/music_service/mpd/mpd.conf.tmpl
)

systemctl enable --now ampdac.socket

for conf in "${TARGETS[@]}"; do
    [ -f "$conf" ] || continue
    if grep -q "$MARK" "$conf"; then
        continue
    fi
    printf '\n' >> "$conf"
    cat "$SNIPPET" >> "$conf"
    echo "ampdac: appended output to $conf"
done

systemctl restart mpd || true

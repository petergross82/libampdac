#!/bin/bash
set -e
for conf in /etc/mpd.conf /volumio/app/plugins/music_service/mpd/mpd.conf.tmpl; do
    [ -f "$conf" ] || continue
    sed -i '/# ampdac-output begin/,/# ampdac-output end/d' "$conf"
done
systemctl restart mpd || true

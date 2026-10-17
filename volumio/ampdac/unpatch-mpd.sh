#!/bin/bash
# Same repair as patch. Installed name is ampdac-unpatch-mpd, next to
# ampdac-patch-mpd. From the plugin directory the sibling is still patch-mpd.sh.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$HERE/patch-mpd.sh" ]; then
    exec /bin/bash "$HERE/patch-mpd.sh"
fi
if [ -f "$HERE/ampdac-patch-mpd" ]; then
    exec /bin/bash "$HERE/ampdac-patch-mpd"
fi
echo "ampdac: скрипт правки MPD не найден, пропуск"
exit 0

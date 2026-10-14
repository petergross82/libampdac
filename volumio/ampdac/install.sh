#!/bin/bash
# Volumio runs this as root from the plugin directory.
set -e
echo "ampdac: enable the daemon, then start the plugin to patch MPD."
if ! command -v ampdacd >/dev/null 2>&1; then
    echo "ampdacd is not installed. Install the ampdac package before enabling this plugin." >&2
    exit 1
fi
systemctl enable --now ampdac.socket
exit 0

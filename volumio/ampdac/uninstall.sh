#!/bin/bash
set -e
bash "$(dirname "$0")/unpatch-mpd.sh" || true
exit 0

#!/bin/bash
# Keep pcm.volumio on the ampdac plugin while that output is selected.
# Volumio rewrites /etc/asound.conf on startup and the in-memory hook is
# gone after reboot, so this script is what puts the route back.
set -eu

CONF=/etc/asound.conf
FLAG=/var/lib/ampdac.route
VCONF=/data/configuration/audio_interface/alsa_controller/config.json
LOCK=/run/ampdac-asound.lock
MODE="${1:-apply}"
RESTART_MPD=0

exec 9>"$LOCK"
flock 9

# The installer's sudo ticket dies on reboot. volumio is in group sudo, and a
# later sudoers.d file can require a password for every command. This name
# sorts last, so NOPASSWD stays the matching rule. Arguments need their own
# line: a bare path does not allow "ampdac-asound sync".
ensure_sudoers() {
    local dest=/etc/sudoers.d/zzzz-ampdac
    local work
    work="$(mktemp)"
    cat > "$work" << 'EOF'
volumio ALL=(ALL) NOPASSWD: /usr/local/sbin/ampdac-patch-mpd
volumio ALL=(ALL) NOPASSWD: /usr/local/sbin/ampdac-unpatch-mpd
volumio ALL=(ALL) NOPASSWD: /usr/local/sbin/ampdac-asound
volumio ALL=(ALL) NOPASSWD: /usr/local/sbin/ampdac-asound *
EOF
    if [ ! -f "$dest" ] || ! cmp -s "$dest" "$work"; then
        cp "$work" "$dest"
    fi
    chown root:root "$dest"
    chmod 440 "$dest"
    rm -f "$work" /etc/sudoers.d/ampdac
}

ensure_sudoers

config_is_ampdac() {
    [ -f "$VCONF" ] || return 1
    # Match a compact one-line config as well as the pretty v-conf file.
    # "ampdac USB DAC" is a different value and must not count.
    grep -qE '"value"[[:space:]]*:[[:space:]]*"ampdac"' "$VCONF"
}

want_route() {
    if [ -f "$VCONF" ]; then
        config_is_ampdac && return 0
        return 1
    fi
    [ -f "$FLAG" ]
}

write_route() {
    local work
    work="$(mktemp)"
    cat > "$work" << 'EOF'
# ampdac-route
# Bit-perfect: no plug, no softvol. Volume stays on the DAC.
pcm.!default {
    type empty
    slave.pcm "volumio"
}

pcm.volumio {
    type empty
    slave.pcm "ampdac"
}
EOF
    # mktemp is mode 600. Leaving that mode on /etc/asound.conf hides the
    # route from `ampdac show status` and from any player that is not root.
    if [ -f "$CONF" ] && cmp -s "$CONF" "$work"; then
        chmod 644 "$CONF" || true
        rm -f "$work"
        return 1
    fi
    local tmp
    tmp="$(mktemp "${CONF}.XXXXXX")"
    cat "$work" > "$tmp"
    chmod 644 "$tmp"
    mv -f "$tmp" "$CONF"
    chmod 644 "$CONF" || true
    rm -f "$work"
    return 0
}

enable_watch() {
    systemctl enable ampdac-asound.service >/dev/null 2>&1 || true
    systemctl enable --now ampdac-asound.path >/dev/null 2>&1 || true
}

disable_watch() {
    # The boot service stays enabled: it rewrites the sudoers rule after reboot.
    systemctl disable --now ampdac-asound.path >/dev/null 2>&1 || true
}

apply_route() {
    mkdir -p /var/lib
    touch "$FLAG"
    enable_watch
    if write_route; then
        echo changed
        # Restarting mpd while this lock is held deadlocks: the path unit
        # starts this same script, and mpd waits for that start.
        RESTART_MPD=1
        return 0
    fi
    echo unchanged
}

# `ampdac show status` runs as the volumio user. asound.conf, the Volumio
# config and mpd.conf are often root-only, and a bare `sudo cat` asks for a
# password. This mode is covered by the NOPASSWD rule above.
report_status() {
    if [ -f "$CONF" ]; then
        chmod 644 "$CONF" || true
    fi
    if [ -r "$CONF" ]; then
        if grep -q 'ampdac-route' "$CONF" || grep -q 'slave.pcm[[:space:]]*"ampdac"' "$CONF"; then
            echo "route|ampdac|"
        else
            local card
            card="$(awk '
                {
                    p = index($0, "card \"")
                    if (p == 0) next
                    line = substr($0, p + 6)
                    q = index(line, "\"")
                    if (q > 1) print substr(line, 1, q - 1)
                }
            ' "$CONF" | head -1)"
            echo "route|kernel|$card"
        fi
    elif [ -f "$CONF" ]; then
        echo "route|unreadable|"
    else
        echo "route|missing|"
    fi

    if [ -r "$VCONF" ]; then
        local web_name web_id web_card
        web_name="$(json_field outputdevicename)"
        web_id="$(json_field outputdevice)"
        web_card="$(json_field outputdevicecardname)"
        echo "web|$web_name|$web_id|$web_card"
        if [ -z "$web_name$web_id$web_card" ]; then
            local snip
            snip="$(grep -E 'outputdevice' "$VCONF" | head -2 | tr '\n' ' ' | tr '|' '/' | cut -c1-180 || true)"
            echo "websnippet|${snip}"
        fi
    elif [ -f "$VCONF" ]; then
        echo "webstate|unreadable"
    else
        echo "webstate|missing"
    fi

    local mpd=/etc/mpd.conf
    if [ -r "$mpd" ]; then
        local mpd_out
        mpd_out="$(awk '
            function trim(s) {
                gsub(/^[ \t]+|[ \t]+$/, "", s)
                return s
            }
            function quoted(s,    p, rest, q) {
                p = index(s, "\"")
                if (p == 0) return ""
                rest = substr(s, p + 1)
                q = index(rest, "\"")
                if (q == 0) return ""
                return substr(rest, 1, q - 1)
            }
            function field(s, name) {
                t = trim(s)
                return index(t, name) == 1
            }
            {
                t = trim($0)
                if (t == "undefined") broken = 1
            }
            index($0, "audio_output") && index($0, "{") {
                inblk = 1
                name = ""
                device = ""
                enabled = "yes"
                dop = ""
                next
            }
            inblk && trim($0) == "}" {
                printf "mpd|%s|%s|%s|%s\n", name, device, enabled, dop
                inblk = 0
                next
            }
            inblk && (field($0, "name") || field($0, "device") || field($0, "enabled") || field($0, "dop")) {
                t = trim($0)
                val = quoted($0)
                if (index(t, "name") == 1) name = val
                else if (index(t, "device") == 1) device = val
                else if (index(t, "enabled") == 1) enabled = val
                else if (index(t, "dop") == 1) dop = val
                next
            }
            END { if (broken) print "mpdwarn|undefined" }
        ' "$mpd")"
        if [ -n "$mpd_out" ]; then
            printf '%s\n' "$mpd_out"
        else
            local bytes hits
            bytes="$(wc -c < "$mpd" | tr -d ' ')"
            hits="$(grep -c audio_output "$mpd" || true)"
            echo "mpdstate|empty|$bytes|$hits"
        fi
    elif [ -f "$mpd" ]; then
        echo "mpdstate|unreadable"
    else
        echo "mpdstate|missing"
    fi

    # A connected socket means a player opened the DAC. The kernel card
    # disappears at that moment, so /proc/asound no longer says RUNNING.
    local player
    player="$(ss -xHp 2>/dev/null | awk '
        index($0, "/run/ampdac.sock") && index($0, "ESTAB") {
            if (match($0, /\* [0-9]+/)) {
                want[substr($0, RSTART + 2, RLENGTH - 2)] = 1
            }
        }
        { lines[++n] = $0 }
        END {
            for (i = 1; i <= n; i++) {
                line = lines[i]
                if (index(line, "ESTAB") == 0 || index(line, "ampdacd")) continue
                hit = 0
                for (ino in want) {
                    if (index(line, "* " ino " *")) hit = 1
                }
                if (!hit) continue
                name = ""
                pid = ""
                if (match(line, /users:\(\("[^"]+"/)) {
                    name = substr(line, RSTART, RLENGTH)
                    sub(/^users:\(\("/, "", name)
                    sub(/"$/, "", name)
                }
                if (match(line, /pid=[0-9]+/)) {
                    pid = substr(line, RSTART + 4, RLENGTH - 4)
                }
                if (name != "") {
                    printf "%s|%s\n", name, pid
                    exit
                }
            }
        }
    ' || true)"
    if [ -n "$player" ]; then
        echo "client|$player"
    elif [ -r /proc/net/unix ] && awk '$NF == "/run/ampdac.sock" && $6 == "03" { found = 1 } END { exit found ? 0 : 1 }' /proc/net/unix; then
        echo "client|open|"
    fi
}

json_field() {
    local key="$1"
    [ -r "$VCONF" ] || return 0
    # index(), not a regex class: the awk on Volumio does not apply
    # [[:space:]] inside a pattern built from a string, so the old parser
    # returned empty values.
    awk -v key="$key" '
        function qvalue(s,    p, rest, q) {
            p = index(s, "\"value\"")
            if (p == 0) return ""
            rest = substr(s, p + 7)
            p = index(rest, "\"")
            if (p == 0) return ""
            rest = substr(rest, p + 1)
            q = index(rest, "\"")
            if (q <= 1) return ""
            return substr(rest, 1, q - 1)
        }
        BEGIN { want = "\"" key "\""; left = 0 }
        {
            if (index($0, want) && index($0, ":")) {
                v = qvalue($0)
                if (v != "") { print v; exit }
                left = 6
                next
            }
            if (left > 0) {
                left--
                v = qvalue($0)
                if (v != "") { print v; exit }
            }
        }
    ' "$VCONF"
}

reload_alsa_config() {
    # libasound reads conf.d once per process. Touching the master file
    # makes the next snd_pcm_open in this MPD pick up the new slave.
    if [ -e /usr/share/alsa/alsa.conf ]; then
        touch /usr/share/alsa/alsa.conf
    fi
    if [ -e /etc/alsa/alsa.conf ]; then
        touch /etc/alsa/alsa.conf
    fi
}

mpd_alsa_output_id() {
    mpc outputs 2>/dev/null | awk '
        /^Output / {
            id = $2
            gsub(/[^0-9]/, "", id)
            if (index($0, "(alsa)") || index($0, " alsa")) {
                print id
                exit
            }
        }
    '
}

kernel_card_visible() {
    aplay -l 2>/dev/null | awk '
        /card / {
            line = tolower($0)
            if (line ~ /hdmi/ || line ~ /bcm2835/ || line ~ /headphone/) next
            found = 1
        }
        END { exit found ? 0 : 1 }
    '
}

# Close the open PCM, then open whatever slave is in asound.conf now.
# The flag and the file are already written by the caller. No mpd restart.
reopen_output() {
    local id i
    id="$(mpd_alsa_output_id || true)"
    mpc -q pause >/dev/null 2>&1 || true
    if [ -n "$id" ]; then
        mpc -q disable "$id" >/dev/null 2>&1 || true
    fi
    if ! grep -q 'slave.pcm[[:space:]]*"ampdac"' "$CONF" 2>/dev/null; then
        i=0
        while [ "$i" -lt 40 ]; do
            if kernel_card_visible; then
                break
            fi
            i=$((i + 1))
            sleep 0.1
        done
    fi
    reload_alsa_config
    if [ -n "$id" ]; then
        i=0
        while [ "$i" -lt 15 ]; do
            if mpc -q enable "$id" >/dev/null 2>&1; then
                break
            fi
            i=$((i + 1))
            sleep 0.1
        done
    fi
    echo reopened
}

PREV=/var/lib/ampdac.previous

remember_args() {
    mkdir -p /var/lib
    printf '%s\t%s\t%s\n' "$1" "$2" "$3" > "$PREV"
    chmod 644 "$PREV" || true
}

remember_from_config() {
    config_is_ampdac && return 0
    local id name card
    id="$(json_field outputdevice)"
    name="$(json_field outputdevicename)"
    card="$(json_field outputdevicecardname)"
    [ -n "$id" ] || return 0
    remember_args "$id" "$name" "$card"
}

case "$MODE" in
    sudoers)
        echo sudoers
        ;;
    report)
        report_status
        ;;
    relax)
        if [ -f "$CONF" ]; then
            chmod 644 "$CONF" || true
        fi
        echo relax
        ;;
    remember)
        remember_args "${2:-}" "${3:-}" "${4:-}"
        echo remember
        ;;
    off)
        rm -f "$FLAG"
        disable_watch
        echo off
        ;;
    live-off)
        rm -f "$FLAG"
        echo off
        ;;
    live-on)
        mkdir -p /var/lib
        touch "$FLAG"
        if write_route; then
            echo changed
        else
            echo unchanged
        fi
        ;;
    reopen)
        reopen_output
        ;;
    on|sync)
        if want_route || [ "$MODE" = "on" ]; then
            apply_route
        elif [ -f "$FLAG" ]; then
            rm -f "$FLAG"
            disable_watch
            echo off
        else
            remember_from_config
            echo idle
        fi
        ;;
    apply)
        if want_route || [ -f "$FLAG" ]; then
            if [ -f "$VCONF" ] && ! config_is_ampdac; then
                rm -f "$FLAG"
                remember_from_config
                echo idle
                exit 0
            fi
            apply_route
        else
            remember_from_config
            echo idle
        fi
        ;;
    *)
        echo "ampdac-asound: неизвестный режим $MODE" >&2
        exit 1
        ;;
esac
exec 9>&-
if [ "$RESTART_MPD" = 1 ]; then
    systemctl restart mpd >/dev/null 2>&1 || true
fi
exit 0

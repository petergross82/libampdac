#!/bin/bash
# Point moOde's pcm._audioout at the kernel card or at libampdac.
# moOde rewrites /etc/alsa/conf.d/_audioout.conf whenever Audio settings change.
# The path unit calls `apply` and puts libampdac back while the flag is set.
set -eu

CONF=/etc/alsa/conf.d/_audioout.conf
SLOOP=/etc/alsa/conf.d/_sndaloop.conf
FLAG=/var/lib/ampdac.route
SAVED=/var/lib/ampdac.kernel
LOCK=/run/ampdac-moode.lock
DB=/var/local/www/db/moode-sqlite3.db
MPD=/etc/mpd.conf
MODE="${1:-apply}"
WATCH="${AMPDAC_WATCH:-0}"

if [ "$MODE" = "transport" ]; then
    exec /usr/bin/ampdac show transport
fi

sql() {
    sqlite3 -cmd '.timeout 8000' "$DB" "$1"
}

sql_get() {
    sql "SELECT value FROM $1 WHERE param='$2';"
}

sql_set() {
    local table="$1" param="$2" value="$3"
    sql "UPDATE $table SET value='$(printf '%s' "$value" | sed "s/'/''/g")' WHERE param='$param';"
}

worker_ready() {
    [ "$(sql_get cfg_system wrkready 2>/dev/null || true)" = "1" ]
}

read_slave() {
    sed -n 's/^[[:space:]]*slave\.pcm[[:space:]]*"\(.*\)".*/\1/p' "$CONF" | head -1
}

# moOde's www-data user rewrites this file with sed. Keep it group/world writable.
write_slave() {
    local want="$1" cur work tmp
    [ -f "$CONF" ] || return 1
    cur="$(read_slave)"
    if [ "$cur" = "$want" ]; then
        chmod 666 "$CONF" || true
        return 1
    fi
    work="$(mktemp)"
    awk -v s="$want" '
        /^[[:space:]]*slave\.pcm[[:space:]]/ { print "slave.pcm \"" s "\""; next }
        { print }
    ' "$CONF" > "$work"
    tmp="$(mktemp "${CONF}.XXXXXX")"
    cat "$work" > "$tmp"
    chmod 666 "$tmp"
    mv -f "$tmp" "$CONF"
    chmod 666 "$CONF" || true
    rm -f "$work"
    return 0
}

kv_get() {
    local key="$1"
    [ -f "$SAVED" ] || return 1
    awk -F '\t' -v k="$key" '$1 == k { print substr($0, index($0, "\t") + 1); exit }' "$SAVED"
}

save_kernel() {
    local slave resample format dop mixer card name
    slave="$(read_slave)"
    case "$slave" in
        ampdac|"") return 0 ;;
    esac
    resample="$(sql_get cfg_mpd auto_resample)"
    format="$(sql_get cfg_mpd auto_format)"
    dop="$(sql_get cfg_mpd dop)"
    mixer="$(sql_get cfg_mpd mixer_type)"
    card="$(sql_get cfg_system cardnum)"
    name="$(sql_get cfg_system adevname)"
    {
        printf 'slave\t%s\n' "$slave"
        printf 'auto_resample\t%s\n' "$resample"
        printf 'auto_format\t%s\n' "$format"
        printf 'dop\t%s\n' "$dop"
        printf 'mixer_type\t%s\n' "$mixer"
        printf 'cardnum\t%s\n' "$card"
        printf 'adevname\t%s\n' "$name"
        printf 'file_auto_resample\t%s\n' "$(file_mpd_value auto_resample)"
        printf 'file_auto_format\t%s\n' "$(file_mpd_value auto_format)"
        printf 'file_dop\t%s\n' "$(file_mpd_value dop)"
        if grep -q '^pcm.!_audioout {$' "$SLOOP" 2>/dev/null; then
            printf 'loopback\ton\n'
        else
            printf 'loopback\toff\n'
        fi
    } > "$SAVED"
}

# moOde's ALSA loopback file overrides _audioout with a plug/multi that
# opens the kernel card. While libampdac is selected that override is off,
# so the copy plugin reaches ampdac with the file format unchanged.
loopback_set_pcm() {
    local want="$1"
    [ -f "$SLOOP" ] || return 0
    grep -q '^a { channels 2 pcm "' "$SLOOP" || return 0
    if grep -q "^a { channels 2 pcm \"${want}\" }$" "$SLOOP"; then
        return 0
    fi
    sed -i "s/^a { channels 2 pcm \".*\" }/a { channels 2 pcm \"${want}\" }/" "$SLOOP"
    chmod 666 "$SLOOP" || true
}

loopback_active() {
    [ -f "$SLOOP" ] && grep -q '^pcm.!_audioout {$' "$SLOOP"
}

loopback_disable() {
    loopback_active || return 1
    sed -i '0,/^pcm.!_audioout {$/s//pcm.!_audioout__ {/' "$SLOOP"
    chmod 666 "$SLOOP" || true
}

loopback_enable() {
    [ -f "$SLOOP" ] || return 1
    grep -q '^pcm.!_audioout__ {$' "$SLOOP" || return 1
    sed -i '0,/^pcm.!_audioout__ {$/s//pcm.!_audioout {/' "$SLOOP"
    chmod 666 "$SLOOP" || true
}

# The override opens hw:Loopback as well as the DAC. moOde unloads snd-aloop
# when ALSA loopback is Off, so putting the override back then makes
# _audioout fail with "No such device". Follow the live setting.
restore_kernel_route() {
    local slave="$1"
    loopback_set_pcm "$slave" || true
    if [ "$(sql_get cfg_system alsa_loopback 2>/dev/null || true)" = "On" ]; then
        modprobe snd-aloop || true
        local i
        for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
            if aplay -l 2>/dev/null | grep -q '\[Loopback\]'; then
                loopback_enable || true
                return 0
            fi
            sleep 0.1
        done
        echo "ampdac: snd-aloop did not start, output goes straight to the card"
        loopback_disable || true
        return 0
    fi
    loopback_disable || true
    modprobe -r snd-aloop >/dev/null 2>&1 || true
}

file_mpd_value() {
    local key="$1" found
    found="$(sed -n -E "s/^${key}[[:space:]]+\"([^\"]*)\".*/\\1/p" "$MPD" | head -1)"
    if [ -n "$found" ]; then
        printf '%s\n' "$found"
    else
        printf '%s\n' "-"
    fi
}

set_mpd_mixer() {
    local value="$1" work
    [ -f "$MPD" ] || return 0
    work="$(mktemp)"
    awk -v mixer="$value" '
        /^audio_output[[:space:]]*\{/ { inblk = 1; hit = 0 }
        inblk && /name[[:space:]]+"ALSA Default"/ { hit = 1 }
        inblk && hit && /^[[:space:]]*mixer_type[[:space:]]/ {
            print "mixer_type \"" mixer "\""
            next
        }
        inblk && /^[[:space:]]*\}/ { inblk = 0; hit = 0 }
        { print }
    ' "$MPD" > "$work"
    cat "$work" > "$MPD"
    rm -f "$work"
}

set_output_key() {
    local key="$1" value="$2" work
    [ -f "$MPD" ] || return 0
    work="$(mktemp)"
    awk -v key="$key" -v value="$value" '
        /^audio_output[[:space:]]*\{/ { inblk = 1; hit = 0; seen = 0 }
        inblk && /name[[:space:]]+"ALSA Default"/ { hit = 1 }
        inblk && hit && $1 == key {
            if (value != "-") print key " \"" value "\""
            seen = 1
            next
        }
        inblk && hit && /^[[:space:]]*}/ {
            if (value != "-" && !seen) print key " \"" value "\""
            inblk = 0
            hit = 0
        }
        { print }
    ' "$MPD" > "$work"
    cat "$work" > "$MPD"
    rm -f "$work"
}

apply_bitperfect() {
    sql_set cfg_mpd auto_resample no
    sql_set cfg_mpd auto_format no
    sql_set cfg_mpd dop no
    sql_set cfg_mpd mixer_type none
    set_output_key auto_resample no
    set_output_key auto_format no
    set_output_key dop no
    set_mpd_mixer none
}

restore_file_key() {
    local key="$1" saved="$2"
    [ -n "$saved" ] || return 0
    if [ "$saved" = "-" ]; then
        set_output_key "$key" "-"
    else
        set_output_key "$key" "$saved"
    fi
}

restore_mpd() {
    local resample format dop mixer
    resample="$(kv_get auto_resample || true)"
    format="$(kv_get auto_format || true)"
    dop="$(kv_get dop || true)"
    mixer="$(kv_get mixer_type || true)"
    [ -n "$resample" ] && sql_set cfg_mpd auto_resample "$resample"
    [ -n "$format" ] && sql_set cfg_mpd auto_format "$format"
    [ -n "$dop" ] && sql_set cfg_mpd dop "$dop"
    [ -n "$mixer" ] && sql_set cfg_mpd mixer_type "$mixer" && set_mpd_mixer "$mixer"
    restore_file_key auto_resample "$(kv_get file_auto_resample || true)"
    restore_file_key auto_format "$(kv_get file_auto_format || true)"
    restore_file_key dop "$(kv_get file_dop || true)"
}

card_number_for() {
    local name="$1"
    aplay -l 2>/dev/null | awk -F '[][]' -v name="$name" '
        /^card / && $2 == name {
            split($1, a, " ")
            gsub(":", "", a[2])
            print a[2]
            exit
        }
    '
}

kernel_slave() {
    local saved name card
    saved="$(kv_get slave || true)"
    name="$(kv_get adevname || true)"
    card=""
    if [ -n "$name" ]; then
        card="$(card_number_for "$name" || true)"
    fi
    case "$saved" in
        hw:*|plughw:*|iec958:*)
            if [ -n "$card" ]; then
                printf '%s\n' "${saved%%:*}:${card},0"
                return 0
            fi
            printf '%s\n' "$saved"
            return 0
            ;;
        ampdac|"")
            ;;
        *)
            printf '%s\n' "$saved"
            return 0
            ;;
    esac
    if [ -n "$card" ]; then
        printf 'hw:%s,0\n' "$card"
        return 0
    fi
    card="$(sql_get cfg_system cardnum || true)"
    case "$card" in
        ''|*[!0-9]*) card=0 ;;
    esac
    printf 'hw:%s,0\n' "$card"
}

remember_card() {
    local name card
    name="$(kv_get adevname || true)"
    [ -n "$name" ] || return 0
    card="$(card_number_for "$name" || true)"
    [ -n "$card" ] || return 0
    sql_set cfg_system cardnum "$card"
}

live_dac_card() {
    aplay -l 2>/dev/null | awk -F '[][]' '
        /^card / && $2 !~ /^(vc4-hdmi|HDMI|Loopback|Dummy)$/ {
            split($1, a, " ")
            gsub(":", "", a[2])
            print a[2]
            exit
        }
    '
}

live_dac_name() {
    aplay -l 2>/dev/null | awk -F '[][]' '
        /^card / && $2 !~ /^(vc4-hdmi|HDMI|Loopback|Dummy)$/ {
            print $2
            exit
        }
    '
}

usb_audio_product() {
    local d iface class subclass
    for d in /sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        for iface in "$d":*; do
            [ -f "$iface/bInterfaceClass" ] || continue
            class="$(tr -d ' \n' < "$iface/bInterfaceClass")"
            subclass="$(tr -d ' \n' < "$iface/bInterfaceSubClass" 2>/dev/null || true)"
            if [ "$class" = "01" ] && [ "$subclass" = "01" ] && [ -f "$d/product" ]; then
                tr -d '\0\n' < "$d/product"
                printf '\n'
                return 0
            fi
        done
    done
    return 1
}

remember_live_dac() {
    local name card
    name="$(live_dac_name || true)"
    card="$(live_dac_card || true)"
    [ -n "$name" ] && [ -n "$card" ] || return 1
    if [ -f "$SAVED" ]; then
        local work
        work="$(mktemp)"
        awk -F '\t' -v name="$name" -v card="$card" '
            $1 == "adevname" { print "adevname\t" name; next }
            $1 == "cardnum" { print "cardnum\t" card; next }
            $1 == "slave" { print "slave\thw:" card ",0"; next }
            { print }
        ' "$SAVED" > "$work"
        cat "$work" > "$SAVED"
        rm -f "$work"
    fi
    sql_set cfg_system cardnum "$card"
    sql_set cfg_system adevname "$name"
}

print_label() {
    local name card
    name="$(live_dac_name || true)"
    card="$(live_dac_card || true)"
    if [ -z "$name" ]; then
        name="$(usb_audio_product || true)"
    fi
    if [ -z "$name" ]; then
        name="$(kv_get adevname || true)"
    fi
    if [ -z "$card" ]; then
        card="$(kv_get cardnum || true)"
    fi
    printf 'name=%s\n' "$name"
    printf 'card=%s\n' "$card"
}

mpd_state() {
    mpc status 2>/dev/null | awk 'NR==2 { gsub(/[\[\]]/, "", $1); print $1; exit }'
}

mpd_output_id() {
    mpc outputs 2>/dev/null | awk '
        /^Output / && /ALSA Default/ {
            gsub(/[^0-9]/, "", $2)
            print $2
            exit
        }
    '
}

# libasound reads conf.d once per process. A later edit of _audioout.conf
# is invisible to the running MPD until the top-level file's mtime
# changes; the next snd_pcm_open then reloads the slave.
reload_alsa_config() {
    local f
    for f in /usr/share/alsa/alsa.conf /etc/alsa/alsa.conf; do
        [ -f "$f" ] || continue
        touch "$f" || true
    done
}

# Pause keeps the queue position. Disabling the output closes the PCM
# so the other driver can take the USB device.
close_mpd_output() {
    MPD_STATE="$(mpd_state || true)"
    mpc -q pause >/dev/null 2>&1 || true
    local id
    id="$(mpd_output_id || true)"
    if [ -n "$id" ]; then
        mpc -q disable "$id" >/dev/null 2>&1 || true
    fi
}

open_mpd_output() {
    local id i
    id="$(mpd_output_id || true)"
    if [ -z "$id" ]; then
        systemctl restart mpd || true
        if [ "${MPD_STATE:-}" = "playing" ]; then
            mpc -q play >/dev/null 2>&1 || true
        fi
        return 0
    fi
    # Disable again so MPD opens the slave that was just written, not the
    # device it recovered while the route was changing.
    mpc -q disable "$id" >/dev/null 2>&1 || true
    reload_alsa_config
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        if mpc -q enable "$id" >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
    if [ "${MPD_STATE:-}" = "playing" ]; then
        mpc -q play >/dev/null 2>&1 || true
    fi
}

release_dac() {
    # Stopping the daemon here makes MPD reopen the output it still has
    # enabled, which claims USB again before the slave pcm has changed.
    # Disabling that output already drops the interface; the process can
    # stay up.
    local i
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
        if [ -n "$(live_dac_card || true)" ]; then
            remember_live_dac || true
            return 0
        fi
        sleep 0.1
    done
    echo "ampdac: the kernel card did not reappear" >&2
    return 0
}

enable_watch() {
    systemctl enable ampdac-moode.path >/dev/null 2>&1 || true
    systemctl enable ampdac-moode-prime.service >/dev/null 2>&1 || true
    systemctl enable ampdac-moode-settle.service >/dev/null 2>&1 || true
    systemctl start ampdac-moode.path >/dev/null 2>&1 || true
}

disable_watch() {
    systemctl disable --now ampdac-moode.path >/dev/null 2>&1 || true
    systemctl disable ampdac-moode-prime.service >/dev/null 2>&1 || true
    systemctl disable ampdac-moode-settle.service >/dev/null 2>&1 || true
}

if [ "$MODE" = "label" ]; then
    print_label
    exit 0
fi

exec 9>"$LOCK"
flock 9

changed=0
restart=0
live_open=0
case "$MODE" in
    ampdac|on)
        close_mpd_output
        mkdir -p /var/lib
        save_kernel
        touch "$FLAG"
        apply_bitperfect
        if write_slave ampdac; then
            changed=1
        fi
        loopback_set_pcm ampdac || true
        if loopback_disable; then
            changed=1
        fi
        live_open=1
        echo "ampdac: _audioout is routed to libampdac"
        echo "mixer=$(sql_get cfg_mpd mixer_type)"
        ;;
    kernel|off)
        # Point the file at the kernel card before USB is released. moOde
        # restarts MPD when the card reappears, and that open must already
        # be hw rather than ampdac.
        rm -f "$FLAG"
        restore_mpd
        restore_kernel_route "$(kernel_slave)"
        write_slave "$(kernel_slave)" || true
        close_mpd_output
        release_dac
        remember_card
        restore_kernel_route "$(kernel_slave)"
        if write_slave "$(kernel_slave)"; then
            changed=1
        fi
        live_open=1
        echo "ampdac: _audioout is routed to $(kv_get adevname || echo the ALSA card)"
        echo "mixer=$(sql_get cfg_mpd mixer_type)"
        ;;
    release)
        rm -f "$FLAG"
        systemctl stop ampdac.service >/dev/null 2>&1 || true
        echo "ampdac: USB released"
        ;;
    prime)
        [ -f "$FLAG" ] || exit 0
        restore_kernel_route "$(kernel_slave)"
        write_slave "$(kernel_slave)" || true
        exit 0
        ;;
    settle)
        if [ ! -f "$FLAG" ]; then
            exit 0
        fi
        i=0
        while [ "$i" -lt 90 ]; do
            worker_ready && break
            i=$((i + 1))
            sleep 1
        done
        apply_bitperfect
        write_slave ampdac || true
        loopback_set_pcm ampdac || true
        loopback_disable || true
        restart=1
        ;;
    apply)
        if [ "$WATCH" != "1" ] || [ ! -f "$FLAG" ]; then
            exit 0
        fi
        if ! worker_ready; then
            exit 0
        fi
        if write_slave ampdac; then
            restart=1
        fi
        loopback_set_pcm ampdac || true
        if loopback_disable; then
            restart=1
        fi
        ;;
    *)
        echo "ampdac-moode: команда $MODE не поддерживается" >&2
        exit 2
        ;;
esac

flock -u 9

# The route flag, not these units, decides whether a moOde rewrite is put
# back. Enabling or disabling them is a daemon-reload and costs several
# seconds, which is the gap the listener hears.
if [ "$restart" = "1" ]; then
    systemctl restart mpd || true
elif [ "$live_open" = "1" ]; then
    open_mpd_output
fi

exit 0

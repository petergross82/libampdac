/*
 * Deadbeef output plugin for libampdac (UAC2 exclusive ISO).
 *
 * Pull model (same as ALSA): a worker thread calls streamer_read() and
 * pushes bytes into ampdacd (ampdac_client_write). One USB session lives across tracks
 * that share rate/channels/packing — that is gapless. Format change
 * closes and reopens the session (PLL retune).
 *
 * DSD: FFmpeg in Deadbeef turns DSF/DFF into float PCM unless DoP is on
 * (that was 1411200 Hz float — not a USB PCM rate). This plugin forces
 * ffmpeg.enable_dop so the streamer delivers 32-bit DoP. We unwrap the
 * markers and call ampdac_client_configure_dsd (AUTO: native RAW if the DAC has
 * it, else DoP on TYPE_I).
 *
 * Build: make -C linux plugin
 */

#include <deadbeef/deadbeef.h>
#include "ampdac_ipc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#define TRACE(...) deadbeef->log_detailed(&plugin.plugin, 0, __VA_ARGS__)

#define LOCK   deadbeef->mutex_lock(g_mutex)
#define UNLOCK deadbeef->mutex_unlock(g_mutex)

static DB_functions_t *deadbeef;
static DB_output_t plugin;

static uintptr_t g_mutex;
static intptr_t g_tid;
static int g_terminate;

static ddb_playback_state_t g_state = DDB_PLAYBACK_STATE_STOPPED;
static int g_setformat_requested;
static ddb_waveformat_t g_requested_fmt;

static ampdac_client *g_session;
static int g_unwrap_dop;
static uint8_t g_dop_carry[8];
static int g_dop_carry_n;

static int fmt_equal(const ddb_waveformat_t *a, const ddb_waveformat_t *b) {
    return a->bps == b->bps &&
           a->channels == b->channels &&
           a->samplerate == b->samplerate &&
           a->is_float == b->is_float &&
           a->flags == b->flags &&
           a->channelmask == b->channelmask;
}

static int is_ffmpeg_dsd_pcm_rate(int hz) {
    return hz == 352800 || hz == 705600 || hz == 1411200 ||
           hz == 2822400 || hz == 5644800 || hz == 11289600;
}

static void close_session(void) {
    if (g_session) {
        ampdac_client_disconnect(g_session);
        g_session = NULL;
    }
    g_unwrap_dop = 0;
    g_dop_carry_n = 0;
}

static uint32_t dsd_rate_from_dop_clock(int dop_hz) {
    if (dop_hz <= 0) return 0;
    return (uint32_t)dop_hz * 16U;
}

static const char *session_socket(char *buf, size_t buflen) {
    deadbeef->conf_get_str("ampdac.socket", "", buf, (int)buflen);
    return buf[0] ? buf : NULL;
}

static int open_session(const ddb_waveformat_t *fmt) {
    uint16_t vid = (uint16_t)deadbeef->conf_get_int("ampdac.vid", 0);
    uint16_t pid = (uint16_t)deadbeef->conf_get_int("ampdac.pid", 0);
    char socket_path[256];

    ddb_waveformat_t negotiated = *fmt;
    if (negotiated.channels <= 0) negotiated.channels = 2;
    if (negotiated.samplerate <= 0) negotiated.samplerate = 44100;
    if (negotiated.bps <= 0) negotiated.bps = 16;

    const int dop_flag = (negotiated.flags & DDB_WAVEFORMAT_FLAG_IS_DOP) != 0;

    /* FFmpeg dsd2pcm: float at packed DSD rate. Bits are already gone. */
    if (!dop_flag && negotiated.is_float &&
        is_ffmpeg_dsd_pcm_rate(negotiated.samplerate)) {
        TRACE("DSD arrived as %d Hz float (FFmpeg dsd2pcm). "
              "Enable DoP in FFmpeg and restart the track. "
              "ampdac will not send this as PCM.\n",
              negotiated.samplerate);
        if (deadbeef->conf_get_int("ampdac.force_ffmpeg_dop", 1)) {
            deadbeef->conf_set_int("ffmpeg.enable_dop", 1);
            deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
        }
        return -1;
    }

    ampdac_client *s = ampdac_client_connect(session_socket(socket_path, sizeof(socket_path)));
    if (!s) {
        TRACE("ampdacd connect failed: %s\n", ampdac_client_last_error());
        return -1;
    }

    ampdac_status st;
    int unwrap = 0;

    if (dop_flag) {
        ampdac_dsd_config dsd;
        memset(&dsd, 0, sizeof(dsd));
        dsd.dsd_rate_hz = dsd_rate_from_dop_clock(negotiated.samplerate);
        dsd.channels = (uint8_t)negotiated.channels;
        dsd.transport = AMPDAC_DSD_AUTO;
        negotiated.bps = 32;
        negotiated.is_float = 0;
        st = ampdac_client_configure_dsd(s, vid, pid, &dsd);
        unwrap = 1;
        if (st != AMPDAC_OK) {
            TRACE("configure_dsd failed (%s); falling back to DoP PCM wire\n",
                  ampdac_client_last_error());
            ampdac_pcm_config cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.sample_rate_hz = (uint32_t)negotiated.samplerate;
            cfg.channels = (uint8_t)negotiated.channels;
            cfg.format = AMPDAC_PCM_S32LE;
            cfg.volume = 1.0f;
            st = ampdac_client_configure_pcm(s, vid, pid, &cfg);
            unwrap = 0;
        }
    } else {
        ampdac_pcm_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.sample_rate_hz = (uint32_t)negotiated.samplerate;
        cfg.channels = (uint8_t)negotiated.channels;
        cfg.volume = 1.0f;
        if (negotiated.is_float) {
            cfg.format = AMPDAC_PCM_F32LE;
            negotiated.bps = 32;
        } else if (negotiated.bps <= 16) {
            cfg.format = AMPDAC_PCM_S16LE;
            negotiated.bps = 16;
            negotiated.is_float = 0;
        } else {
            cfg.format = AMPDAC_PCM_S32LE;
            negotiated.bps = 32;
            negotiated.is_float = 0;
        }
        st = ampdac_client_configure_pcm(s, vid, pid, &cfg);
    }

    if (st != AMPDAC_OK) {
        TRACE("ampdac_configure failed: %s (%s)\n",
              ampdac_ipc_status_string(st), ampdac_client_last_error());
        ampdac_client_disconnect(s);
        return -1;
    }

    st = ampdac_client_start(s);
    if (st != AMPDAC_OK) {
        TRACE("ampdac_start failed: %s (%s)\n",
              ampdac_ipc_status_string(st), ampdac_client_last_error());
        ampdac_client_disconnect(s);
        return -1;
    }

    g_session = s;
    g_unwrap_dop = unwrap;
    g_dop_carry_n = 0;
    memcpy(&plugin.fmt, &negotiated, sizeof(plugin.fmt));
    if (unwrap) {
        ampdac_negotiated neg;
        memset(&neg, 0, sizeof(neg));
        (void)ampdac_client_negotiated_format(s, &neg);
        TRACE("USB DSD %s from DoP unwrap, streamer %d Hz, USB clock %u Hz\n",
              neg.dsd_native ? "native" : "DoP",
              negotiated.samplerate, neg.sample_rate_hz);
    } else {
        TRACE("USB %s %d Hz %d ch %d bit\n",
              negotiated.is_float ? "float" : "PCM",
              negotiated.samplerate, negotiated.channels, negotiated.bps);
    }
    return 0;
}

static int apply_format(void) {
    g_setformat_requested = 0;

    if (g_requested_fmt.channels <= 0) {
        g_requested_fmt.bps = 16;
        g_requested_fmt.is_float = 0;
        g_requested_fmt.channels = 2;
        g_requested_fmt.samplerate = 44100;
        g_requested_fmt.channelmask = 3;
        g_requested_fmt.flags = 0;
    }

    if (g_session && fmt_equal(&g_requested_fmt, &plugin.fmt)) {
        return 0;
    }

    TRACE("format change → %dbit %s %dch %dHz flags=0x%x (new USB session)\n",
          g_requested_fmt.bps,
          g_requested_fmt.is_float ? "float" : "int",
          g_requested_fmt.channels,
          g_requested_fmt.samplerate,
          g_requested_fmt.flags);

    close_session();
    return open_session(&g_requested_fmt);
}

static int push_dsd_bytes(const uint8_t *p, int nbytes) {
    int off = 0;
    while (off < nbytes) {
        const int n = ampdac_client_write(g_session, p + off, (size_t)(nbytes - off));
        if (n < 0) {
            TRACE("ampdac_write: %s (%s)\n",
                  ampdac_ipc_status_string((ampdac_status)n),
                  ampdac_client_last_error());
            return -1;
        }
        if (n == 0) break;
        off += n;
    }
    return 0;
}

/* DoP S32LE stereo frame → interleaved MSBF DSD (L0,R0,L1,R1) for ampdac_write. */
static int unwrap_dop_and_write(const uint8_t *src, int nbytes) {
    uint8_t tmp[8192 + 8];
    int ntmp = 0;
    if (g_dop_carry_n > 0) {
        memcpy(tmp, g_dop_carry, (size_t)g_dop_carry_n);
        ntmp = g_dop_carry_n;
        g_dop_carry_n = 0;
    }
    if (ntmp + nbytes > (int)sizeof(tmp)) {
        nbytes = (int)sizeof(tmp) - ntmp;
    }
    memcpy(tmp + ntmp, src, (size_t)nbytes);
    ntmp += nbytes;

    uint8_t dsd[4096];
    int ndsd = 0;
    int i = 0;
    while (i + 8 <= ntmp) {
        /* LE word: pad, dsd1, dsd0, marker */
        if (ndsd + 4 > (int)sizeof(dsd)) {
            if (push_dsd_bytes(dsd, ndsd) != 0) return -1;
            ndsd = 0;
        }
        dsd[ndsd++] = tmp[i + 2]; /* L dsd0 */
        dsd[ndsd++] = tmp[i + 6]; /* R dsd0 */
        dsd[ndsd++] = tmp[i + 1]; /* L dsd1 */
        dsd[ndsd++] = tmp[i + 5]; /* R dsd1 */
        i += 8;
    }
    if (ntmp - i > 0 && ntmp - i <= (int)sizeof(g_dop_carry)) {
        memcpy(g_dop_carry, tmp + i, (size_t)(ntmp - i));
        g_dop_carry_n = ntmp - i;
    }
    return ndsd > 0 ? push_dsd_bytes(dsd, ndsd) : 0;
}

static int write_bytes(const void *data, int nbytes) {
    if (!g_session || nbytes <= 0) return 0;
    if (g_unwrap_dop) {
        return unwrap_dop_and_write((const uint8_t *)data, nbytes);
    }
    return push_dsd_bytes((const uint8_t *)data, nbytes);
}

static void audio_thread(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "deadbeef-ampdac");
    char buf[8192];

    for (;;) {
        LOCK;
        if (g_terminate) {
            UNLOCK;
            break;
        }
        if (g_setformat_requested) {
            if (apply_format() != 0) {
                g_state = DDB_PLAYBACK_STATE_STOPPED;
            }
        }
        const ddb_playback_state_t st = g_state;
        UNLOCK;

        if (st != DDB_PLAYBACK_STATE_PLAYING) {
            usleep(10000);
            continue;
        }

        if (!deadbeef->streamer_ok_to_read(-1)) {
            usleep(2000);
            continue;
        }

        const int n = deadbeef->streamer_read(buf, (int)sizeof(buf));
        if (n <= 0) {
            usleep(2000);
            continue;
        }
        /* Do not zero-pad short reads — that inserts silence between gapless tracks. */
        if (write_bytes(buf, n) != 0) {
            LOCK;
            g_state = DDB_PLAYBACK_STATE_STOPPED;
            close_session();
            UNLOCK;
        }
    }

    LOCK;
    close_session();
    g_tid = 0;
    g_terminate = 0;
    UNLOCK;
}

static int ampdac_out_init(void) {
    LOCK;
    if (g_tid) {
        UNLOCK;
        return 0;
    }
    g_terminate = 0;
    if (!g_session) {
        ddb_waveformat_t fmt = plugin.fmt;
        if (fmt.samplerate <= 0) {
            fmt.bps = 16;
            fmt.channels = 2;
            fmt.samplerate = 44100;
            fmt.channelmask = 3;
        }
        if (open_session(&fmt) != 0) {
            UNLOCK;
            return -1;
        }
    }
    g_tid = deadbeef->thread_start(audio_thread, NULL);
    UNLOCK;
    return g_tid ? 0 : -1;
}

static int ampdac_out_free(void) {
    LOCK;
    if (!g_tid) {
        close_session();
        UNLOCK;
        return 0;
    }
    g_terminate = 1;
    const intptr_t tid = g_tid;
    UNLOCK;
    deadbeef->thread_join(tid);
    return 0;
}

static int ampdac_out_setformat(ddb_waveformat_t *fmt) {
    LOCK;
    g_setformat_requested = 1;
    memcpy(&g_requested_fmt, fmt, sizeof(g_requested_fmt));
    UNLOCK;
    return 0;
}

static int ampdac_out_play(void) {
    LOCK;
    if (!g_tid) {
        UNLOCK;
        if (ampdac_out_init() != 0) return -1;
        LOCK;
    }
    g_state = DDB_PLAYBACK_STATE_PLAYING;
    UNLOCK;
    return 0;
}

static int ampdac_out_stop(void) {
    LOCK;
    g_state = DDB_PLAYBACK_STATE_STOPPED;
    UNLOCK;
    return ampdac_out_free();
}

static int ampdac_out_pause(void) {
    LOCK;
    if (!g_tid) {
        UNLOCK;
        if (ampdac_out_init() != 0) return -1;
        LOCK;
    }
    g_state = DDB_PLAYBACK_STATE_PAUSED;
    UNLOCK;
    return 0;
}

static int ampdac_out_unpause(void) {
    LOCK;
    if (!g_tid) {
        UNLOCK;
        return ampdac_out_play();
    }
    if (g_state == DDB_PLAYBACK_STATE_PAUSED) {
        g_state = DDB_PLAYBACK_STATE_PLAYING;
    }
    UNLOCK;
    return 0;
}

static ddb_playback_state_t ampdac_out_state(void) {
    LOCK;
    const ddb_playback_state_t st = g_state;
    UNLOCK;
    return st;
}

static void ampdac_out_enum_soundcards(
        void (*callback)(const char *name, const char *desc, void *),
        void *userdata) {
    if (!callback) return;
    ampdac_device_info devices[16];
    char socket_path[256];
    const int n = ampdac_client_list_devices(
            session_socket(socket_path, sizeof(socket_path)), devices, 16);
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) {
        char name[32];
        char desc[320];
        snprintf(name, sizeof(name), "%04x:%04x", devices[i].vid, devices[i].pid);
        snprintf(desc, sizeof(desc), "%s %s (bus %u addr %u)",
                 devices[i].manufacturer, devices[i].product,
                 devices[i].bus, devices[i].address);
        callback(name, desc, userdata);
    }
}

static int ampdac_plugin_start(void) {
    g_mutex = deadbeef->mutex_create();
    if (!g_mutex) return -1;
    /* Before FFmpeg's start() so the first DSF/DFF is DoP, not float dsd2pcm. */
    if (deadbeef->conf_get_int("ampdac.force_ffmpeg_dop", 1)) {
        deadbeef->conf_set_int("ffmpeg.enable_dop", 1);
    }
    return 0;
}

static int ampdac_plugin_stop(void) {
    (void)ampdac_out_free();
    if (g_mutex) {
        deadbeef->mutex_free(g_mutex);
        g_mutex = 0;
    }
    return 0;
}

static const char settings_dlg[] =
        "property \"USB vendor id (0 = first UAC2 DAC)\" entry ampdac.vid 0;\n"
        "property \"USB product id (0 = first UAC2 DAC)\" entry ampdac.pid 0;\n"
        "property \"Force FFmpeg DoP for DSD (no dsd2pcm)\" checkbox ampdac.force_ffmpeg_dop 1;\n"
        "property \"ampdacd socket (empty = /run/ampdac.sock)\" entry ampdac.socket \"\";\n";

static DB_output_t plugin = {
        DDB_PLUGIN_SET_API_VERSION
        .plugin.version_major = 0,
        .plugin.version_minor = 1,
        .plugin.type = DB_PLUGIN_OUTPUT,
        .plugin.flags = DDB_PLUGIN_FLAG_LOGGING | DDB_PLUGIN_FLAG_REPLAYGAIN,
        .plugin.id = "ampdac",
        .plugin.name = "ampdac USB DAC (ampdacd)",
        .plugin.descr =
                "Bit-perfect exclusive UAC2 output via the ampdacd daemon.\n"
                "Keeps one ISO session across same-format tracks (gapless).\n"
                "DSF/DFF: forces FFmpeg DoP, unwraps markers, native DSD if the\n"
                "DAC has a RAW alt else DoP on PCM. Disable DSP for bit-perfect.\n"
                "Start ampdacd first (systemctl enable --now ampdac.socket).\n",
        .plugin.copyright =
                "MIT — derived from Audiophile Music Player USB path.\n",
        .plugin.website = "https://github.com/androidexpert35/Audiophile-Music-Player",
        .plugin.start = ampdac_plugin_start,
        .plugin.stop = ampdac_plugin_stop,
        .plugin.configdialog = settings_dlg,
        .init = ampdac_out_init,
        .free = ampdac_out_free,
        .setformat = ampdac_out_setformat,
        .play = ampdac_out_play,
        .stop = ampdac_out_stop,
        .pause = ampdac_out_pause,
        .unpause = ampdac_out_unpause,
        .state = ampdac_out_state,
        .enum_soundcards = ampdac_out_enum_soundcards,
        .has_volume = 1,
};

DB_plugin_t *ddb_out_ampdac_load(DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN(&plugin);
}

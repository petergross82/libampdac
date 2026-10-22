/**
 * ALSA PCM plugin "ampdac".
 *
 * Players open the device name ampdac. This .so only forwards hw_params and
 * PCM bytes to ampdacd. Install as libasound_module_pcm_ampdac.so.
 */

#include "ampdac_ipc.h"

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

struct ampdac_pcm {
    snd_pcm_ioplug_t io;
    ampdac_client *client;
    char socket_path[108];
    uint16_t vid;
    uint16_t pid;
    /* Frames handed to the daemon since prepare, and frames the DAC has
     * consumed. ALSA resets its hw pointer in prepare; both restart there. */
    snd_pcm_uframes_t written;
    snd_pcm_uframes_t played;
    snd_pcm_uframes_t anchor_played;
    uint64_t anchor_ns;
    uint64_t stats_ns;
    /* Wire bytes of one ALSA frame. The open alternate sets the slot width.
     * One DSD_U32 frame becomes two DoP frames. */
    unsigned int wire_bpf;
    int clock_on;
    int started;
    /* PortAudio (Audacity) will not start a stream that has no poll fd.
     * The timer fires when the next period of buffer space is free. */
    int timer_fd;
};

static unsigned int frame_bytes(const snd_pcm_ioplug_t *io) {
    const int bits = snd_pcm_format_physical_width(io->format);
    if (bits <= 0) return 0;
    return (unsigned int)(bits / 8) * io->channels;
}

static int ensure_client(struct ampdac_pcm *pcm) {
    if (pcm->client != NULL) return 0;
    pcm->client = ampdac_client_connect(
            pcm->socket_path[0] ? pcm->socket_path : NULL);
    if (pcm->client == NULL) {
        SNDERR("ampdac: %s", ampdac_client_last_error());
        return -ENOENT;
    }
    return 0;
}

static int cb_close(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    ampdac_client_disconnect(pcm->client);
    pcm->client = NULL;
    if (pcm->timer_fd >= 0) close(pcm->timer_fd);
    free(pcm);
    return 0;
}

static void timer_drain(int fd) {
    uint64_t expired;
    while (read(fd, &expired, sizeof expired) > 0) {}
}

/* How long until at least one period can be written.
 * min_ns is 1 when poll() should return immediately, and at least 1 ms when
 * re-arming from poll_revents: MPD keeps the fd and does not call
 * poll_descriptors again, so a spent one-shot timer never wakes it. */
static void arm_poll_timer(struct ampdac_pcm *pcm, uint64_t min_ns) {
    struct itimerspec ts;
    uint64_t ns = 1000000ull;
    memset(&ts, 0, sizeof ts);
    if (pcm->timer_fd < 0) return;
    if (min_ns < 1ull) min_ns = 1ull;
    timer_drain(pcm->timer_fd);
    if (pcm->io.pcm != NULL && pcm->io.rate > 0 && pcm->io.period_size > 0) {
        const snd_pcm_sframes_t avail = snd_pcm_avail(pcm->io.pcm);
        if (avail < 0 || (snd_pcm_uframes_t)avail >= pcm->io.period_size) {
            ns = min_ns;
        } else {
            const snd_pcm_uframes_t need =
                    pcm->io.period_size - (snd_pcm_uframes_t)avail;
            ns = (uint64_t)need * 1000000000ull / pcm->io.rate;
            if (ns < 500000ull) ns = 500000ull;
            if (ns < min_ns) ns = min_ns;
            if (ns > 50000000ull) ns = 50000000ull;
        }
    }
    ts.it_value.tv_sec = (time_t)(ns / 1000000000ull);
    ts.it_value.tv_nsec = (long)(ns % 1000000000ull);
    if (timerfd_settime(pcm->timer_fd, 0, &ts, NULL) < 0) {
        SNDERR("ampdac: timerfd_settime: %s", strerror(errno));
    }
}

static int cb_poll_descriptors_count(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    return pcm->timer_fd >= 0 ? 1 : 0;
}

static int cb_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd,
                               unsigned int space) {
    struct ampdac_pcm *pcm = io->private_data;
    if (pcm->timer_fd < 0) return 0;
    if (space < 1 || pfd == NULL) return 0;
    arm_poll_timer(pcm, 1ull);
    pfd[0].fd = pcm->timer_fd;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    return 1;
}

static int cb_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
                           unsigned int nfds, unsigned short *revents) {
    struct ampdac_pcm *pcm = io->private_data;
    (void)nfds;
    if (pfd != NULL && (pfd[0].revents & POLLIN)) timer_drain(pcm->timer_fd);
    /* MPD's ALSA output leaves this fd registered and will not ask for a
     * new descriptor until the set changes. Re-arm here or the one-shot
     * fires once, reports nothing while the buffer is still full, and
     * playback stops after the first buffer. */
    arm_poll_timer(pcm, 1000000ull);
    *revents = 0;
    if (pcm->io.pcm != NULL && pcm->io.period_size > 0) {
        const snd_pcm_sframes_t avail = snd_pcm_avail(pcm->io.pcm);
        if (avail < 0 || (snd_pcm_uframes_t)avail >= pcm->io.period_size)
            *revents = POLLOUT;
    } else {
        *revents = POLLOUT;
    }
    return 0;
}

static int cb_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
    (void)params;
    struct ampdac_pcm *pcm = io->private_data;
    const int rc = ensure_client(pcm);
    if (rc != 0) return rc;

    ampdac_status st = AMPDAC_ERR_INVALID;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (io->format == SND_PCM_FORMAT_DSD_U32_LE) {
            ampdac_dsd_config cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.dsd_rate_hz = io->rate * 32u;
            cfg.channels = (uint8_t)io->channels;
            cfg.transport = AMPDAC_DSD_AUTO;
            st = ampdac_client_configure_dsd(pcm->client, pcm->vid, pcm->pid, &cfg);
        } else {
            ampdac_pcm_config cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.sample_rate_hz = io->rate;
            cfg.channels = (uint8_t)io->channels;
            cfg.volume = 1.0f;
            if (io->format == SND_PCM_FORMAT_S16_LE) cfg.format = AMPDAC_PCM_S16LE;
            else if (io->format == SND_PCM_FORMAT_S24_3LE) cfg.format = AMPDAC_PCM_S24_3LE;
            else if (io->format == SND_PCM_FORMAT_FLOAT_LE) cfg.format = AMPDAC_PCM_F32LE;
            else cfg.format = AMPDAC_PCM_S32LE;
            st = ampdac_client_configure_pcm(pcm->client, pcm->vid, pcm->pid, &cfg);
        }
        /* The previous track's socket is still releasing USB. */
        if (st != AMPDAC_ERR_BUSY) break;
        usleep(100000);
    }
    if (st != AMPDAC_OK) {
        SNDERR("ampdac: %s (%s)", ampdac_ipc_status_string(st),
               ampdac_client_last_error());
        if (st == AMPDAC_ERR_BUSY) return -EBUSY;
        if (st == AMPDAC_ERR_NO_DEVICE) return -ENODEV;
        return -EINVAL;
    }
    pcm->wire_bpf = frame_bytes(io);
    if (io->format != SND_PCM_FORMAT_DSD_U32_LE) {
        ampdac_negotiated neg;
        memset(&neg, 0, sizeof(neg));
        if (ampdac_client_negotiated_format(pcm->client, &neg) == AMPDAC_OK &&
            neg.subslot_bytes > 0 && neg.channels > 0) {
            pcm->wire_bpf = (unsigned int)neg.channels * neg.subslot_bytes;
        } else if (io->format == SND_PCM_FORMAT_S16_LE) {
            pcm->wire_bpf *= 2u;
        } else if (io->format == SND_PCM_FORMAT_S24_3LE && pcm->wire_bpf > 0u) {
            pcm->wire_bpf = (pcm->wire_bpf / 3u) * 4u;
        }
    } else if (io->format == SND_PCM_FORMAT_DSD_U32_LE) {
        ampdac_negotiated neg;
        memset(&neg, 0, sizeof(neg));
        if (ampdac_client_negotiated_format(pcm->client, &neg) == AMPDAC_OK &&
            neg.dsd && !neg.dsd_native) {
            pcm->wire_bpf *= 2u;
        }
    }
    pcm->started = 0;
    return 0;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cb_prepare(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    /* prepare() has already zeroed ALSA's hw pointer. Match it. */
    pcm->written = io->hw_ptr;
    pcm->played = io->hw_ptr;
    pcm->anchor_played = io->hw_ptr;
    pcm->anchor_ns = mono_ns();
    pcm->stats_ns = 0;
    pcm->clock_on = 0;
    return 0;
}

static int cb_hw_free(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    if (pcm->client != NULL) {
        (void)ampdac_client_release(pcm->client);
    }
    pcm->started = 0;
    return 0;
}

static int cb_start(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    const int rc = ensure_client(pcm);
    if (rc != 0) return rc;
    const ampdac_status st = ampdac_client_start(pcm->client);
    if (st != AMPDAC_OK) {
        SNDERR("ampdac: start: %s", ampdac_client_last_error());
        return -EIO;
    }
    pcm->started = 1;
    return 0;
}

static int cb_stop(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    /* drop() means the queued track is abandoned. Silence the ring now,
     * otherwise DeaDBeeF keeps hearing it until the DAC finishes the queue. */
    if (pcm->client != NULL) {
        (void)ampdac_client_drop(pcm->client);
    }
    pcm->written = pcm->played;
    pcm->clock_on = 0;
    return 0;
}

static snd_pcm_uframes_t frames_played(struct ampdac_pcm *pcm, snd_pcm_ioplug_t *io) {
    snd_pcm_uframes_t est = pcm->played;
    const uint64_t now = mono_ns();
    if (pcm->clock_on && io->rate > 0) {
        const uint64_t dt = now - pcm->anchor_ns;
        est = pcm->anchor_played +
                (snd_pcm_uframes_t)((dt * (uint64_t)io->rate) / 1000000000ull);
        if (est > pcm->written) est = pcm->written;
        if (est < pcm->played) est = pcm->played;
    }

    /* The ring is what the DAC has not taken yet. A fast local clock must
     * not run past it, or DeaDBeeF will fill the queue for seconds. */
    if (pcm->client != NULL && pcm->clock_on &&
        now - pcm->stats_ns >= 20000000ull) {
        ampdac_stream_stats stats;
        memset(&stats, 0, sizeof(stats));
        pcm->stats_ns = now;
        const unsigned int wbpf = pcm->wire_bpf;
        if (wbpf > 0 &&
            ampdac_client_get_stats(pcm->client, &stats) == AMPDAC_OK) {
            const snd_pcm_uframes_t queued = stats.ring_bytes / wbpf;
            const snd_pcm_uframes_t consumed =
                    pcm->written > queued ? pcm->written - queued : 0;
            /* Follow the DAC both ways. A host clock that lags 44.1 kHz
             * used to leave the pointer behind, ALSA stopped refilling,
             * and the ring ran dry — one held sample, one click. */
            if (est > consumed) est = consumed;
            if (consumed > est) est = consumed;
            if (est < pcm->played) est = pcm->played;
            if (est > pcm->written) est = pcm->written;
            pcm->anchor_played = est;
            pcm->anchor_ns = now;
        }
    }
    pcm->played = est;
    return est;
}

static snd_pcm_sframes_t cb_pointer(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    snd_pcm_uframes_t hw = frames_played(pcm, io);
    /* Delay must stay inside the ALSA buffer. A larger delay wraps avail
     * and DeaDBeeF then writes that many frames while holding its mutex. */
    if (io->buffer_size > 0 && io->appl_ptr > io->buffer_size) {
        const snd_pcm_uframes_t min_hw = io->appl_ptr - io->buffer_size;
        if (hw < min_hw) hw = min_hw;
    }
    if (hw < io->hw_ptr) hw = io->hw_ptr;
    if (hw > pcm->written) hw = pcm->written;
    pcm->played = hw;
    return (snd_pcm_sframes_t)hw;
}

/* MPD's DSD_U32_LE puts the oldest DSD byte in the high byte of the
 * host word, so on little endian each channel arrives newest-first:
 * L3 L2 L1 L0 R3 R2 R1 R0, L0 being the oldest. FiiO raw DSD is the
 * kernel's DSD_U32_BE order: oldest byte first on the USB wire.
 * Rebuild byte-interleaved MSBF (L0 R0 L1 R1 …) for the daemon; its
 * formatter then emits L0 L1 L2 L3 R0 R1 R2 R3. */
static void dsd_u32_to_byte_interleaved(const unsigned char *src,
                                        unsigned char *dst,
                                        snd_pcm_uframes_t frames) {
    snd_pcm_uframes_t i;
    for (i = 0; i < frames; ++i, src += 8, dst += 8) {
        dst[0] = src[3];
        dst[1] = src[7];
        dst[2] = src[2];
        dst[3] = src[6];
        dst[4] = src[1];
        dst[5] = src[5];
        dst[6] = src[0];
        dst[7] = src[4];
    }
}

static snd_pcm_sframes_t cb_transfer(snd_pcm_ioplug_t *io,
                                     const snd_pcm_channel_area_t *areas,
                                     snd_pcm_uframes_t offset,
                                     snd_pcm_uframes_t size) {
    struct ampdac_pcm *pcm = io->private_data;
    const unsigned int bpf = frame_bytes(io);
    if (bpf == 0 || pcm->client == NULL || size == 0) return -EIO;
    /* aplay/MPD prefill the buffer before the start callback. */
    if (!pcm->started) {
        const int rc = cb_start(io);
        if (rc < 0) return rc;
    }
    if (io->buffer_size > 0 && size > io->buffer_size) {
        size = io->buffer_size;
    }

    const char *base = (const char *)areas[0].addr + (areas[0].first / 8);
    if (areas[0].step != bpf * 8u) {
        SNDERR("ampdac: non-interleaved transfer is not supported");
        return -EINVAL;
    }
    const char *src = base + offset * bpf;
    /* Native DSD only. DoP keeps the ALSA bytes: the daemon reads them
     * as raw DSD and inserts markers itself. wire_bpf doubles for DoP. */
    const int native_dsd = io->format == SND_PCM_FORMAT_DSD_U32_LE &&
            pcm->wire_bpf == bpf;
    size_t left = (size_t)size * bpf;
    size_t done = 0;
    unsigned char dsd_tmp[8 * 256];
    while (left > 0) {
        const char *chunk = src + done;
        size_t chunk_len = left;
        if (native_dsd) {
            const size_t max_bytes = sizeof dsd_tmp;
            if (chunk_len > max_bytes) chunk_len = max_bytes;
            chunk_len -= chunk_len % 8u;
            dsd_u32_to_byte_interleaved((const unsigned char *)chunk,
                                        dsd_tmp,
                                        (snd_pcm_uframes_t)(chunk_len / 8u));
            chunk = (const char *)dsd_tmp;
        }
        size_t sent = 0;
        while (sent < chunk_len) {
            const int n = ampdac_client_write(pcm->client,
                                              chunk + sent,
                                              chunk_len - sent);
            if (n < 0) {
                SNDERR("ampdac: write: %s", ampdac_client_last_error());
                return n == AMPDAC_ERR_TIMEOUT ? -EPIPE : -EIO;
            }
            if (n == 0) return -EPIPE;
            sent += (size_t)n;
        }
        done += chunk_len;
        left -= chunk_len;
    }
    pcm->written += (snd_pcm_uframes_t)size;
    if (!pcm->clock_on) {
        pcm->clock_on = 1;
        pcm->anchor_ns = mono_ns();
        pcm->anchor_played = pcm->played;
    }
    return (snd_pcm_sframes_t)size;
}

static int cb_drain(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    const unsigned int bpf = frame_bytes(io);
    if (pcm->client == NULL || bpf == 0) return 0;
    for (int i = 0; i < 50; ++i) {
        ampdac_stream_stats stats;
        memset(&stats, 0, sizeof(stats));
        if (ampdac_client_get_stats(pcm->client, &stats) != AMPDAC_OK) break;
        if (stats.ring_bytes < bpf * 64u) break;
        usleep(20000);
    }
    return 0;
}

static const snd_pcm_ioplug_callback_t k_callbacks = {
    .start = cb_start,
    .stop = cb_stop,
    .pointer = cb_pointer,
    .transfer = cb_transfer,
    .close = cb_close,
    .hw_params = cb_hw_params,
    .hw_free = cb_hw_free,
    .prepare = cb_prepare,
    .drain = cb_drain,
    .poll_descriptors_count = cb_poll_descriptors_count,
    .poll_descriptors = cb_poll_descriptors,
    .poll_revents = cb_poll_revents,
};

static int ampdac_pcm_open(snd_pcm_t **pcmp, const char *name,
                           snd_pcm_stream_t stream, int mode,
                           const char *socket_path, uint16_t vid, uint16_t pid) {
    if (stream != SND_PCM_STREAM_PLAYBACK) return -EINVAL;

    struct ampdac_pcm *pcm = calloc(1, sizeof(*pcm));
    if (pcm == NULL) return -ENOMEM;
    if (socket_path != NULL) {
        snprintf(pcm->socket_path, sizeof(pcm->socket_path), "%s", socket_path);
    }
    pcm->vid = vid;
    pcm->pid = pid;
    pcm->timer_fd = -1;
    pcm->io.version = SND_PCM_IOPLUG_VERSION;
    pcm->io.name = "ampdac";
    pcm->io.flags = SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;
    pcm->io.callback = &k_callbacks;
    pcm->io.private_data = pcm;

    int err = snd_pcm_ioplug_create(&pcm->io, name, stream, mode);
    if (err < 0) {
        free(pcm);
        return err;
    }
    pcm->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (pcm->timer_fd < 0) {
        SNDERR("ampdac: timerfd_create: %s", strerror(errno));
    }

    static const unsigned int accesses[] = {
        SND_PCM_ACCESS_MMAP_INTERLEAVED,
        SND_PCM_ACCESS_RW_INTERLEAVED,
    };
    static const unsigned int formats[] = {
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_FORMAT_DSD_U32_LE,
    };
    static const unsigned int rates[] = {
        44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000,
    };

    err = snd_pcm_ioplug_set_param_list(&pcm->io, SND_PCM_IOPLUG_HW_ACCESS, 2, accesses);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_list(&pcm->io, SND_PCM_IOPLUG_HW_FORMAT, 5, formats);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_list(&pcm->io, SND_PCM_IOPLUG_HW_RATE, 10, rates);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_CHANNELS, 2, 2);
    if (err < 0) goto fail;
    /* Deep enough that the DAC is still fed while DeaDBeeF sleeps a period.
     * The pointer follows real consumption, so the player refills one period
     * at a time and releases its mutex between refills. A 16-frame period
     * is shorter than one USB scheduling quantum and underruns. */
    err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 4096, 32768);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 65536, 262144);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_PERIODS, 4, 8);
    if (err < 0) goto fail;

    *pcmp = pcm->io.pcm;
    return 0;

fail:
    snd_pcm_ioplug_delete(&pcm->io);
    return err;
}

SND_PCM_PLUGIN_DEFINE_FUNC(ampdac) {
    const char *socket_path = NULL;
    long vid = 0;
    long pid = 0;
    snd_config_iterator_t i, next;

    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;
        if (snd_config_get_id(n, &id) < 0) continue;
        if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 ||
            strcmp(id, "hint") == 0) {
            continue;
        }
        if (strcmp(id, "socket") == 0) {
            if (snd_config_get_string(n, &socket_path) < 0) {
                SNDERR("ampdac: socket must be a string");
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(id, "vid") == 0) {
            if (snd_config_get_integer(n, &vid) < 0) return -EINVAL;
            continue;
        }
        if (strcmp(id, "pid") == 0) {
            if (snd_config_get_integer(n, &pid) < 0) return -EINVAL;
            continue;
        }
        SNDERR("ampdac: unknown field %s", id);
        return -EINVAL;
    }

    (void)root;
    return ampdac_pcm_open(pcmp, name, stream, mode, socket_path,
                           (uint16_t)vid, (uint16_t)pid);
}

SND_PCM_PLUGIN_SYMBOL(ampdac);

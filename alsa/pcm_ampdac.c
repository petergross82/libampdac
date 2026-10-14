/**
 * ALSA PCM plugin "ampdac".
 *
 * Players open the device name ampdac. This .so only forwards hw_params and
 * PCM bytes to ampdacd. Install as libasound_module_pcm_ampdac.so.
 */

#include "ampdac_ipc.h"

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ampdac_pcm {
    snd_pcm_ioplug_t io;
    ampdac_client *client;
    char socket_path[108];
    uint16_t vid;
    uint16_t pid;
    unsigned int hw_ptr;
    int started;
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
    free(pcm);
    return 0;
}

static int cb_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
    (void)params;
    struct ampdac_pcm *pcm = io->private_data;
    const int rc = ensure_client(pcm);
    if (rc != 0) return rc;

    ampdac_status st;
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
        else if (io->format == SND_PCM_FORMAT_FLOAT_LE) cfg.format = AMPDAC_PCM_F32LE;
        else cfg.format = AMPDAC_PCM_S32LE;
        st = ampdac_client_configure_pcm(pcm->client, pcm->vid, pcm->pid, &cfg);
    }
    if (st != AMPDAC_OK) {
        SNDERR("ampdac: %s (%s)", ampdac_ipc_status_string(st),
               ampdac_client_last_error());
        if (st == AMPDAC_ERR_BUSY) return -EBUSY;
        if (st == AMPDAC_ERR_NO_DEVICE) return -ENODEV;
        return -EINVAL;
    }
    pcm->started = 0;
    pcm->hw_ptr = 0;
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
    (void)io;
    return 0;
}

static snd_pcm_sframes_t cb_pointer(snd_pcm_ioplug_t *io) {
    struct ampdac_pcm *pcm = io->private_data;
    return pcm->hw_ptr;
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

    const char *base = (const char *)areas[0].addr + (areas[0].first / 8);
    if (areas[0].step != bpf * 8u) {
        SNDERR("ampdac: non-interleaved transfer is not supported");
        return -EINVAL;
    }
    const char *src = base + offset * bpf;
    size_t left = (size_t)size * bpf;
    size_t done = 0;
    while (left > 0) {
        const int n = ampdac_client_write(pcm->client, src + done, left);
        if (n < 0) {
            SNDERR("ampdac: write: %s", ampdac_client_last_error());
            return n == AMPDAC_ERR_TIMEOUT ? -EPIPE : -EIO;
        }
        if (n == 0) return -EPIPE;
        done += (size_t)n;
        left -= (size_t)n;
    }
    pcm->hw_ptr += (unsigned int)size;
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
    .drain = cb_drain,
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

    static const unsigned int accesses[] = {
        SND_PCM_ACCESS_MMAP_INTERLEAVED,
        SND_PCM_ACCESS_RW_INTERLEAVED,
    };
    static const unsigned int formats[] = {
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_FORMAT_DSD_U32_LE,
    };
    static const unsigned int rates[] = {
        44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000,
    };

    err = snd_pcm_ioplug_set_param_list(&pcm->io, SND_PCM_IOPLUG_HW_ACCESS, 2, accesses);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_list(&pcm->io, SND_PCM_IOPLUG_HW_FORMAT, 4, formats);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_list(&pcm->io, SND_PCM_IOPLUG_HW_RATE, 10, rates);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_CHANNELS, 2, 2);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 1024, 65536);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 8192, 262144);
    if (err < 0) goto fail;
    err = snd_pcm_ioplug_set_param_minmax(&pcm->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 8);
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

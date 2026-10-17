#include "ampdac_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>

struct ampdac_client {
    int fd;
};

static _Thread_local char g_client_error[512];

static void set_client_error(const char *message) {
    if (message == NULL) message = "";
    snprintf(g_client_error, sizeof(g_client_error), "%s", message);
}

const char *ampdac_client_last_error(void) {
    return g_client_error;
}

static int recv_hdr(int fd, struct ampdac_ipc_hdr *hdr, uint8_t *payload, uint32_t cap) {
    if (ampdac_ipc_read_full(fd, hdr, sizeof(*hdr)) != 0) {
        set_client_error("ampdacd closed the socket");
        return -1;
    }
    if (hdr->magic != AMPDAC_IPC_MAGIC || hdr->version != AMPDAC_IPC_VERSION) {
        set_client_error("ampdacd protocol mismatch");
        return -1;
    }
    if (hdr->length > cap) {
        set_client_error("ampdacd reply is too large");
        return -1;
    }
    if (hdr->length > 0 &&
        ampdac_ipc_read_full(fd, payload, hdr->length) != 0) {
        set_client_error("ampdacd closed the socket");
        return -1;
    }
    if (hdr->status < 0 && hdr->length > 0) {
        payload[hdr->length < cap ? hdr->length : cap - 1] = '\0';
        set_client_error((const char *)payload);
    } else if (hdr->status < 0) {
        set_client_error(ampdac_ipc_status_string((ampdac_status)hdr->status));
    }
    return 0;
}

ampdac_client *ampdac_client_connect(const char *socket_path) {
    const char *path = (socket_path != NULL && socket_path[0] != '\0')
            ? socket_path
            : ampdac_client_default_socket();
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        set_client_error("socket path is too long");
        return NULL;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        set_client_error("socket() failed");
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(g_client_error, sizeof(g_client_error),
                 "ampdacd is not running (%s)", path);
        close(fd);
        return NULL;
    }

    ampdac_client *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        set_client_error("out of memory");
        close(fd);
        return NULL;
    }
    client->fd = fd;
    g_client_error[0] = '\0';
    return client;
}

void ampdac_client_disconnect(ampdac_client *client) {
    if (client == NULL) return;
    if (client->fd >= 0) {
        (void)ampdac_ipc_send(client->fd, AMPDAC_IPC_CLOSE, 0, NULL, 0);
        struct ampdac_ipc_hdr hdr;
        uint8_t sink[256];
        (void)recv_hdr(client->fd, &hdr, sink, sizeof(sink));
        close(client->fd);
    }
    free(client);
}

static ampdac_status rpc_empty(ampdac_client *client, uint16_t type,
                               const void *payload, uint32_t length,
                               void *out, uint32_t out_cap, uint32_t *out_len) {
    if (client == NULL || client->fd < 0) {
        set_client_error("not connected to ampdacd");
        return AMPDAC_ERR_STATE;
    }
    if (ampdac_ipc_send(client->fd, type, 0, payload, length) != 0) {
        set_client_error("write to ampdacd failed");
        return AMPDAC_ERR_IO;
    }
    struct ampdac_ipc_hdr hdr;
    uint8_t buf[2048];
    if (recv_hdr(client->fd, &hdr, buf, sizeof(buf)) != 0) return AMPDAC_ERR_IO;
    if (out_len) *out_len = hdr.length;
    if (hdr.status < 0) return (ampdac_status)hdr.status;
    if (out != NULL && hdr.length > 0) {
        if (hdr.length > out_cap) {
            set_client_error("ampdacd reply is too large");
            return AMPDAC_ERR_IO;
        }
        memcpy(out, buf, hdr.length);
    }
    return (ampdac_status)hdr.status;
}

int ampdac_client_list_devices(const char *socket_path,
                               ampdac_device_info *out,
                               int max_devices) {
    if (out == NULL || max_devices <= 0) {
        set_client_error("ampdac_client_list_devices: invalid buffer");
        return AMPDAC_ERR_INVALID;
    }
    ampdac_client *client = ampdac_client_connect(socket_path);
    if (client == NULL) return AMPDAC_ERR_IO;

    const uint32_t want = (uint32_t)max_devices;
    if (ampdac_ipc_send(client->fd, AMPDAC_IPC_LIST, 0, &want, sizeof(want)) != 0) {
        set_client_error("write to ampdacd failed");
        ampdac_client_disconnect(client);
        return AMPDAC_ERR_IO;
    }

    struct ampdac_ipc_hdr hdr;
    const uint32_t cap = (uint32_t)max_devices * (uint32_t)sizeof(ampdac_device_info);
    if (recv_hdr(client->fd, &hdr, (uint8_t *)out, cap) != 0) {
        close(client->fd);
        free(client);
        return AMPDAC_ERR_IO;
    }
    close(client->fd);
    free(client);
    if (hdr.status < 0) return hdr.status;
    return hdr.status;
}

ampdac_status ampdac_client_configure_pcm(ampdac_client *client,
                                          uint16_t vid,
                                          uint16_t pid,
                                          const ampdac_pcm_config *config) {
    if (config == NULL) {
        set_client_error("configure_pcm: missing config");
        return AMPDAC_ERR_INVALID;
    }
    struct ampdac_ipc_pcm msg;
    memset(&msg, 0, sizeof(msg));
    msg.vid = vid;
    msg.pid = pid;
    msg.sample_rate_hz = config->sample_rate_hz;
    msg.channels = config->channels;
    msg.format = (uint8_t)config->format;
    float volume = config->volume;
    if (volume < 0.f) volume = 0.f;
    if (volume > 1.f) volume = 1.f;
    msg.volume_ppm = (uint32_t)(volume * (float)AMPDAC_IPC_VOLUME_UNITY + 0.5f);
    return rpc_empty(client, AMPDAC_IPC_CONFIGURE_PCM, &msg, sizeof(msg), NULL, 0, NULL);
}

ampdac_status ampdac_client_configure_dsd(ampdac_client *client,
                                          uint16_t vid,
                                          uint16_t pid,
                                          const ampdac_dsd_config *config) {
    if (config == NULL) {
        set_client_error("configure_dsd: missing config");
        return AMPDAC_ERR_INVALID;
    }
    struct ampdac_ipc_dsd msg;
    memset(&msg, 0, sizeof(msg));
    msg.vid = vid;
    msg.pid = pid;
    msg.dsd_rate_hz = config->dsd_rate_hz;
    msg.channels = config->channels;
    msg.transport = (uint8_t)config->transport;
    return rpc_empty(client, AMPDAC_IPC_CONFIGURE_DSD, &msg, sizeof(msg), NULL, 0, NULL);
}

ampdac_status ampdac_client_start(ampdac_client *client) {
    return rpc_empty(client, AMPDAC_IPC_START, NULL, 0, NULL, 0, NULL);
}

ampdac_status ampdac_client_drop(ampdac_client *client) {
    return rpc_empty(client, AMPDAC_IPC_DROP, NULL, 0, NULL, 0, NULL);
}

int ampdac_client_write(ampdac_client *client, const void *pcm, size_t bytes) {
    if (client == NULL || client->fd < 0 || pcm == NULL) {
        set_client_error("ampdac_client_write: invalid arguments");
        return AMPDAC_ERR_INVALID;
    }
    if (bytes == 0) return 0;
    if (bytes > AMPDAC_IPC_MAX_PAYLOAD) bytes = AMPDAC_IPC_MAX_PAYLOAD;

    if (ampdac_ipc_send(client->fd, AMPDAC_IPC_WRITE, 0, pcm, (uint32_t)bytes) != 0) {
        set_client_error("write to ampdacd failed");
        return AMPDAC_ERR_IO;
    }
    struct ampdac_ipc_hdr hdr;
    uint8_t errbuf[512];
    if (recv_hdr(client->fd, &hdr, errbuf, sizeof(errbuf)) != 0) return AMPDAC_ERR_IO;
    return hdr.status;
}

ampdac_status ampdac_client_negotiated_format(ampdac_client *client,
                                              ampdac_negotiated *out) {
    if (out == NULL) return AMPDAC_ERR_INVALID;
    uint32_t got = 0;
    const ampdac_status st = rpc_empty(
            client, AMPDAC_IPC_NEGOTIATED, NULL, 0,
            out, sizeof(*out), &got);
    if (st != AMPDAC_OK) return st;
    if (got != sizeof(*out)) {
        set_client_error("short negotiated-format reply");
        return AMPDAC_ERR_IO;
    }
    return AMPDAC_OK;
}

ampdac_status ampdac_client_get_stats(ampdac_client *client,
                                      ampdac_stream_stats *out) {
    if (out == NULL) return AMPDAC_ERR_INVALID;
    uint32_t got = 0;
    const ampdac_status st = rpc_empty(
            client, AMPDAC_IPC_STATS, NULL, 0,
            out, sizeof(*out), &got);
    if (st != AMPDAC_OK) return st;
    if (got != sizeof(*out)) {
        set_client_error("short stats reply");
        return AMPDAC_ERR_IO;
    }
    return AMPDAC_OK;
}

ampdac_status ampdac_client_release(ampdac_client *client) {
    return rpc_empty(client, AMPDAC_IPC_CLOSE, NULL, 0, NULL, 0, NULL);
}

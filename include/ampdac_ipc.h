/**
 * Local socket between players and ampdacd.
 *
 * Players do not open the DAC themselves. ampdacd owns libusb, the ISO
 * schedule, and SCHED_FIFO. One connection owns the DAC until it disconnects;
 * a second CONFIGURE gets AMPDAC_ERR_BUSY. LIST is allowed during playback.
 *
 * Wire format is host-endian. Client and daemon are the same build, on one
 * machine. AMPDAC_IPC_VERSION must match or the peer rejects the header.
 */
#pragma once

#include "ampdac.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMPDAC_IPC_VERSION 1u
#define AMPDAC_DEFAULT_SOCKET "/run/ampdac.sock"
/** 'AMPD' as a little-endian uint32. */
#define AMPDAC_IPC_MAGIC 0x44504D41u
#define AMPDAC_IPC_MAX_PAYLOAD (64u * 1024u)
#define AMPDAC_IPC_VOLUME_UNITY 1000000u

enum ampdac_ipc_type {
    AMPDAC_IPC_LIST = 1,
    AMPDAC_IPC_CONFIGURE_PCM = 2,
    AMPDAC_IPC_CONFIGURE_DSD = 3,
    AMPDAC_IPC_START = 4,
    AMPDAC_IPC_WRITE = 5,
    AMPDAC_IPC_CLOSE = 6,
    AMPDAC_IPC_NEGOTIATED = 7,
    AMPDAC_IPC_STATS = 8,
};

struct ampdac_ipc_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    int32_t status;
    uint32_t length;
};

struct ampdac_ipc_pcm {
    uint16_t vid;
    uint16_t pid;
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t format;
    uint8_t pad[2];
    uint32_t volume_ppm;
};

struct ampdac_ipc_dsd {
    uint16_t vid;
    uint16_t pid;
    uint32_t dsd_rate_hz;
    uint8_t channels;
    uint8_t transport;
    uint8_t pad[2];
};

typedef struct ampdac_client ampdac_client;

static inline const char *ampdac_client_default_socket(void) {
    const char *env = getenv("AMPDAC_SOCKET");
    if (env != NULL && env[0] != '\0') return env;
    return AMPDAC_DEFAULT_SOCKET;
}

const char *ampdac_client_last_error(void);

static inline const char *ampdac_ipc_status_string(ampdac_status status) {
    switch (status) {
        case AMPDAC_OK: return "ok";
        case AMPDAC_ERR_NO_DEVICE: return "no UAC2 DAC found";
        case AMPDAC_ERR_LIBUSB: return "libusb error";
        case AMPDAC_ERR_ACCESS: return "permission denied";
        case AMPDAC_ERR_BUSY: return "USB interface busy";
        case AMPDAC_ERR_UNSUPPORTED: return "device or format unsupported";
        case AMPDAC_ERR_FORMAT: return "PCM format rejected";
        case AMPDAC_ERR_CLOCK: return "UAC2 clock programming failed";
        case AMPDAC_ERR_IO: return "USB I/O error";
        case AMPDAC_ERR_TIMEOUT: return "ring back-pressure timeout";
        case AMPDAC_ERR_STATE: return "illegal session state";
        case AMPDAC_ERR_NOMEM: return "out of memory";
        case AMPDAC_ERR_INVALID: return "invalid argument";
        default: return "unknown error";
    }
}

/** Connect to a running ampdacd. NULL path uses AMPDAC_SOCKET or the default. */
ampdac_client *ampdac_client_connect(const char *socket_path);

/** Release the DAC (if this connection owns it) and close the socket. */
void ampdac_client_disconnect(ampdac_client *client);

int ampdac_client_list_devices(const char *socket_path,
                               ampdac_device_info *out,
                               int max_devices);

ampdac_status ampdac_client_configure_pcm(ampdac_client *client,
                                          uint16_t vid,
                                          uint16_t pid,
                                          const ampdac_pcm_config *config);

ampdac_status ampdac_client_configure_dsd(ampdac_client *client,
                                          uint16_t vid,
                                          uint16_t pid,
                                          const ampdac_dsd_config *config);

ampdac_status ampdac_client_start(ampdac_client *client);

int ampdac_client_write(ampdac_client *client, const void *pcm, size_t bytes);

ampdac_status ampdac_client_negotiated_format(ampdac_client *client,
                                              ampdac_negotiated *out);

ampdac_status ampdac_client_get_stats(ampdac_client *client,
                                      ampdac_stream_stats *out);

/** Drop the USB session and keep the socket. The connection still owns the slot. */
ampdac_status ampdac_client_release(ampdac_client *client);

static inline int ampdac_ipc_write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        const ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static inline int ampdac_ipc_read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        const ssize_t n = read(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static inline int ampdac_ipc_send(int fd, uint16_t type, int32_t status,
                                  const void *payload, uint32_t length) {
    struct ampdac_ipc_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = AMPDAC_IPC_MAGIC;
    hdr.version = (uint16_t)AMPDAC_IPC_VERSION;
    hdr.type = type;
    hdr.status = status;
    hdr.length = length;
    if (ampdac_ipc_write_full(fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (length > 0 && payload != NULL &&
        ampdac_ipc_write_full(fd, payload, length) != 0) {
        return -1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

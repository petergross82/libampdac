/**
 * ampdac — userspace UAC2 USB DAC output library for Linux.
 *
 * Extracted from Audiophile Music Player's libusb isochronous path.
 * The host player decodes audio; this library claims a class-compliant
 * USB Audio Class 2 DAC, programs its clock, and streams bit-perfect
 * PCM or DSD (native DSD_U32LE, else DoP) over isochronous OUT transfers.
 *
 * Typical plugin integration:
 *
 *   ampdac_session *s = ampdac_open(0, 0);          // first UAC2 DAC
 *   ampdac_pcm_config cfg = {
 *       .sample_rate_hz = 44100,
 *       .channels = 2,
 *       .format = AMPDAC_PCM_S16LE,
 *       .volume = 1.0f,                             // unity = bit-perfect
 *   };
 *   ampdac_configure_pcm(s, &cfg);
 *   ampdac_start(s);
 *   ampdac_write(s, interleaved_pcm, nbytes);       // blocking, back-pressure
 *   ampdac_close(s);
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMPDAC_VERSION_STRING "0.1.0"

typedef enum ampdac_status {
    AMPDAC_OK = 0,
    AMPDAC_ERR_NO_DEVICE = -1,
    AMPDAC_ERR_LIBUSB = -2,
    AMPDAC_ERR_ACCESS = -3,
    AMPDAC_ERR_BUSY = -4,
    AMPDAC_ERR_UNSUPPORTED = -5,
    AMPDAC_ERR_FORMAT = -6,
    AMPDAC_ERR_CLOCK = -7,
    AMPDAC_ERR_IO = -8,
    AMPDAC_ERR_TIMEOUT = -9,
    AMPDAC_ERR_STATE = -10,
    AMPDAC_ERR_NOMEM = -11,
    AMPDAC_ERR_INVALID = -12,
} ampdac_status;

typedef enum ampdac_pcm_format {
    AMPDAC_PCM_S16LE = 0,
    AMPDAC_PCM_S32LE = 1,
    AMPDAC_PCM_F32LE = 2,
} ampdac_pcm_format;

typedef struct ampdac_device_info {
    uint16_t vid;
    uint16_t pid;
    uint8_t bus;
    uint8_t address;
    char manufacturer[128];
    char product[128];
    char serial[64];
} ampdac_device_info;

typedef struct ampdac_pcm_config {
    uint32_t sample_rate_hz;
    uint8_t channels;
    ampdac_pcm_format format;
    /** UI volume in [0, 1]. 1.0 is exact unity (bit-perfect). */
    float volume;
} ampdac_pcm_config;

/**
 * How to wrap a 1-bit DSD file onto a UAC2 ISO OUT.
 *
 * AUTO: Native DSD_U32LE when the DAC advertises a RAW/TYPE_IV alt, else DoP
 * on a TYPE_I PCM alt (the path that works on generic 24/32-bit DACs).
 * NATIVE / DOP: require that transport; fail rather than guess.
 */
typedef enum ampdac_dsd_transport {
    AMPDAC_DSD_AUTO = 0,
    AMPDAC_DSD_NATIVE = 1,
    AMPDAC_DSD_DOP = 2,   /**< DSD-over-PCM 1.1: USB clock = DSD/16, markers 0x05/0xFA. */
} ampdac_dsd_transport;

/**
 * DSD rate of the source file: 1-bit Hz (2822400 / 5644800 / 11289600)
 * or FFmpeg's packed 8-bit sample_rate (352800 / 705600 / 1411200).
 * Stereo only.
 */
typedef struct ampdac_dsd_config {
    uint32_t dsd_rate_hz;
    uint8_t channels;
    ampdac_dsd_transport transport;
} ampdac_dsd_config;

typedef enum ampdac_sync_mode {
    AMPDAC_SYNC_NONE = 0,
    AMPDAC_SYNC_ASYNC = 1,      /**< Device clock is master; host follows feedback. */
    AMPDAC_SYNC_ADAPTIVE = 2,
    AMPDAC_SYNC_SYNC = 3,       /**< USB SOF is master. */
} ampdac_sync_mode;

typedef enum ampdac_feedback_mode {
    AMPDAC_FEEDBACK_NONE = 0,
    AMPDAC_FEEDBACK_EXPLICIT = 1,  /**< ISO IN feedback endpoint (Q16.16). */
    AMPDAC_FEEDBACK_IMPLICIT = 2,
} ampdac_feedback_mode;

typedef struct ampdac_negotiated {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t subslot_bytes;
    uint8_t valid_bits;
    uint8_t interface_number;
    uint8_t alt_setting;
    uint8_t endpoint_address;
    ampdac_sync_mode sync;
    ampdac_feedback_mode feedback;
    uint8_t feedback_endpoint;
    /** Non-zero when this session is DSD rather than PCM. */
    uint8_t dsd;
    /** Non-zero when DSD is native DSD_U32LE; zero means DoP. */
    uint8_t dsd_native;
} ampdac_negotiated;

#define AMPDAC_MAX_RATES 64
#define AMPDAC_MAX_DEPTHS 8

typedef struct ampdac_capabilities {
    ampdac_device_info device;
    ampdac_sync_mode sync;
    ampdac_feedback_mode feedback;
    uint8_t feedback_endpoint;
    uint8_t channels_max;
    uint8_t bit_depths[AMPDAC_MAX_DEPTHS];
    int bit_depth_count;
    uint32_t sample_rates[AMPDAC_MAX_RATES];
    int sample_rate_count;
    uint8_t native_dsd;
    uint8_t dop;
    uint8_t native_dsd_interface;
    uint8_t native_dsd_alt;
    uint8_t dop_pcm_alt;
    char notes[256];
} ampdac_capabilities;

typedef struct ampdac_session ampdac_session;

const char *ampdac_status_string(ampdac_status status);
const char *ampdac_last_error(void);

/**
 * Enumerate class-compliant UAC2 DACs currently attached.
 * @return number of devices written, or a negative ampdac_status.
 */
int ampdac_list_devices(ampdac_device_info *out, int max_devices);

/**
 * Inspect a UAC2 DAC without starting playback: sync mode, feedback pipe,
 * advertised PCM rates and bit depths.
 */
ampdac_status ampdac_probe(uint16_t vid, uint16_t pid, ampdac_capabilities *out);

/** Same as ampdac_probe, but selects the device by USB bus/address. */
ampdac_status ampdac_probe_bus_address(uint8_t bus, uint8_t address,
                                       ampdac_capabilities *out);

/**
 * Open a UAC2 DAC. vid=0 and pid=0 selects the first UAC2 device found.
 * Detaches snd-usb-audio from the streaming interface when the kernel owns it.
 */
ampdac_session *ampdac_open(uint16_t vid, uint16_t pid);

ampdac_session *ampdac_open_bus_address(uint8_t bus, uint8_t address);

ampdac_status ampdac_configure_pcm(ampdac_session *session,
                                   const ampdac_pcm_config *config);

ampdac_status ampdac_configure_dsd(ampdac_session *session,
                                   const ampdac_dsd_config *config);

ampdac_status ampdac_negotiated_format(const ampdac_session *session,
                                       ampdac_negotiated *out);

ampdac_status ampdac_start(ampdac_session *session);

typedef struct ampdac_stream_stats {
    uint64_t underrun_count;
    /** 0 until the first valid explicit-feedback IN packet. */
    uint32_t feedback_q16_16;
    uint32_t ring_bytes;
    uint32_t ring_capacity;
} ampdac_stream_stats;

ampdac_status ampdac_get_stats(const ampdac_session *session,
                               ampdac_stream_stats *out);

/**
 * Push interleaved samples for the configured session.
 * PCM: host decoder bytes in the format given to ampdac_configure_pcm.
 * DSD: interleaved MSBF DSD bytes (FFmpeg dsd_msbf / u8), no volume.
 * Blocks while the ring is full (500 µs poll), up to ~2 s since the last
 * successful push. A full ring does not return a short count: the call waits,
 * then returns AMPDAC_ERR_TIMEOUT. ISO transfers start after the ring is half
 * full. A trailing partial sample that does not fill a frame is left
 * unconsumed and the return value is that shorter count.
 * @return bytes consumed from input, or a negative ampdac_status.
 */
int ampdac_write(ampdac_session *session, const void *pcm, size_t bytes);

/** Volume UI position in [0, 1]. Applied on subsequent writes. */
void ampdac_set_volume(ampdac_session *session, float volume);

void ampdac_close(ampdac_session *session);

#ifdef __cplusplus
}
#endif

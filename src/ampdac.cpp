#include "ampdac.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unistd.h>
#include <vector>

#include "libusb/libusb.h"

#include "audio_gain.h"
#include "dop_formatter.h"
#include "dsd_playback_manager.h"
#include "dsd_wire_mode.h"
#include "native_dsd_formatter.h"
#include "pcm_wire_formatter.h"
#include "uac2_clock_control.h"
#include "uac2_descriptor_parser.h"
#include "uac2_dsd_detector.h"
#include "usb_device_controller.h"
#include "usb_driver_context.h"
#include "usb_iso_transfer_pool.h"
#include "usb_session_transport.h"
#include "usb_teardown.h"
#include "ampdac_kernel.h"

namespace {

thread_local std::string g_last_error;

void set_error(const std::string &message) {
    g_last_error = message;
}

constexpr uint32_t kIsoPoolSize = 16;
constexpr uint32_t kIsoPacketsPerTransfer = 8;
constexpr std::size_t kWriteChunkBytes = 16U * 1024U;
constexpr int kWriteTimeoutMs = 2000;
constexpr useconds_t kWritePollUs = 500U;
constexpr useconds_t kDsdPllSettleUs = 50000U;
constexpr uint32_t kDsd64Hz = 2822400U;
constexpr uint32_t kDsd128Hz = 5644800U;
constexpr uint32_t kDsd256Hz = 11289600U;
constexpr uint32_t kNativeDsdBitsPerUsbFrame = 32U;
constexpr uint32_t kDopDsdBitsPerPcmFrame = 16U;
constexpr std::size_t kDsdStereoWireBytes = 8U;
constexpr std::size_t kNativeDsdInputBytesPerFrame = 8U;
constexpr std::size_t kDopInputBytesPerFrame = 4U;

bool is_uac2_device(libusb_device *dev) {
    libusb_config_descriptor *cfg = nullptr;
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != LIBUSB_SUCCESS) {
        rc = libusb_get_config_descriptor(dev, 0, &cfg);
    }
    if (rc != LIBUSB_SUCCESS || cfg == nullptr) {
        return false;
    }

    bool found = false;
    for (int i = 0; i < cfg->bNumInterfaces && !found; ++i) {
        const libusb_interface &iface = cfg->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const libusb_interface_descriptor &id = iface.altsetting[a];
            if (id.bInterfaceClass == UAC_CLASS_AUDIO &&
                id.bInterfaceSubClass == UAC_SUBCLASS_AUDIOSTREAMING &&
                id.bInterfaceProtocol == UAC_PROTOCOL_VERSION_2) {
                found = true;
                break;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

void fill_strings(libusb_device_handle *handle,
                  const libusb_device_descriptor &desc,
                  ampdac_device_info *info) {
    if (desc.iManufacturer != 0) {
        libusb_get_string_descriptor_ascii(
                handle, desc.iManufacturer,
                reinterpret_cast<unsigned char *>(info->manufacturer),
                static_cast<int>(sizeof(info->manufacturer) - 1));
    }
    if (desc.iProduct != 0) {
        libusb_get_string_descriptor_ascii(
                handle, desc.iProduct,
                reinterpret_cast<unsigned char *>(info->product),
                static_cast<int>(sizeof(info->product) - 1));
    }
    if (desc.iSerialNumber != 0) {
        libusb_get_string_descriptor_ascii(
                handle, desc.iSerialNumber,
                reinterpret_cast<unsigned char *>(info->serial),
                static_cast<int>(sizeof(info->serial) - 1));
    }
}

bool has_audio_in_flight(const IsoTransferPool &pool) noexcept {
    const auto &slots = pool.slots();
    const std::size_t n = std::min(
            slots.size(),
            static_cast<std::size_t>(pool.config().pool_size));
    for (std::size_t i = 0; i < n; ++i) {
        if (slots[i].in_flight.load(std::memory_order_acquire)) return true;
    }
    return false;
}

PcmSourceEncoding source_encoding(ampdac_pcm_format format) {
    switch (format) {
        case AMPDAC_PCM_S16LE: return PcmSourceEncoding::S16Le;
        case AMPDAC_PCM_S32LE:
        case AMPDAC_PCM_F32LE:
        default:
            return PcmSourceEncoding::S32Le;
    }
}

}  // namespace

struct ampdac_session {
    UsbDriverContext *ctx = nullptr;
    Uac2StreamingEndpointInfo endpoint{};
    ampdac_pcm_format format = AMPDAC_PCM_S32LE;
    std::atomic<float> volume{1.0f};
    bool configured = false;
    bool started = false;
    bool transfers_started = false;
    bool dsd = false;
    bool native_dsd = false;
    uint8_t dop_marker = kDopMarkerA;
    std::vector<uint8_t> dsd_carry;
    AmpdacKernelClaim kernel{};
};

namespace {

const Uac2StreamingEndpointInfo *match_endpoint(
        const std::vector<Uac2StreamingEndpointInfo> &candidates,
        uint8_t iface,
        uint8_t alt,
        uint8_t endpoint_address) {
    for (const auto &ep : candidates) {
        if (ep.interface_number == iface &&
            ep.alt_setting == alt &&
            ep.endpoint_address == endpoint_address) {
            return &ep;
        }
    }
    return nullptr;
}

/**
 * Canonical 1-bit DSD rate, or 0 if unknown.
 *
 * FFmpeg's dsd_* codecs advertise an 8-bit packed sample_rate (DSD64 → 352800,
 * DSD256 → 1411200). DSF/DSDIFF headers and Android use the 1-bit rate
 * (2822400 / 5644800 / 11289600). Accept both so a host can pass ffprobe
 * output unchanged.
 */
uint32_t canonicalize_dsd_rate_hz(uint32_t rate_hz) {
    if (rate_hz == kDsd64Hz || rate_hz == kDsd128Hz || rate_hz == kDsd256Hz) {
        return rate_hz;
    }
    const uint32_t unpacked = rate_hz * 8U;
    if (unpacked == kDsd64Hz || unpacked == kDsd128Hz || unpacked == kDsd256Hz) {
        return unpacked;
    }
    return 0;
}

ampdac_status program_usb_stream(
        ampdac_session *session,
        uint32_t usb_clock_hz,
        uint32_t bytes_per_frame,
        bool settle_pll) {
    UsbDriverContext *ctx = session->ctx;

    std::string retry_error;
    (void)ampdac_kernel_takeover(ctx->device_handle, &session->kernel, &retry_error);

    const UsbInterfaceClaimResult claim =
            uac2_claim_interface_only(ctx->device_handle, session->endpoint);
    if (!claim.success) {
        if (claim.status == UsbControlStatus::ClaimBusyKernelDriverOwner) {
            set_error(retry_error.empty()
                    ? "kernel still owns the streaming interface (snd-usb-audio / PipeWire)"
                    : retry_error);
            return AMPDAC_ERR_BUSY;
        }
        if (claim.status == UsbControlStatus::ClaimAccessDenied) {
            set_error("libusb_claim_interface ACCESS — check udev rules");
            return AMPDAC_ERR_ACCESS;
        }
        set_error("failed to claim AudioStreaming interface");
        return AMPDAC_ERR_IO;
    }
    ctx->claimed_interface_number = claim.claimed_interface_number;

    const auto idle_then_clock = [&](bool extra_settle) -> int {
        (void)uac2_apply_alt_setting(
                ctx->device_handle,
                ctx->claimed_interface_number,
                0,
                0);
        // After Native DSD the XMOS PLL is still unwinding; SET_CUR too soon
        // lands as LIBUSB_ERROR_IO and the next session looks like FSR ERROR.
        usleep(extra_settle ? 100000U : 20000U);

        int detected_ac_iface = -1;
        const int detected_clock_id =
                uac2_find_clock_source_id(ctx->device_handle, &detected_ac_iface);
        const int ctrl_iface = detected_ac_iface >= 0 ? detected_ac_iface : 0;
        const int csid = detected_clock_id > 0 ? detected_clock_id : 1;

        const int ctrl_claim = libusb_claim_interface(ctx->device_handle, ctrl_iface);
        if (ctrl_claim == LIBUSB_SUCCESS) {
            ctx->ctrl_interface_number = ctrl_iface;
        }

        int winning_clock_id = 0;
        const int clock_rc = uac2_force_clock_sample_rate(
                ctx->device_handle,
                static_cast<uint8_t>(ctrl_iface),
                csid,
                usb_clock_hz,
                &winning_clock_id);
        if (clock_rc == 4) {
            ctx->resolved_clock_source_id = static_cast<uint8_t>(winning_clock_id);
        }
        return clock_rc;
    };

    int clock_rc = idle_then_clock(false);
    if (clock_rc != 4) {
        // EP0 often stays stalled after a DSD session whose soft-reset timed
        // out and then poked a second clock ID. A port reset clears it; the
        // handle stays valid on Linux and we re-claim.
        const int rst = libusb_reset_device(ctx->device_handle);
        if (rst != LIBUSB_SUCCESS) {
            set_error("UAC2 SET_CUR clock sample rate failed (device reset "
                      "also failed after DSD)");
            return AMPDAC_ERR_CLOCK;
        }
        usleep(250000U);
        (void)ampdac_kernel_takeover(
                ctx->device_handle, &session->kernel, &retry_error);
        const UsbInterfaceClaimResult reclaim =
                uac2_claim_interface_only(ctx->device_handle, session->endpoint);
        if (!reclaim.success) {
            set_error("UAC2 SET_CUR clock failed; re-claim after reset failed");
            return AMPDAC_ERR_IO;
        }
        ctx->claimed_interface_number = reclaim.claimed_interface_number;
        clock_rc = idle_then_clock(true);
    }
    if (clock_rc != 4) {
        set_error("UAC2 SET_CUR clock sample rate failed");
        return AMPDAC_ERR_CLOCK;
    }

    if (settle_pll) {
        usleep(kDsdPllSettleUs);
    }

    const UsbControlStatus alt = uac2_apply_alt_setting(
            ctx->device_handle,
            session->endpoint.interface_number,
            session->endpoint.alt_setting,
            session->endpoint.endpoint_address);
    if (alt != UsbControlStatus::Success) {
        set_error("SET_INTERFACE alt-setting failed");
        return AMPDAC_ERR_IO;
    }
    ctx->active_alt_setting = session->endpoint.alt_setting;

    IsoTransferPoolConfig pool_cfg;
    pool_cfg.sample_rate_hz = usb_clock_hz;
    pool_cfg.bytes_per_audio_frame = bytes_per_frame;
    pool_cfg.endpoint_address = session->endpoint.endpoint_address;
    pool_cfg.pool_size = kIsoPoolSize;
    pool_cfg.packets_per_transfer = kIsoPacketsPerTransfer;
    pool_cfg.sync_type = session->endpoint.sync_type;
    pool_cfg.feedback_endpoint_address = session->endpoint.feedback_endpoint;
    pool_cfg.feedback_interval = session->endpoint.feedback_interval != 0U
            ? session->endpoint.feedback_interval
            : static_cast<uint8_t>(4);

    std::string pool_error;
    ctx->transfer_pool = IsoTransferPool::create(
            ctx->device_handle, pool_cfg, &pool_error);
    if (!ctx->transfer_pool) {
        set_error(std::string("IsoTransferPool: ") + pool_error);
        return AMPDAC_ERR_IO;
    }
    ctx->iso_endpoint_address = pool_cfg.endpoint_address;
    return AMPDAC_OK;
}

int push_wire_with_prefill(
        ampdac_session *session,
        const uint8_t *wire,
        std::size_t wire_bytes,
        bool *transfers_started,
        UsbPlaybackState live_state) {
    auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(kWriteTimeoutMs);
    std::size_t chunk_offset = 0;
    while (chunk_offset < wire_bytes) {
        const std::size_t free_bytes = session->ctx->ring_buffer->free_space();
        if (free_bytes == 0U) {
            if (std::chrono::steady_clock::now() >= deadline) {
                set_error("ring buffer back-pressure timeout");
                return AMPDAC_ERR_TIMEOUT;
            }
            usleep(kWritePollUs);
            continue;
        }

        const std::size_t push_bytes = std::min(free_bytes, wire_bytes - chunk_offset);
        if (!session->ctx->ring_buffer->push(wire + chunk_offset, push_bytes)) {
            usleep(kWritePollUs);
            continue;
        }
        chunk_offset += push_bytes;
        deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kWriteTimeoutMs);

        if (!*transfers_started) {
            const std::size_t prefill_goal =
                    session->ctx->ring_buffer->capacity() / 2U;
            if (session->ctx->ring_buffer->size() < prefill_goal) {
                continue;
            }
            const int submitted = session->ctx->transfer_pool->submit_all_transfers();
            if (submitted < 1) {
                set_error("ISO transfer submission failed");
                return AMPDAC_ERR_IO;
            }
            *transfers_started = true;
            session->transfers_started = true;
            (void)session->ctx->playback_state.transition_to(live_state);
        }
    }
    return 0;
}

}  // namespace

extern "C" const char *ampdac_status_string(ampdac_status status) {
    switch (status) {
        case AMPDAC_OK: return "ok";
        case AMPDAC_ERR_NO_DEVICE: return "no UAC2 DAC found";
        case AMPDAC_ERR_LIBUSB: return "libusb error";
        case AMPDAC_ERR_ACCESS: return "permission denied (udev / raw USB)";
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

extern "C" const char *ampdac_last_error(void) {
    return g_last_error.empty() ? "" : g_last_error.c_str();
}

extern "C" int ampdac_list_devices(ampdac_device_info *out, int max_devices) {
    if (out == nullptr || max_devices <= 0) {
        set_error("ampdac_list_devices: invalid buffer");
        return AMPDAC_ERR_INVALID;
    }

    libusb_context *usb = nullptr;
    const int init_rc = libusb_init(&usb);
    if (init_rc != LIBUSB_SUCCESS) {
        set_error(std::string("libusb_init: ") + libusb_error_name(init_rc));
        return AMPDAC_ERR_LIBUSB;
    }

    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(usb, &list);
    if (count < 0) {
        libusb_exit(usb);
        set_error("libusb_get_device_list failed");
        return AMPDAC_ERR_LIBUSB;
    }

    int written = 0;
    for (ssize_t i = 0; i < count && written < max_devices; ++i) {
        libusb_device *dev = list[i];
        if (!is_uac2_device(dev)) continue;

        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) continue;

        ampdac_device_info info{};
        info.vid = desc.idVendor;
        info.pid = desc.idProduct;
        info.bus = libusb_get_bus_number(dev);
        info.address = libusb_get_device_address(dev);

        libusb_device_handle *handle = nullptr;
        if (libusb_open(dev, &handle) == LIBUSB_SUCCESS) {
            fill_strings(handle, desc, &info);
            libusb_close(handle);
        }

        out[written++] = info;
    }

    libusb_free_device_list(list, 1);
    libusb_exit(usb);
    return written;
}

static ampdac_session *open_matching(
        bool (*match)(libusb_device *, void *),
        void *user) {
    auto *session = new (std::nothrow) ampdac_session();
    if (session == nullptr) {
        set_error("out of memory allocating session");
        return nullptr;
    }

    auto *ctx = new (std::nothrow) UsbDriverContext();
    if (ctx == nullptr) {
        delete session;
        set_error("out of memory allocating UsbDriverContext");
        return nullptr;
    }
    try {
        ctx->producer_coordinator = std::make_shared<UsbProducerCoordinator>();
    } catch (...) {
        delete ctx;
        delete session;
        set_error("out of memory allocating producer coordinator");
        return nullptr;
    }

    const int init_rc = libusb_init(&ctx->usb_ctx);
    if (init_rc != LIBUSB_SUCCESS) {
        set_error(std::string("libusb_init: ") + libusb_error_name(init_rc));
        delete ctx;
        delete session;
        return nullptr;
    }

    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx->usb_ctx, &list);
    if (count < 0) {
        set_error("libusb_get_device_list failed");
        teardown_context(ctx);
        delete ctx;
        delete session;
        return nullptr;
    }

    libusb_device *chosen = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        if (!is_uac2_device(list[i])) continue;
        if (match(list[i], user)) {
            chosen = list[i];
            break;
        }
    }

    if (chosen == nullptr) {
        libusb_free_device_list(list, 1);
        set_error("no matching UAC2 DAC");
        teardown_context(ctx);
        delete ctx;
        delete session;
        return nullptr;
    }

    const int open_rc = libusb_open(chosen, &ctx->device_handle);
    if (open_rc != LIBUSB_SUCCESS) {
        const uint8_t bus = libusb_get_bus_number(chosen);
        const uint8_t addr = libusb_get_device_address(chosen);
        char node[64];
        std::snprintf(node, sizeof(node), "/dev/bus/usb/%03u/%03u", bus, addr);
        libusb_free_device_list(list, 1);
        std::string msg = std::string("libusb_open: ") + libusb_error_name(open_rc);
        if (open_rc == LIBUSB_ERROR_ACCESS) {
            msg += " — no write access to ";
            msg += node;
            msg += " (now root:root 0644/0664). Install rules and grant the node:";
            msg += "  sudo linux/grant-usb-dac.sh";
        }
        set_error(msg);
        libusb_exit(ctx->usb_ctx);
        ctx->usb_ctx = nullptr;
        delete ctx;
        delete session;
        return nullptr;
    }
    libusb_free_device_list(list, 1);

    session->ctx = ctx;

    std::string kernel_error;
    if (!ampdac_kernel_takeover(ctx->device_handle, &session->kernel, &kernel_error)) {
        // Still return a session: probe/list-like opens can inspect descriptors
        // even when ALSA holds the PCM node. configure/claim will fail BUSY
        // with this message if the kernel never let go.
        if (!kernel_error.empty()) set_error(kernel_error);
    }
    return session;
}

struct VidPid {
    uint16_t vid;
    uint16_t pid;
};

extern "C" ampdac_session *ampdac_open(uint16_t vid, uint16_t pid) {
    VidPid id{vid, pid};
    return open_matching(
            [](libusb_device *dev, void *user) {
                const auto *want = static_cast<const VidPid *>(user);
                if (want->vid == 0 && want->pid == 0) return true;
                libusb_device_descriptor desc{};
                if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) {
                    return false;
                }
                return desc.idVendor == want->vid && desc.idProduct == want->pid;
            },
            &id);
}

struct BusAddr {
    uint8_t bus;
    uint8_t address;
};

extern "C" ampdac_session *ampdac_open_bus_address(uint8_t bus, uint8_t address) {
    BusAddr loc{bus, address};
    return open_matching(
            [](libusb_device *dev, void *user) {
                const auto *want = static_cast<const BusAddr *>(user);
                return libusb_get_bus_number(dev) == want->bus &&
                       libusb_get_device_address(dev) == want->address;
            },
            &loc);
}

extern "C" ampdac_status ampdac_configure_pcm(
        ampdac_session *session,
        const ampdac_pcm_config *config) {
    if (session == nullptr || session->ctx == nullptr || config == nullptr) {
        set_error("ampdac_configure_pcm: invalid session or config");
        return AMPDAC_ERR_INVALID;
    }
    if (session->started) {
        set_error("ampdac_configure_pcm: already started");
        return AMPDAC_ERR_STATE;
    }
    if (config->sample_rate_hz == 0 || config->channels == 0) {
        set_error("ampdac_configure_pcm: sample rate and channels must be > 0");
        return AMPDAC_ERR_INVALID;
    }

    UsbDriverContext *ctx = session->ctx;
    const auto candidates = uac2_find_streaming_endpoints(ctx->device_handle);
    if (candidates.empty()) {
        set_error("no UAC2 isochronous OUT endpoints");
        return AMPDAC_ERR_UNSUPPORTED;
    }

    // PcmWireFormatter requires a 4-byte Type-I subslot (Android contract).
    const Uac2StreamingEndpointInfo *selected = uac2_select_endpoint_for_format(
            candidates,
            32,
            static_cast<int>(config->channels),
            static_cast<int>(config->sample_rate_hz),
            true);
    if (selected == nullptr) {
        selected = uac2_select_endpoint_for_format(
                candidates,
                32,
                static_cast<int>(config->channels),
                static_cast<int>(config->sample_rate_hz),
                false);
    }
    if (selected == nullptr) {
        set_error("no matching UAC2 endpoint for requested PCM format");
        return AMPDAC_ERR_UNSUPPORTED;
    }
    if (selected->as_general.subslot_size != 4U) {
        set_error("DAC alternate setting is not a 4-byte PCM subslot");
        return AMPDAC_ERR_UNSUPPORTED;
    }

    session->endpoint = *selected;
    const uint8_t valid_bits =
            selected->as_general.bit_resolution > 0U
                ? selected->as_general.bit_resolution
                : static_cast<uint8_t>(32);
    ctx->pcm_valid_bit_depth = valid_bits;
    ctx->pcm_wire_format = UsbPcmWireFormat{
            config->sample_rate_hz,
            config->channels,
            selected->as_general.subslot_size,
            valid_bits,
    };
    if (!ctx->pcm_wire_format.is_valid()) {
        set_error("negotiated PCM wire format is invalid");
        return AMPDAC_ERR_FORMAT;
    }
    (void)ctx->playback_state.transition_to(UsbPlaybackState::Configured);

    const ampdac_status usb_st = program_usb_stream(
            session,
            config->sample_rate_hz,
            static_cast<uint32_t>(config->channels) * 4U,
            false);
    if (usb_st != AMPDAC_OK) return usb_st;

    session->format = config->format;
    session->volume.store(config->volume, std::memory_order_relaxed);
    session->dsd = false;
    session->native_dsd = false;
    ctx->was_dsd_session = false;
    ctx->dsd_wire_mode = DsdWireMode::Native;
    session->configured = true;
    return AMPDAC_OK;
}

extern "C" ampdac_status ampdac_configure_dsd(
        ampdac_session *session,
        const ampdac_dsd_config *config) {
    if (session == nullptr || session->ctx == nullptr || config == nullptr) {
        set_error("ampdac_configure_dsd: invalid session or config");
        return AMPDAC_ERR_INVALID;
    }
    if (session->started) {
        set_error("ampdac_configure_dsd: already started");
        return AMPDAC_ERR_STATE;
    }
    if (config->channels != 2U) {
        set_error("ampdac_configure_dsd: stereo DSD only");
        return AMPDAC_ERR_UNSUPPORTED;
    }
    const uint32_t dsd_rate_hz = canonicalize_dsd_rate_hz(config->dsd_rate_hz);
    if (dsd_rate_hz == 0U) {
        set_error("ampdac_configure_dsd: rate must be DSD64/128/256 "
                  "(1-bit Hz or FFmpeg packed rate / 8)");
        return AMPDAC_ERR_UNSUPPORTED;
    }

    UsbDriverContext *ctx = session->ctx;
    const auto candidates = uac2_find_streaming_endpoints(ctx->device_handle);
    if (candidates.empty()) {
        set_error("no UAC2 isochronous OUT endpoints");
        return AMPDAC_ERR_UNSUPPORTED;
    }

    const Uac2DsdCapabilitySummary dsd = uac2_detect_native_dsd(candidates);
    bool native = dsd.supports_native_dsd;
    if (config->transport == AMPDAC_DSD_DOP) {
        native = false;
    } else if (config->transport == AMPDAC_DSD_NATIVE) {
        if (!dsd.supports_native_dsd) {
            set_error("DAC does not advertise a Native DSD RAW/TYPE_IV alt");
            return AMPDAC_ERR_UNSUPPORTED;
        }
        native = true;
    }

    const uint32_t usb_clock_hz = native
            ? dsd_rate_hz / kNativeDsdBitsPerUsbFrame
            : dsd_rate_hz / kDopDsdBitsPerPcmFrame;

    const Uac2StreamingEndpointInfo *selected = nullptr;
    if (native) {
        selected = match_endpoint(
                candidates,
                dsd.native_dsd_interface,
                dsd.native_dsd_alt_setting,
                dsd.native_dsd_endpoint);
        if (selected == nullptr) {
            set_error("Native DSD alt from descriptors has no matching ISO OUT");
            return AMPDAC_ERR_UNSUPPORTED;
        }
    } else {
        // DoP 1.1 is TYPE_I PCM at DSD/16 with a 4-byte subslot (24-in-32).
        selected = uac2_select_endpoint_for_format(
                candidates,
                32,
                static_cast<int>(config->channels),
                static_cast<int>(usb_clock_hz),
                true);
        if (selected == nullptr) {
            selected = uac2_select_endpoint_for_format(
                    candidates,
                    32,
                    static_cast<int>(config->channels),
                    static_cast<int>(usb_clock_hz),
                    false);
        }
        if (selected == nullptr && dsd.supports_dop) {
            selected = match_endpoint(
                    candidates,
                    dsd.pcm_interface,
                    dsd.pcm_alt_setting,
                    dsd.pcm_endpoint);
        }
        if (selected == nullptr || selected->as_general.subslot_size != 4U) {
            char msg[160];
            std::snprintf(
                    msg, sizeof(msg),
                    "no TYPE_I 4-byte PCM alt for DoP at %u Hz (DSD/16)",
                    usb_clock_hz);
            set_error(msg);
            return AMPDAC_ERR_UNSUPPORTED;
        }
    }

    session->endpoint = *selected;
    ctx->pcm_valid_bit_depth = native ? static_cast<uint8_t>(1) : static_cast<uint8_t>(24);
    ctx->pcm_wire_format = UsbPcmWireFormat{
            usb_clock_hz,
            config->channels,
            4,
            ctx->pcm_valid_bit_depth,
    };
    (void)ctx->playback_state.transition_to(UsbPlaybackState::Configured);

    const ampdac_status usb_st = program_usb_stream(
            session,
            usb_clock_hz,
            static_cast<uint32_t>(kDsdStereoWireBytes),
            true);
    if (usb_st != AMPDAC_OK) return usb_st;

    session->dsd = true;
    session->native_dsd = native;
    session->dop_marker = kDopMarkerA;
    session->dsd_carry.clear();
    session->volume.store(1.0f, std::memory_order_relaxed);
    ctx->was_dsd_session = true;
    ctx->dsd_wire_mode = native ? DsdWireMode::Native : DsdWireMode::Dop;
    if (native) {
        ctx->transfer_pool->set_silence_byte(kNativeDsdSilenceByte);
    } else {
        uint8_t dop_idle[kDopIdlePatternBytes];
        fill_dop_idle_stereo(dop_idle, kDopIdlePatternFrames, kDopMarkerA);
        ctx->transfer_pool->set_silence_pattern(dop_idle, kDopIdlePatternBytes);
    }
    session->configured = true;
    return AMPDAC_OK;
}

extern "C" ampdac_status ampdac_negotiated_format(
        const ampdac_session *session,
        ampdac_negotiated *out) {
    if (session == nullptr || session->ctx == nullptr || out == nullptr) {
        return AMPDAC_ERR_INVALID;
    }
    if (!session->configured) {
        return AMPDAC_ERR_STATE;
    }
    const auto &wire = session->ctx->pcm_wire_format;
    *out = ampdac_negotiated{
            wire.sample_rate_hz,
            wire.channel_count,
            wire.subslot_bytes,
            wire.valid_bits,
            session->endpoint.interface_number,
            session->endpoint.alt_setting,
            session->endpoint.endpoint_address,
            static_cast<ampdac_sync_mode>(session->endpoint.sync_type),
            session->endpoint.feedback_endpoint != 0U
                    ? AMPDAC_FEEDBACK_EXPLICIT
                    : (session->endpoint.usage_type == UAC_ISO_USAGE_IMPLICIT
                               ? AMPDAC_FEEDBACK_IMPLICIT
                               : AMPDAC_FEEDBACK_NONE),
            session->endpoint.feedback_endpoint,
            session->dsd ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
            session->native_dsd ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
    };
    return AMPDAC_OK;
}

extern "C" ampdac_status ampdac_start(ampdac_session *session) {
    if (session == nullptr || session->ctx == nullptr) {
        set_error("ampdac_start: invalid session");
        return AMPDAC_ERR_INVALID;
    }
    if (!session->configured) {
        set_error("ampdac_start: configure PCM or DSD first");
        return AMPDAC_ERR_STATE;
    }
    if (session->started) {
        return AMPDAC_OK;
    }

    const int rc = usb_transport_prepare(session->ctx, "ampdac_start");
    if (rc != 0) {
        set_error("usb_transport_prepare failed");
        return AMPDAC_ERR_IO;
    }
    (void)session->ctx->playback_state.transition_to(UsbPlaybackState::Priming);
    session->started = true;
    return AMPDAC_OK;
}

extern "C" int ampdac_write(ampdac_session *session, const void *pcm, size_t bytes) {
    if (session == nullptr || session->ctx == nullptr || pcm == nullptr) {
        set_error("ampdac_write: invalid arguments");
        return AMPDAC_ERR_INVALID;
    }
    if (!session->started || !session->ctx->ring_buffer ||
        !session->ctx->transfer_pool || !session->ctx->event_thread) {
        set_error("ampdac_write: call ampdac_start first");
        return AMPDAC_ERR_STATE;
    }
    if (bytes == 0) return 0;

    const auto *input = static_cast<const uint8_t *>(pcm);
    bool transfers_started = session->transfers_started ||
            has_audio_in_flight(*session->ctx->transfer_pool);
    std::array<uint8_t, kWriteChunkBytes> wire_chunk{};

    if (session->dsd) {
        const std::size_t in_per_frame = session->native_dsd
                ? kNativeDsdInputBytesPerFrame
                : kDopInputBytesPerFrame;
        const UsbPlaybackState live = session->native_dsd
                ? UsbPlaybackState::StreamingNativeDsd
                : UsbPlaybackState::StreamingDop;

        session->dsd_carry.insert(
                session->dsd_carry.end(), input, input + bytes);
        std::size_t carry_offset = 0;
        while (session->dsd_carry.size() - carry_offset >= in_per_frame) {
            const std::size_t available = session->dsd_carry.size() - carry_offset;
            std::size_t frames = available / in_per_frame;
            const std::size_t max_frames = wire_chunk.size() / kDsdStereoWireBytes;
            if (frames > max_frames) frames = max_frames;
            if (frames == 0) break;

            const uint8_t *chunk = session->dsd_carry.data() + carry_offset;
            if (session->native_dsd) {
                format_native_dsd_from_interleaved_msbf(
                        chunk, wire_chunk.data(), frames);
            } else {
                session->dop_marker = format_dop_from_interleaved_dsd(
                        chunk, wire_chunk.data(), frames, session->dop_marker);
            }
            const int pushed = push_wire_with_prefill(
                    session,
                    wire_chunk.data(),
                    frames * kDsdStereoWireBytes,
                    &transfers_started,
                    live);
            if (pushed < 0) return pushed;
            carry_offset += frames * in_per_frame;
        }
        if (carry_offset > 0U) {
            session->dsd_carry.erase(
                    session->dsd_carry.begin(),
                    session->dsd_carry.begin() +
                            static_cast<std::ptrdiff_t>(carry_offset));
        }

        if (!transfers_started && session->ctx->ring_buffer->size() > 0U) {
            const int submitted = session->ctx->transfer_pool->submit_all_transfers();
            if (submitted < 1) {
                set_error("ISO transfer submission failed");
                return AMPDAC_ERR_IO;
            }
            session->transfers_started = true;
            (void)session->ctx->playback_state.transition_to(live);
        }
        return static_cast<int>(bytes);
    }

    const double gain = ui_position_to_gain(
            static_cast<double>(session->volume.load(std::memory_order_relaxed)));
    const PcmWireFormatter formatter(session->ctx->pcm_wire_format);

    std::size_t input_offset = 0;
    while (input_offset < bytes) {
        const std::size_t remaining = bytes - input_offset;
        std::size_t in_chunk = remaining;
        if (session->format == AMPDAC_PCM_S16LE) {
            in_chunk = std::min(remaining, wire_chunk.size() / 2U);
            in_chunk -= in_chunk % 2U;
        } else {
            in_chunk = std::min(remaining, wire_chunk.size());
            in_chunk -= in_chunk % 4U;
        }
        if (in_chunk == 0) break;

        PcmFormatResult formatted{};
        if (session->format == AMPDAC_PCM_F32LE) {
            formatted = formatter.format_float32(
                    input + input_offset, in_chunk, gain,
                    wire_chunk.data(), wire_chunk.size());
        } else {
            formatted = formatter.format(
                    input + input_offset, in_chunk,
                    source_encoding(session->format), gain,
                    wire_chunk.data(), wire_chunk.size());
        }
        if (!formatted.success) {
            set_error("PCM wire formatter rejected chunk");
            return AMPDAC_ERR_FORMAT;
        }

        const int pushed = push_wire_with_prefill(
                session,
                wire_chunk.data(),
                formatted.bytes_written,
                &transfers_started,
                UsbPlaybackState::StreamingPcm);
        if (pushed < 0) return pushed;
        input_offset += in_chunk;
    }

    if (!transfers_started && session->ctx->ring_buffer->size() > 0U) {
        const int submitted = session->ctx->transfer_pool->submit_all_transfers();
        if (submitted < 1) {
            set_error("ISO transfer submission failed");
            return AMPDAC_ERR_IO;
        }
        session->transfers_started = true;
        (void)session->ctx->playback_state.transition_to(
                UsbPlaybackState::StreamingPcm);
    }

    return static_cast<int>(input_offset);
}

extern "C" ampdac_status ampdac_get_stats(
        const ampdac_session *session, ampdac_stream_stats *out) {
    if (session == nullptr || session->ctx == nullptr || out == nullptr) {
        return AMPDAC_ERR_INVALID;
    }
    if (!session->started || session->ctx->transfer_pool == nullptr ||
        session->ctx->ring_buffer == nullptr) {
        return AMPDAC_ERR_STATE;
    }
    *out = ampdac_stream_stats{
            session->ctx->transfer_pool->underrun_count(),
            session->ctx->transfer_pool->current_feedback_q16_16(),
            static_cast<uint32_t>(session->ctx->ring_buffer->size()),
            static_cast<uint32_t>(session->ctx->ring_buffer->capacity()),
    };
    return AMPDAC_OK;
}

static ampdac_status probe_open_session(ampdac_session *session,
                                        ampdac_capabilities *out) {
    if (session == nullptr || session->ctx == nullptr) {
        return AMPDAC_ERR_NO_DEVICE;
    }
    std::memset(out, 0, sizeof(*out));

    libusb_device_handle *handle = session->ctx->device_handle;
    libusb_device *dev = libusb_get_device(handle);
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) == LIBUSB_SUCCESS) {
        out->device.vid = desc.idVendor;
        out->device.pid = desc.idProduct;
        out->device.bus = libusb_get_bus_number(dev);
        out->device.address = libusb_get_device_address(dev);
        fill_strings(handle, desc, &out->device);
    }

    const auto endpoints = uac2_find_streaming_endpoints(handle);
    uint8_t sync = 0;
    for (const auto &ep : endpoints) {
        if (ep.as_general.nr_channels > out->channels_max) {
            out->channels_max = ep.as_general.nr_channels;
        }
        if (ep.as_general.bit_resolution > 0U &&
            out->bit_depth_count < AMPDAC_MAX_DEPTHS) {
            bool seen = false;
            for (int i = 0; i < out->bit_depth_count; ++i) {
                if (out->bit_depths[i] == ep.as_general.bit_resolution) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                out->bit_depths[out->bit_depth_count++] =
                        ep.as_general.bit_resolution;
            }
        }
        if (ep.sync_type == UAC_ISO_SYNC_ASYNC) sync = UAC_ISO_SYNC_ASYNC;
        else if (sync == 0U) sync = ep.sync_type;
        if (ep.feedback_endpoint != 0U && out->feedback_endpoint == 0U) {
            out->feedback_endpoint = ep.feedback_endpoint;
            out->feedback = AMPDAC_FEEDBACK_EXPLICIT;
        } else if (ep.usage_type == UAC_ISO_USAGE_IMPLICIT &&
                   out->feedback == AMPDAC_FEEDBACK_NONE) {
            out->feedback = AMPDAC_FEEDBACK_IMPLICIT;
        }
    }
    out->sync = static_cast<ampdac_sync_mode>(sync);

    const Uac2DsdCapabilitySummary dsd = uac2_detect_native_dsd(endpoints);
    out->native_dsd = dsd.supports_native_dsd ? 1U : 0U;
    out->dop = dsd.supports_dop ? 1U : 0U;
    out->native_dsd_interface = dsd.native_dsd_interface;
    out->native_dsd_alt = dsd.native_dsd_alt_setting;
    out->dop_pcm_alt = dsd.pcm_alt_setting;

    int ac = -1;
    const int clock_id = uac2_find_clock_source_id(handle, &ac);
    const int ctrl = ac >= 0 ? ac : 0;
    std::string kernel_error;
    (void)ampdac_kernel_takeover(handle, &session->kernel, &kernel_error);
    (void)libusb_claim_interface(handle, ctrl);
    if (clock_id > 0) {
        out->sample_rate_count = uac2_query_clock_sample_rates(
                handle,
                static_cast<uint8_t>(ctrl),
                static_cast<uint8_t>(clock_id),
                out->sample_rates,
                AMPDAC_MAX_RATES);
    }
    libusb_release_interface(handle, ctrl);

    char dsd_note[96] = "";
    if (out->native_dsd != 0U && out->dop != 0U) {
        std::snprintf(dsd_note, sizeof(dsd_note),
                      "; Native DSD iface=%u alt=%u, DoP PCM alt=%u",
                      out->native_dsd_interface, out->native_dsd_alt,
                      out->dop_pcm_alt);
    } else if (out->native_dsd != 0U) {
        std::snprintf(dsd_note, sizeof(dsd_note),
                      "; Native DSD iface=%u alt=%u",
                      out->native_dsd_interface, out->native_dsd_alt);
    } else if (out->dop != 0U) {
        std::snprintf(dsd_note, sizeof(dsd_note),
                      "; DoP on PCM alt=%u", out->dop_pcm_alt);
    } else {
        std::snprintf(dsd_note, sizeof(dsd_note), "; no DSD");
    }

    if (out->sync == AMPDAC_SYNC_ASYNC &&
        out->feedback == AMPDAC_FEEDBACK_EXPLICIT) {
        std::snprintf(out->notes, sizeof(out->notes),
                      "Async UAC2: DAC is clock master, explicit feedback IN 0x%02X%s",
                      out->feedback_endpoint, dsd_note);
    } else if (out->sync == AMPDAC_SYNC_ASYNC) {
        std::snprintf(out->notes, sizeof(out->notes),
                      "Async UAC2 without a usable feedback pipe — "
                      "packet sizes follow USB SOF (Bresenham)%s",
                      dsd_note);
    } else {
        std::snprintf(out->notes, sizeof(out->notes),
                      "Iso sync=%d — host SOF / adaptive packet grid%s",
                      static_cast<int>(out->sync), dsd_note);
    }

    ampdac_close(session);
    return AMPDAC_OK;
}

extern "C" ampdac_status ampdac_probe(
        uint16_t vid, uint16_t pid, ampdac_capabilities *out) {
    if (out == nullptr) {
        set_error("ampdac_probe: null capabilities");
        return AMPDAC_ERR_INVALID;
    }
    return probe_open_session(ampdac_open(vid, pid), out);
}

extern "C" ampdac_status ampdac_probe_bus_address(
        uint8_t bus, uint8_t address, ampdac_capabilities *out) {
    if (out == nullptr) {
        set_error("ampdac_probe_bus_address: null capabilities");
        return AMPDAC_ERR_INVALID;
    }
    return probe_open_session(ampdac_open_bus_address(bus, address), out);
}

extern "C" void ampdac_set_volume(ampdac_session *session, float volume) {
    if (session == nullptr) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    session->volume.store(volume, std::memory_order_relaxed);
}

extern "C" void ampdac_close(ampdac_session *session) {
    if (session == nullptr) return;
    AmpdacKernelClaim kernel = session->kernel;
    if (session->ctx != nullptr) {
        session->ctx->dsd_manager.reset();
        teardown_context(session->ctx);
        delete session->ctx;
        session->ctx = nullptr;
    }
    ampdac_kernel_restore(kernel);
    delete session;
}

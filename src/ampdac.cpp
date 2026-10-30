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

#include <android/log.h>

#include "libusb/libusb.h"

#include "audio_gain.h"
#include "dop_formatter.h"
#include "dsd_playback_manager.h"
#include "dsd_wire_mode.h"
#include "native_dsd_formatter.h"
#include "pcm_wire_formatter.h"
#include "uac2_clock_control.h"
#include "uac2_feature_volume.h"
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
/* alsa/pcm_ampdac.c rejects a buffer under 64 KB. The hw pointer stays
 * put until USB consumes, so a larger prefill never arrives and the
 * stream never starts. */
constexpr std::size_t kClientBufferFloorBytes = 64U * 1024U;
/* Two in-flight queues: prime_out_buffers takes one, the ring must
 * still hold another when the first callback runs. 16 URBs of 8
 * microframes is 32 ms; 50 ms leaves that queue plus a margin. */
constexpr uint32_t kPrefillMs = 50;
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

bool wire_is_zero(const uint8_t *p, std::size_t n) noexcept {
    if (p == nullptr) return true;
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

/** Byte offset of the first 8-frame DoP run, or n if there is none. */
std::size_t dop_payload_offset(const uint8_t *p, std::size_t n) noexcept {
    if (p == nullptr || n < 64U) return n;
    for (std::size_t base = 0; base + 64U <= n; base += 8U) {
        uint8_t marker = p[base + 3U];
        if (marker != kDopMarkerA && marker != kDopMarkerB) continue;
        bool ok = true;
        for (std::size_t i = 0; i < 8U; ++i) {
            const std::size_t off = base + i * 8U;
            if (p[off + 3U] != marker || p[off + 7U] != marker) {
                ok = false;
                break;
            }
            marker = static_cast<uint8_t>(marker ^ kDopToggleMask);
        }
        if (ok) return base;
    }
    return n;
}

void apply_pcm_fade_in(uint8_t *wire, std::size_t bytes, int channels,
                       int bytes_per_sample, int &left, int total) noexcept {
    if (bytes_per_sample != 4) {
        left = 0;
        return;
    }
    if (wire == nullptr || left <= 0 || total <= 0 || channels <= 0) return;
    const std::size_t bpf = static_cast<std::size_t>(channels) * 4U;
    if (bytes < bpf) return;
    const std::size_t frames = bytes / bpf;
    for (std::size_t frame = 0; frame < frames && left > 0; ++frame) {
        const int done = total - left;
        for (int ch = 0; ch < channels; ++ch) {
            int32_t sample = 0;
            uint8_t *const p = wire + frame * bpf + static_cast<std::size_t>(ch) * 4U;
            std::memcpy(&sample, p, 4);
            sample = static_cast<int32_t>(
                    (static_cast<int64_t>(sample) * done) / total);
            std::memcpy(p, &sample, 4);
        }
        --left;
    }
}

PcmSourceEncoding source_encoding(ampdac_pcm_format format) {
    switch (format) {
        case AMPDAC_PCM_S16LE: return PcmSourceEncoding::S16Le;
        case AMPDAC_PCM_S24_3LE: return PcmSourceEncoding::S24_3Le;
        case AMPDAC_PCM_S32LE:
        case AMPDAC_PCM_F32LE:
        default:
            return PcmSourceEncoding::S32Le;
    }
}

/** bBitResolution, or the width implied by the slot when the DAC left it 0. */
uint8_t declared_pcm_bits(const Uac2AsGeneralInfo &as) noexcept {
    if (as.bit_resolution > 0U) return as.bit_resolution;
    if (as.subslot_size == 2U) return 16;
    if (as.subslot_size == 3U) return 24;
    if (as.subslot_size == 4U) return 32;
    return 0;
}

bool pcm_alt_can_carry(
        const Uac2StreamingEndpointInfo &candidate,
        int channel_count,
        int sample_rate_hz) noexcept {
    const auto &as = candidate.as_general;
    if (!as.is_present || as.format_type != UAC2_FORMAT_TYPE_I) return false;
    if ((as.bm_formats & UAC_FORMAT_TYPE_I_PCM) == 0U) return false;
    if (as.subslot_size != 2U && as.subslot_size != 3U && as.subslot_size != 4U) {
        return false;
    }
    if (as.nr_channels != 0U &&
        as.nr_channels != static_cast<uint8_t>(channel_count)) {
        return false;
    }
    if (declared_pcm_bits(as) == 0U) return false;
    const uint32_t frames =
            (static_cast<uint32_t>(sample_rate_hz) + 7999U) / 8000U;
    const uint32_t need = frames * as.subslot_size *
            static_cast<uint32_t>(channel_count);
    return candidate.effective_bytes_per_uframe >= need;
}

/** Lower is a better container for this declared width. */
int pcm_slot_rank(uint8_t bits, uint8_t subslot) noexcept {
    if (bits <= 16U) {
        if (subslot == 2U) return 0;
        if (subslot == 4U) return 1;
        return 2;
    }
    if (bits <= 24U) {
        if (subslot == 4U) return 0;
        if (subslot == 3U) return 1;
        return 2;
    }
    return subslot == 4U ? 0 : 1;
}

/**
 * Smallest advertised PCM width that can hold the file.
 * 32/24/16 → each file uses its own alt.
 * 32 and 16 → 32- and 24-bit files use 32, 16-bit uses 16.
 * 32 and 24 → 16- and 24-bit files use 24, 32-bit uses 32.
 * One alt carries every PCM file. A packed 3-byte slot loses to a 4-byte
 * slot of the same declared width.
 */
const Uac2StreamingEndpointInfo *select_pcm_alt(
        const std::vector<Uac2StreamingEndpointInfo> &candidates,
        uint8_t source_bits,
        int channel_count,
        int sample_rate_hz) {
    uint8_t target = 0;
    bool fits = false;
    for (const auto &candidate : candidates) {
        if (!pcm_alt_can_carry(candidate, channel_count, sample_rate_hz)) {
            continue;
        }
        const uint8_t bits = declared_pcm_bits(candidate.as_general);
        if (bits >= source_bits) {
            if (!fits || bits < target) {
                fits = true;
                target = bits;
            }
        } else if (!fits && bits > target) {
            target = bits;
        }
    }
    if (target == 0U) return nullptr;

    const Uac2StreamingEndpointInfo *best = nullptr;
    for (const auto &candidate : candidates) {
        if (!pcm_alt_can_carry(candidate, channel_count, sample_rate_hz)) {
            continue;
        }
        if (declared_pcm_bits(candidate.as_general) != target) continue;
        if (best == nullptr) {
            best = &candidate;
            continue;
        }
        const int rank = pcm_slot_rank(target, candidate.as_general.subslot_size);
        const int best_rank = pcm_slot_rank(target, best->as_general.subslot_size);
        if (rank < best_rank ||
            (rank == best_rank && candidate.alt_setting < best->alt_setting)) {
            best = &candidate;
        }
    }
    return best;
}

/**
 * Last resort: a DAC whose only PCM alternate is a packed 3-byte slot.
 * Four-byte selection has already failed for this rate and channel count.
 * DoP never comes here; it stays on a 4-byte slot.
 */
const Uac2StreamingEndpointInfo *prefer_packed_pcm24(
        const std::vector<Uac2StreamingEndpointInfo> &candidates,
        int channel_count,
        int sample_rate_hz) {
    const uint32_t frames =
            (static_cast<uint32_t>(sample_rate_hz) + 7999U) / 8000U;
    const uint32_t need = frames * 3U * static_cast<uint32_t>(channel_count);
    const Uac2StreamingEndpointInfo *best = nullptr;
    for (const auto &candidate : candidates) {
        const auto &as = candidate.as_general;
        if (!as.is_present || as.format_type != UAC2_FORMAT_TYPE_I) continue;
        if ((as.bm_formats & UAC_FORMAT_TYPE_I_PCM) == 0U) continue;
        if (as.subslot_size != 3U || as.bit_resolution > 24U) continue;
        if (as.nr_channels != 0U &&
            as.nr_channels != static_cast<uint8_t>(channel_count)) {
            continue;
        }
        if (candidate.effective_bytes_per_uframe < need) continue;
        const int rank = (as.bit_resolution == 24U || as.bit_resolution == 0U) ? 0 : 1;
        const int best_rank = best == nullptr
                ? 2
                : ((best->as_general.bit_resolution == 24U ||
                    best->as_general.bit_resolution == 0U) ? 0 : 1);
        if (best == nullptr || rank < best_rank ||
            (rank == best_rank && candidate.alt_setting < best->alt_setting)) {
            best = &candidate;
        }
    }
    return best;
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
    int pcm_fade_in_left = 0;
    int pcm_fade_in_total = 0;
    bool dop_lead_sent = false;
    uint32_t skipped_silence_bytes = 0;
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

struct VolumeAcLease {
    bool yielded = false;
    VolumeAcLease() {
        uac2_volume_service_start();
        uac2_volume_service_yield_ac();
        yielded = true;
    }
    void attached(libusb_device_handle *handle, int ac_interface) {
        uac2_volume_service_attach(handle, ac_interface);
        yielded = false;
    }
    ~VolumeAcLease() {
        if (yielded) {
            uac2_volume_service_ac_free();
        }
    }
};

/* Survives close. The next open uses it to tell a PCM↔DSD alt change
 * from a reopen in the same mode. */
bool g_last_stream_was_dsd = false;

ampdac_status program_usb_stream(
        ampdac_session *session,
        uint32_t usb_clock_hz,
        uint32_t bytes_per_frame,
        bool settle_pll) {
    UsbDriverContext *ctx = session->ctx;
    VolumeAcLease volume_ac;

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

    int early_ac = -1;
    (void)uac2_find_clock_source_id(ctx->device_handle, &early_ac);
    if (early_ac >= 0) {
        if (libusb_claim_interface(ctx->device_handle, early_ac) == LIBUSB_SUCCESS) {
            ctx->ctrl_interface_number = early_ac;
        }
    }
    /* KA13 volume is a host feature-unit write. It is applied once the
     * streaming alt is up and before any isochronous transfer is queued.
     * A control transfer after that wedges dwc_otg's periodic schedule. */

    int clock_iface = 0;
    int clock_id = 1;

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
        clock_iface = detected_ac_iface >= 0 ? detected_ac_iface : 0;
        clock_id = detected_clock_id > 0 ? detected_clock_id : 1;
        const int ctrl_iface = clock_iface;
        const int csid = clock_id;

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
            ctx->ctrl_interface_number = ctrl_iface;
        }
        return clock_rc;
    };

    /* PCM alt 1/2 and the RAW/DSD alt 3 share one interface. The PLL step
     * between them is the one SMSL stalls on if SET_CUR is followed by
     * another alt 0. Wait out that step before touching the clock. */
    const bool mode_switch = g_last_stream_was_dsd != session->dsd;
    int clock_rc = idle_then_clock(mode_switch);
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

    /* idle_then_clock already parked alt 0 and programmed the rate.
     * Do not idle again: that second SET_INTERFACE 0 is what makes the
     * following PCM↔DSD jump stall, after which the DAC re-enumerates. */
    UsbControlStatus alt = uac2_apply_alt_setting(
            ctx->device_handle,
            session->endpoint.interface_number,
            session->endpoint.alt_setting,
            session->endpoint.endpoint_address,
            false);
    if (alt != UsbControlStatus::Success) {
        (void)uac2_apply_alt_setting(
                ctx->device_handle,
                ctx->claimed_interface_number,
                0,
                0);
        usleep(100000U);
        int winning_clock_id = 0;
        const int again = uac2_force_clock_sample_rate(
                ctx->device_handle,
                static_cast<uint8_t>(clock_iface),
                clock_id,
                usb_clock_hz,
                &winning_clock_id);
        if (again == 4) {
            ctx->resolved_clock_source_id = static_cast<uint8_t>(winning_clock_id);
            ctx->ctrl_interface_number = clock_iface;
        }
        alt = uac2_apply_alt_setting(
                ctx->device_handle,
                session->endpoint.interface_number,
                session->endpoint.alt_setting,
                session->endpoint.endpoint_address,
                false);
    }
    if (alt != UsbControlStatus::Success) {
        set_error("SET_INTERFACE alt-setting failed");
        return AMPDAC_ERR_IO;
    }
    ctx->active_alt_setting = session->endpoint.alt_setting;
    ctx->was_dsd_session = session->dsd;
    g_last_stream_was_dsd = session->dsd;

    /* XMOS/FiiO drops a clock programmed at alt 0 when the streaming alt
     * comes up. The PLL then stays at the power-on rate while we send
     * 352.8 kHz, and the mismatch is a continuous crackle. Write the rate
     * again now that the alt which will actually stream is active. */
    if (ctx->resolved_clock_source_id != 0U && ctx->ctrl_interface_number >= 0) {
        int winning_clock_id = 0;
        const int locked = uac2_force_clock_sample_rate(
                ctx->device_handle,
                static_cast<uint8_t>(ctx->ctrl_interface_number),
                ctx->resolved_clock_source_id,
                usb_clock_hz,
                &winning_clock_id);
        if (locked != 4) {
            set_error("UAC2 SET_CUR clock sample rate failed after alt-setting");
            return AMPDAC_ERR_CLOCK;
        }
        ctx->resolved_clock_source_id = static_cast<uint8_t>(winning_clock_id);
    }
    /* The streaming alt brings KA13 back to 0 dB. Write the level the
     * buttons last chose, before any isochronous transfer. KA17, K7 and
     * the SMSL keep their own knob and are not written. */
    {
        libusb_device_descriptor volume_desc{};
        if (libusb_get_device_descriptor(libusb_get_device(ctx->device_handle),
                                         &volume_desc) == LIBUSB_SUCCESS &&
            volume_desc.idVendor == 0x2972U &&
            volume_desc.idProduct == 0x0062U) {
            uac2_volume_service_lock_full(ctx->device_handle);
        }
    }
    if (settle_pll) {
        usleep(kDsdPllSettleUs);
    }

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
    pool_cfg.max_packet_bytes = session->endpoint.effective_bytes_per_uframe;

    std::string pool_error;
    ctx->transfer_pool = IsoTransferPool::create(
            ctx->device_handle, pool_cfg, &pool_error);
    if (!ctx->transfer_pool) {
        set_error(std::string("IsoTransferPool: ") + pool_error);
        return AMPDAC_ERR_IO;
    }
    ctx->iso_endpoint_address = pool_cfg.endpoint_address;
    if (ctx->ctrl_interface_number >= 0) {
        volume_ac.attached(ctx->device_handle, ctx->ctrl_interface_number);
    }
    return AMPDAC_OK;
}

std::size_t playback_prefill_bytes(const UsbPcmWireFormat &wire) {
    const uint64_t bpf = wire.bytes_per_frame();
    uint64_t bytes = 0;
    if (wire.sample_rate_hz > 0U && bpf > 0U) {
        bytes = static_cast<uint64_t>(wire.sample_rate_hz) * bpf *
                kPrefillMs / 1000U;
    }
    const uint64_t primed = static_cast<uint64_t>(kIsoPoolSize) *
            kIsoPacketsPerTransfer * (bpf > 0U ? bpf : 8U);
    if (bytes < primed * 2U) bytes = primed * 2U;
    if (bytes > kClientBufferFloorBytes) bytes = kClientBufferFloorBytes;
    return static_cast<std::size_t>(bytes);
}

int push_wire_with_prefill(
        ampdac_session *session,
        const uint8_t *wire,
        std::size_t wire_bytes,
        bool *transfers_started,
        UsbPlaybackState live_state) {
    const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(kWriteTimeoutMs);
    const std::size_t prefill_goal =
            playback_prefill_bytes(session->ctx->pcm_wire_format);
    std::size_t chunk_offset = 0;
    while (chunk_offset < wire_bytes) {
        if (std::chrono::steady_clock::now() >= deadline) {
            set_error("ring buffer back-pressure timeout");
            return AMPDAC_ERR_TIMEOUT;
        }
        const std::size_t free_bytes = session->ctx->ring_buffer->free_space();
        if (free_bytes == 0U) {
            usleep(kWritePollUs);
            continue;
        }

        const std::size_t push_bytes = std::min(free_bytes, wire_bytes - chunk_offset);
        if (!session->ctx->ring_buffer->push(wire + chunk_offset, push_bytes)) {
            usleep(kWritePollUs);
            continue;
        }
        chunk_offset += push_bytes;

        if (!*transfers_started) {
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

    // Match the file to the narrowest PCM alt whose declared width can
    // hold it. DoP disguised as PCM at the carrier rates stays on the
    // 32-bit alt: that is the layout the marker already uses.
    const bool dop_carrier =
            config->sample_rate_hz == 176400U ||
            config->sample_rate_hz == 352800U ||
            config->sample_rate_hz == 705600U;
    uint8_t source_bits = 32;
    if (config->format == AMPDAC_PCM_S16LE) source_bits = 16;
    else if (config->format == AMPDAC_PCM_S24_3LE) source_bits = 24;
    const Uac2StreamingEndpointInfo *selected = nullptr;
    if (!(dop_carrier && source_bits != 16U)) {
        selected = select_pcm_alt(
                candidates,
                source_bits,
                static_cast<int>(config->channels),
                static_cast<int>(config->sample_rate_hz));
    }
    if (selected == nullptr) {
        selected = uac2_select_endpoint_for_format(
            candidates,
            32,
            static_cast<int>(config->channels),
            static_cast<int>(config->sample_rate_hz),
            true);
    }
    if (selected == nullptr) {
        selected = uac2_select_endpoint_for_format(
                candidates,
                32,
                static_cast<int>(config->channels),
                static_cast<int>(config->sample_rate_hz),
                false);
    }
    if (selected == nullptr) {
        selected = prefer_packed_pcm24(
                candidates,
                static_cast<int>(config->channels),
                static_cast<int>(config->sample_rate_hz));
    }
    if (selected == nullptr ||
        (selected->as_general.subslot_size != 4U &&
         selected->as_general.subslot_size != 3U &&
         selected->as_general.subslot_size != 2U)) {
        set_error("no matching UAC2 endpoint for requested PCM format");
        return AMPDAC_ERR_UNSUPPORTED;
    }

    session->endpoint = *selected;
    const uint8_t subslot = selected->as_general.subslot_size;
    uint8_t valid_bits =
            selected->as_general.bit_resolution > 0U
                ? selected->as_general.bit_resolution
                : static_cast<uint8_t>(subslot == 3U ? 24 : 32);
    if (subslot == 3U && valid_bits > 24U) valid_bits = 24;
    if (subslot == 2U && valid_bits > 16U) valid_bits = 16;
    ctx->pcm_valid_bit_depth = valid_bits;
    ctx->pcm_wire_format = UsbPcmWireFormat{
            config->sample_rate_hz,
            config->channels,
            subslot,
            valid_bits,
    };
    if (!ctx->pcm_wire_format.is_valid()) {
        set_error("negotiated PCM wire format is invalid");
        return AMPDAC_ERR_FORMAT;
    }
    (void)ctx->playback_state.transition_to(UsbPlaybackState::Configured);
    session->dsd = false;

    const ampdac_status usb_st = program_usb_stream(
            session,
            config->sample_rate_hz,
            static_cast<uint32_t>(config->channels) * subslot,
            true);
    if (usb_st != AMPDAC_OK) return usb_st;

    session->format = config->format;
    session->volume.store(config->volume, std::memory_order_relaxed);
    session->dsd = false;
    session->pcm_fade_in_left = 0;
    session->pcm_fade_in_total = 0;
    session->dop_lead_sent = false;
    session->skipped_silence_bytes = 0;
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
    session->dsd = true;
    session->native_dsd = native;

    const ampdac_status usb_st = program_usb_stream(
            session,
            usb_clock_hz,
            static_cast<uint32_t>(kDsdStereoWireBytes),
            true);
    if (usb_st != AMPDAC_OK) return usb_st;

    session->dsd = true;
    session->native_dsd = native;
    session->dop_marker = kDopMarkerA;
    session->pcm_fade_in_left = 0;
    session->pcm_fade_in_total = 0;
    session->dop_lead_sent = false;
    session->skipped_silence_bytes = 0;
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
    /* Feedback IN is already queued. The button read joins that same
     * handle so dwc_otg keeps one periodic schedule. */
    uac2_volume_service_arm_session_hid(session->ctx->device_handle);
    (void)session->ctx->playback_state.transition_to(UsbPlaybackState::Priming);
    session->started = true;
    return AMPDAC_OK;
}

extern "C" ampdac_status ampdac_drop(ampdac_session *session) {
    if (session == nullptr || session->ctx == nullptr) {
        set_error("ampdac_drop: invalid session");
        return AMPDAC_ERR_INVALID;
    }
    session->dsd_carry.clear();
    if (!session->started || session->ctx->ring_buffer == nullptr ||
        session->ctx->transfer_pool == nullptr) {
        return AMPDAC_OK;
    }
    /* PCM: the next USB buffers ramp the last sample to zero. A hard step
     * to digital silence is the click at every track change.
     * DoP rides inside PCM (DeaDBeeF ffmpeg). Ramping those samples
     * clears the marker and the DAC leaves DSD. */
    const bool dop_pcm = !session->dsd &&
            session->ctx->transfer_pool->stream_is_dop();
    if (!session->dsd && !dop_pcm) {
        const uint32_t rate = session->ctx->pcm_wire_format.sample_rate_hz;
        int frames = rate > 0U ? static_cast<int>(rate / 125U) : 352;
        if (frames < 32) frames = 32;
        if (frames > 8000) frames = 8000;
        session->ctx->transfer_pool->request_pcm_fade(frames);
        session->pcm_fade_in_left = frames;
        session->pcm_fade_in_total = frames;
    }
    /* New completions play silence or the ramp. A callback that already
     * took the ring pointer finishes its pop before the indices are cleared. */
    session->ctx->transfer_pool->attach_ring_buffer(nullptr);
    usleep(1000);
    session->ctx->ring_buffer->reset();
    if ((session->dsd && !session->native_dsd) || dop_pcm) {
        /* Continue the marker the wire just used. A silence pattern that
         * restarts at 0x05 while the stream was on 0xFA drops DoP lock. */
        const uint8_t last = session->ctx->transfer_pool->last_dop_marker();
        uint8_t marker = (last == kDopMarkerA || last == kDopMarkerB)
                ? static_cast<uint8_t>(last ^ kDopToggleMask)
                : session->dop_marker;
        uint8_t block[64U * kDsdStereoWireBytes];
        for (int i = 0; i < 4; ++i) {
            marker = fill_dop_idle_stereo(block, 64U, marker);
            (void)session->ctx->ring_buffer->push(block, sizeof(block));
        }
        session->dop_marker = marker;
    } else if (session->native_dsd) {
        uint8_t block[2048];
        std::memset(block, kNativeDsdSilenceByte, sizeof(block));
        (void)session->ctx->ring_buffer->push(block, sizeof(block));
    }
    session->ctx->transfer_pool->attach_ring_buffer(session->ctx->ring_buffer.get());
    return AMPDAC_OK;
}

void log_first_wire_peak(const uint8_t *payload, std::size_t bytes,
                         std::size_t slot) {
    static std::atomic<int> logged{0};
    static std::atomic<int> silent_chunks{0};
    if (logged.load(std::memory_order_relaxed) != 0 ||
        payload == nullptr || slot == 0U || bytes < slot) {
        return;
    }
    int32_t peak = 0;
    const std::size_t samples = bytes / slot;
    const std::size_t limit = samples > 4096U ? 4096U : samples;
    for (std::size_t i = 0; i < limit; ++i) {
        const uint8_t *p = payload + i * slot;
        int32_t sample = 0;
        if (slot == 2U) {
            sample = static_cast<int16_t>(
                    static_cast<uint16_t>(p[0]) |
                    (static_cast<uint16_t>(p[1]) << 8));
        } else if (slot == 3U) {
            sample = static_cast<int32_t>(p[0] |
                    (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16));
            if ((sample & 0x800000) != 0) sample |= ~0xFFFFFF;
        } else {
            sample = static_cast<int32_t>(
                    static_cast<uint32_t>(p[0]) |
                    (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16) |
                    (static_cast<uint32_t>(p[3]) << 24));
        }
        if (sample < 0) {
            sample = sample == static_cast<int32_t>(0x80000000u)
                    ? 0x7FFFFFFF
                    : -sample;
        }
        if (sample > peak) peak = sample;
    }
    if (peak == 0 && silent_chunks.fetch_add(1, std::memory_order_relaxed) < 8) {
        return;
    }
    if (logged.exchange(1, std::memory_order_relaxed) != 0) return;
    char hex[48];
    int pos = 0;
    const int shown = bytes < 12U ? static_cast<int>(bytes) : 12;
    for (int i = 0; i < shown && pos < static_cast<int>(sizeof(hex)) - 3; ++i) {
        pos += std::snprintf(hex + pos, sizeof(hex) - static_cast<std::size_t>(pos),
                             "%02X", payload[i]);
    }
    __android_log_print(ANDROID_LOG_INFO, "UsbAudioBridge",
                        "wire peak %d slot %zu bytes %zu head %s",
                        static_cast<int>(peak), slot, bytes, hex);
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

        return static_cast<int>(bytes);
    }

    const double gain = ui_position_to_gain(
            static_cast<double>(session->volume.load(std::memory_order_relaxed)));
    const PcmWireFormatter formatter(session->ctx->pcm_wire_format);

    std::size_t input_offset = 0;
    while (input_offset < bytes) {
        const std::size_t remaining = bytes - input_offset;
        std::size_t in_chunk = remaining;
        const std::size_t slot = session->ctx->pcm_wire_format.subslot_bytes;
        const std::size_t samples = slot > 0U ? wire_chunk.size() / slot : 0U;
        if (session->format == AMPDAC_PCM_S16LE) {
            in_chunk = std::min(remaining, samples * 2U);
            in_chunk -= in_chunk % 2U;
        } else if (session->format == AMPDAC_PCM_S24_3LE) {
            in_chunk = std::min(remaining, samples * 3U);
            in_chunk -= in_chunk % 3U;
        } else {
            in_chunk = std::min(remaining, samples * 4U);
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
        if (session->pcm_fade_in_left > 0 &&
            !session->ctx->transfer_pool->stream_is_dop()) {
            apply_pcm_fade_in(wire_chunk.data(), formatted.bytes_written,
                              session->ctx->pcm_wire_format.channel_count,
                              session->ctx->pcm_wire_format.subslot_bytes,
                              session->pcm_fade_in_left,
                              session->pcm_fade_in_total);
        }

        /* DeaDBeeF writes a period of zeros before DoP is ready. Those zeros
         * go out as PCM and the DAC sometimes never enters DSD. Hold them
         * until real samples arrive, then start DoP from marked silence.
         * Bytes before the first marker must not be queued: SMSL samples
         * the format at the start of the stream and will not switch later. */
        const uint8_t *payload = wire_chunk.data();
        std::size_t payload_bytes = formatted.bytes_written;
        if (!session->transfers_started) {
            const auto &wire_fmt = session->ctx->pcm_wire_format;
            const uint32_t bpf = static_cast<uint32_t>(wire_fmt.bytes_per_frame());
            const uint32_t silence_cap = wire_fmt.sample_rate_hz > 0 && bpf > 0
                    ? (wire_fmt.sample_rate_hz / 20U) * bpf
                    : 0U;
            const std::size_t dop_at = dop_payload_offset(
                    wire_chunk.data(), formatted.bytes_written);
            if (dop_at < formatted.bytes_written) {
                session->ctx->transfer_pool->mark_stream_dop();
                session->skipped_silence_bytes = 0;
                if (!session->dop_lead_sent) {
                    session->dop_lead_sent = true;
                    /* 50 ms of marked silence before the music. SMSL decides
                     * PCM vs DSD from the start of the stream and will not
                     * switch later if that start was zeros. */
                    int left = wire_fmt.sample_rate_hz > 0
                            ? static_cast<int>(wire_fmt.sample_rate_hz / 20U)
                            : 882;
                    if (left < 128) left = 128;
                    if (left > 8192) left = 8192;
                    uint8_t marker = kDopMarkerA;
                    uint8_t block[64U * kDsdStereoWireBytes];
                    while (left > 0) {
                        const int n = left > 64 ? 64 : left;
                        marker = fill_dop_idle_stereo(
                                block, static_cast<std::size_t>(n), marker);
                        std::size_t off = 0;
                        const std::size_t nbytes =
                                static_cast<std::size_t>(n) * kDsdStereoWireBytes;
                        while (off < nbytes) {
                            const std::size_t free_bytes =
                                    session->ctx->ring_buffer->free_space();
                            if (free_bytes == 0U) {
                                usleep(kWritePollUs);
                                continue;
                            }
                            const std::size_t take = std::min(free_bytes, nbytes - off);
                            if (!session->ctx->ring_buffer->push(block + off, take)) {
                                usleep(kWritePollUs);
                                continue;
                            }
                            off += take;
                        }
                        left -= n;
                    }
                    session->dop_marker = marker;
                }
                payload += dop_at;
                payload_bytes -= dop_at;
            } else if (wire_is_zero(wire_chunk.data(), formatted.bytes_written) &&
                       silence_cap > 0U &&
                       session->skipped_silence_bytes < silence_cap) {
                session->skipped_silence_bytes +=
                        static_cast<uint32_t>(formatted.bytes_written);
                input_offset += in_chunk;
                continue;
            } else {
                session->skipped_silence_bytes = 0;
            }
        }

        if (payload_bytes == 0U) {
            input_offset += in_chunk;
            continue;
        }
        log_first_wire_peak(
                payload,
                payload_bytes,
                session->ctx->pcm_wire_format.subslot_bytes);
        const int pushed = push_wire_with_prefill(
                session,
                payload,
                payload_bytes,
                &transfers_started,
                UsbPlaybackState::StreamingPcm);
        if (pushed < 0) return pushed;
        input_offset += in_chunk;
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

static ampdac_status fill_capabilities(ampdac_session *session,
                                        ampdac_capabilities *out,
                                        bool take_kernel) {
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
        if (out->alt_count < AMPDAC_MAX_ALTS) {
            ampdac_alt_info &slot = out->alts[out->alt_count];
            slot.interface_number = ep.interface_number;
            slot.alt_setting = ep.alt_setting;
            slot.channels = ep.as_general.nr_channels;
            slot.subslot_bytes = ep.as_general.subslot_size;
            slot.valid_bits = ep.as_general.bit_resolution;
            slot.native_dsd = uac2_classify_endpoint_dsd(ep.as_general).is_native_dsd
                    ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
            slot.bytes_per_uframe = ep.effective_bytes_per_uframe;
            out->alt_count++;
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
    if (take_kernel) {
        std::string kernel_error;
        (void)ampdac_kernel_takeover(handle, &session->kernel, &kernel_error);
    }
    /* A live session already holds this interface. Releasing it would drop
     * the claim the stream is using. Release only a claim this call made. */
    const int ctrl_claim = libusb_claim_interface(handle, ctrl);
    if (clock_id > 0) {
        out->sample_rate_count = uac2_query_clock_sample_rates(
                handle,
                static_cast<uint8_t>(ctrl),
                static_cast<uint8_t>(clock_id),
                out->sample_rates,
                AMPDAC_MAX_RATES);
    }
    if (ctrl_claim == LIBUSB_SUCCESS) {
        libusb_release_interface(handle, ctrl);
    }

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

    return AMPDAC_OK;
}

static ampdac_status probe_open_session(ampdac_session *session,
                                        ampdac_capabilities *out) {
    const ampdac_status st = fill_capabilities(session, out, true);
    if (session != nullptr) ampdac_close(session);
    return st;
}

extern "C" ampdac_status ampdac_describe(ampdac_session *session,
                                         ampdac_capabilities *out) {
    if (out == nullptr) return AMPDAC_ERR_INVALID;
    return fill_capabilities(session, out, false);
}

extern "C" ampdac_status ampdac_session_device(const ampdac_session *session,
                                               ampdac_device_info *out) {
    if (session == nullptr || session->ctx == nullptr ||
        session->ctx->device_handle == nullptr || out == nullptr) {
        return AMPDAC_ERR_INVALID;
    }
    std::memset(out, 0, sizeof(*out));
    libusb_device_handle *handle = session->ctx->device_handle;
    libusb_device *dev = libusb_get_device(handle);
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) {
        return AMPDAC_ERR_IO;
    }
    out->vid = desc.idVendor;
    out->pid = desc.idProduct;
    out->bus = libusb_get_bus_number(dev);
    out->address = libusb_get_device_address(dev);
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

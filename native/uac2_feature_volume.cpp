#include "uac2_feature_volume.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/input.h>

#include "libusb/libusb.h"

static constexpr const char *VOL_TAG = "UsbAudioBridge";

#define VLOGD(...) __android_log_print(ANDROID_LOG_DEBUG, VOL_TAG, __VA_ARGS__)
#define VLOGI(...) __android_log_print(ANDROID_LOG_INFO,  VOL_TAG, __VA_ARGS__)
#define VLOGW(...) __android_log_print(ANDROID_LOG_WARN,  VOL_TAG, __VA_ARGS__)

namespace {

/** KA13 level in 1/256 dB, shared by the feature unit and the kernel mixer.
 *  0 is full scale. The two paths must store this same number: adding a
 *  step onto a stale copy makes the next driver quieter than the one
 *  the listener just set. */
std::atomic<int> g_ka13_level{-21 * 256};

constexpr uint8_t kAudioClass = 0x01U;
constexpr uint8_t kAudioControlSubclass = 0x01U;
constexpr uint8_t kHidClass = 0x03U;
constexpr uint8_t kCsInterface = 0x24U;
constexpr uint8_t kFeatureUnitSubtype = 0x06U;
constexpr uint8_t kRequestSetCur = 0x01U;
constexpr uint8_t kRequestGetRange = 0x02U;
constexpr uint8_t kRequestGetCur = 0x81U;
constexpr uint8_t kHostToClassInterface = 0x21U;
constexpr uint8_t kDeviceToClassInterface = 0xA1U;
constexpr uint8_t kCsMute = 0x01U;
constexpr uint8_t kCsVolume = 0x02U;
constexpr unsigned int kControlTimeoutMs = 300U;
constexpr int kMaxVolumeChannels = 8;
constexpr int kHidReportBytes = 256;
constexpr uint32_t kConsumerPage = 0x0CU;
constexpr uint32_t kUsageVolume = (kConsumerPage << 16) | 0x00E0U;
constexpr uint32_t kUsageVolumeInc = (kConsumerPage << 16) | 0x00E9U;
constexpr uint32_t kUsageVolumeDec = (kConsumerPage << 16) | 0x00EAU;

constexpr int kHoldDelayMs = 300;
constexpr int kHoldRepeatMs = 80;
/* A timeout of 0 leaves the interrupt URB pending forever. On dwc_otg that
 * stalls explicit-feedback IN (one completion every minute) and the DAC
 * stays silent. One second still catches a press and lets ISO run. */
constexpr unsigned int kHidIdleMs = 1000;

struct FeatureVolume {
    int ac_interface = -1;
    uint8_t unit_id = 0;
    uint8_t channels[kMaxVolumeChannels] = {};
    int channel_count = 0;
    int mute_channel = -1;
    int16_t minimum = 0;
    int16_t maximum = 0;
    int16_t step = 256;
    int16_t current = 0;
};

uint32_t read_le(const uint8_t *p, int size) {
    uint32_t value = 0;
    for (int i = 0; i < size; ++i) {
        value |= static_cast<uint32_t>(p[i]) << (8 * i);
    }
    return value;
}

bool volume_writable(uint32_t controls) {
    // UAC2 packs each control as a 2-bit pair. Volume is control 2, so
    // the writable bit is bit 3.
    return ((controls >> 3) & 0x1U) != 0U;
}

bool mute_writable(uint32_t controls) {
    return ((controls >> 1) & 0x1U) != 0U;
}

int16_t choose_step(int16_t resolution, int16_t minimum, int16_t maximum) {
    int32_t res = resolution;
    if (res <= 0) {
        res = 256;
    }
    int32_t step = res;
    // A resolution of 1/256 dB would make one click inaudible. Advance
    // by at least 1 dB, staying on the device's resolution grid.
    if (step < 256) {
        const int32_t ticks = (256 + res - 1) / res;
        step = ticks * res;
    }
    const int32_t span = static_cast<int32_t>(maximum) - static_cast<int32_t>(minimum);
    if (span > 0 && step > span) {
        step = res;
    }
    if (step <= 0) {
        step = 256;
    }
    if (step > 32767) {
        step = 32767;
    }
    return static_cast<int16_t>(step);
}

uint16_t control_value(uint8_t cs, int channel) {
    return static_cast<uint16_t>((static_cast<uint16_t>(cs) << 8) |
                                 (static_cast<uint16_t>(channel) & 0xFFU));
}

uint16_t control_index(uint8_t unit_id, int ac_interface) {
    return static_cast<uint16_t>((static_cast<uint16_t>(unit_id) << 8) |
                                 (static_cast<uint16_t>(ac_interface) & 0xFFU));
}

bool get_s16(libusb_device_handle *handle,
             int ac_interface,
             uint8_t unit_id,
             uint8_t cs,
             int channel,
             int16_t *out) {
    unsigned char data[2] = {};
    const int n = libusb_control_transfer(
            handle,
            kDeviceToClassInterface,
            kRequestGetCur,
            control_value(cs, channel),
            control_index(unit_id, ac_interface),
            data,
            sizeof(data),
            kControlTimeoutMs);
    if (n != 2 || out == nullptr) {
        return false;
    }
    *out = static_cast<int16_t>(data[0] | (data[1] << 8));
    return true;
}

bool get_u8(libusb_device_handle *handle,
            int ac_interface,
            uint8_t unit_id,
            uint8_t cs,
            int channel,
            uint8_t *out) {
    unsigned char data[1] = {};
    const int n = libusb_control_transfer(
            handle,
            kDeviceToClassInterface,
            kRequestGetCur,
            control_value(cs, channel),
            control_index(unit_id, ac_interface),
            data,
            sizeof(data),
            kControlTimeoutMs);
    if (n != 1 || out == nullptr) {
        return false;
    }
    *out = data[0];
    return true;
}

bool set_u8(libusb_device_handle *handle,
            int ac_interface,
            uint8_t unit_id,
            uint8_t cs,
            int channel,
            uint8_t value) {
    unsigned char data[1] = {value};
    const int n = libusb_control_transfer(
            handle,
            kHostToClassInterface,
            kRequestSetCur,
            control_value(cs, channel),
            control_index(unit_id, ac_interface),
            data,
            sizeof(data),
            kControlTimeoutMs);
    return n == 1;
}

bool set_s16(libusb_device_handle *handle,
             int ac_interface,
             uint8_t unit_id,
             uint8_t cs,
             int channel,
             int16_t value) {
    unsigned char data[2] = {
            static_cast<unsigned char>(value & 0xFF),
            static_cast<unsigned char>((value >> 8) & 0xFF),
    };
    const int n = libusb_control_transfer(
            handle,
            kHostToClassInterface,
            kRequestSetCur,
            control_value(cs, channel),
            control_index(unit_id, ac_interface),
            data,
            sizeof(data),
            kControlTimeoutMs);
    return n == 2;
}

bool read_volume_range(libusb_device_handle *handle,
                       int ac_interface,
                       uint8_t unit_id,
                       int channel,
                       int16_t *minimum,
                       int16_t *maximum,
                       int16_t *resolution) {
    unsigned char data[64] = {};
    const int n = libusb_control_transfer(
            handle,
            kDeviceToClassInterface,
            kRequestGetRange,
            control_value(kCsVolume, channel),
            control_index(unit_id, ac_interface),
            data,
            sizeof(data),
            kControlTimeoutMs);
    if (n < 8) {
        return false;
    }
    const uint16_t subranges = static_cast<uint16_t>(data[0] | (data[1] << 8));
    if (subranges == 0U) {
        return false;
    }
    const int16_t min_v = static_cast<int16_t>(data[2] | (data[3] << 8));
    const int16_t max_v = static_cast<int16_t>(data[4] | (data[5] << 8));
    const int16_t res_v = static_cast<int16_t>(data[6] | (data[7] << 8));
    if (minimum != nullptr) {
        *minimum = min_v <= max_v ? min_v : max_v;
    }
    if (maximum != nullptr) {
        *maximum = min_v <= max_v ? max_v : min_v;
    }
    if (resolution != nullptr) {
        *resolution = res_v;
    }
    return true;
}

bool find_feature_volume(libusb_device_handle *handle,
                         int ac_interface,
                         FeatureVolume *out) {
    libusb_device *device = libusb_get_device(handle);
    libusb_config_descriptor *config = nullptr;
    int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != LIBUSB_SUCCESS) {
        rc = libusb_get_config_descriptor(device, 0, &config);
    }
    if (rc != LIBUSB_SUCCESS || config == nullptr) {
        return false;
    }

    bool found = false;
    for (int i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface &iface = config->interface[i];
        for (int alt_index = 0; alt_index < iface.num_altsetting; ++alt_index) {
            const libusb_interface_descriptor &alt = iface.altsetting[alt_index];
            if (alt.bInterfaceNumber != ac_interface ||
                alt.bInterfaceClass != kAudioClass ||
                alt.bInterfaceSubClass != kAudioControlSubclass ||
                alt.extra == nullptr || alt.extra_length < 6) {
                continue;
            }
            const uint8_t *p = alt.extra;
            int left = alt.extra_length;
            while (left >= 3) {
                const int len = p[0];
                if (len < 3 || len > left) {
                    break;
                }
                if (p[1] == kCsInterface && p[2] == kFeatureUnitSubtype && len >= 6) {
                    const int control_bytes = len - 6;
                    if (control_bytes >= 4 && (control_bytes % 4) == 0) {
                        FeatureVolume candidate;
                        candidate.ac_interface = ac_interface;
                        candidate.unit_id = p[3];
                        const int controls = control_bytes / 4;
                        for (int ch = 0; ch < controls && ch < 16; ++ch) {
                            const uint8_t *raw = p + 5 + (ch * 4);
                            const uint32_t bits = read_le(raw, 4);
                            if (ch == 0 && mute_writable(bits)) {
                                candidate.mute_channel = 0;
                            }
                            if (volume_writable(bits) &&
                                candidate.channel_count < kMaxVolumeChannels) {
                                candidate.channels[candidate.channel_count++] =
                                        static_cast<uint8_t>(ch);
                            }
                        }
                        if (candidate.channel_count > 0) {
                            *out = candidate;
                            found = true;
                            break;
                        }
                    }
                }
                p += len;
                left -= len;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return found;
}

struct HidCandidate {
    int interface_number = -1;
    uint8_t endpoint = 0;
    int packet_bytes = 8;
    Uac2HidVolumeReport report;
};

int read_hid_report_descriptor(libusb_device_handle *handle,
                               int interface_number,
                               uint8_t *buf,
                               int cap) {
    return libusb_control_transfer(
            handle,
            LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD |
                    LIBUSB_RECIPIENT_INTERFACE,
            LIBUSB_REQUEST_GET_DESCRIPTOR,
            static_cast<uint16_t>(0x22U << 8),
            static_cast<uint16_t>(interface_number),
            buf,
            static_cast<uint16_t>(cap),
            kControlTimeoutMs);
}

bool find_hid_volume(libusb_device_handle *handle, HidCandidate *out) {
    libusb_device *device = libusb_get_device(handle);
    libusb_config_descriptor *config = nullptr;
    int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != LIBUSB_SUCCESS) {
        rc = libusb_get_config_descriptor(device, 0, &config);
    }
    if (rc != LIBUSB_SUCCESS || config == nullptr) {
        return false;
    }

    bool found = false;
    for (int i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface &iface = config->interface[i];
        if (iface.num_altsetting < 1) {
            continue;
        }
        const libusb_interface_descriptor &alt = iface.altsetting[0];
        if (alt.bInterfaceClass != kHidClass) {
            continue;
        }
        uint8_t endpoint = 0;
        int packet = 8;
        for (int e = 0; e < alt.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor &ep = alt.endpoint[e];
            const bool in = (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
            const bool interrupt = (ep.bmAttributes & 0x03U) == LIBUSB_TRANSFER_TYPE_INTERRUPT;
            if (in && interrupt) {
                endpoint = ep.bEndpointAddress;
                packet = ep.wMaxPacketSize & 0x07FF;
                break;
            }
        }
        if (endpoint == 0) {
            continue;
        }

        uint8_t desc[kHidReportBytes];
        int n = read_hid_report_descriptor(handle, alt.bInterfaceNumber, desc, sizeof(desc));
        bool temporary_claim = false;
        if (n < 0) {
            /* Detach only this HID interface. auto-detach on the handle
             * uses DISCONNECT_CLAIM, which fails when no driver is bound
             * and would also be armed for the audio interfaces. */
            (void)libusb_detach_kernel_driver(handle, alt.bInterfaceNumber);
            if (libusb_claim_interface(handle, alt.bInterfaceNumber) == LIBUSB_SUCCESS) {
                temporary_claim = true;
                n = read_hid_report_descriptor(
                        handle, alt.bInterfaceNumber, desc, sizeof(desc));
            } else {
                (void)libusb_attach_kernel_driver(handle, alt.bInterfaceNumber);
            }
        }
        Uac2HidVolumeReport report;
        const bool useful = n > 0 &&
                uac2_hid_parse_consumer_volume(desc, static_cast<std::size_t>(n), &report);
        if (!useful) {
            if (temporary_claim) {
                (void)libusb_release_interface(handle, alt.bInterfaceNumber);
                (void)libusb_attach_kernel_driver(handle, alt.bInterfaceNumber);
            }
            continue;
        }
        if (!temporary_claim) {
            (void)libusb_detach_kernel_driver(handle, alt.bInterfaceNumber);
            if (libusb_claim_interface(handle, alt.bInterfaceNumber) != LIBUSB_SUCCESS) {
                (void)libusb_attach_kernel_driver(handle, alt.bInterfaceNumber);
                continue;
            }
        }
        out->interface_number = alt.bInterfaceNumber;
        out->endpoint = endpoint;
        if (packet < 1) {
            packet = 8;
        }
        if (packet > 64) {
            packet = 64;
        }
        out->packet_bytes = packet;
        out->report = report;
        found = true;
    }
    libusb_free_config_descriptor(config);
    return found;
}

bool load_range_and_current(libusb_device_handle *handle, FeatureVolume *volume) {
    bool any = false;
    int16_t minimum = 0;
    int16_t maximum = 0;
    int16_t resolution = 256;
    for (int i = 0; i < volume->channel_count; ++i) {
        int16_t ch_min = 0;
        int16_t ch_max = 0;
        int16_t ch_res = 0;
        if (!read_volume_range(handle, volume->ac_interface, volume->unit_id,
                               volume->channels[i], &ch_min, &ch_max, &ch_res)) {
            continue;
        }
        if (!any) {
            minimum = ch_min;
            maximum = ch_max;
            resolution = ch_res;
            any = true;
        } else {
            if (ch_min > minimum) {
                minimum = ch_min;
            }
            if (ch_max < maximum) {
                maximum = ch_max;
            }
            if (ch_res > resolution) {
                resolution = ch_res;
            }
        }
    }
    if (!any || minimum > maximum) {
        return false;
    }
    int16_t current = 0;
    if (!get_s16(handle, volume->ac_interface, volume->unit_id, kCsVolume,
                 volume->channels[0], &current)) {
        return false;
    }
    if (current < minimum) {
        current = minimum;
    }
    if (current > maximum) {
        current = maximum;
    }
    volume->minimum = minimum;
    volume->maximum = maximum;
    volume->step = choose_step(resolution, minimum, maximum);
    volume->current = current;
    return true;
}

bool bit_on(const uint8_t *buf, int length, int bit) {
    if (bit < 0) {
        return false;
    }
    const int byte = bit / 8;
    if (byte < 0 || byte >= length) {
        return false;
    }
    return (buf[byte] & static_cast<uint8_t>(1U << (bit % 8))) != 0;
}

int32_t sign_extend(uint32_t raw, int bits) {
    if (bits <= 0 || bits >= 32) {
        return static_cast<int32_t>(raw);
    }
    const uint32_t sign = 1U << (bits - 1);
    return static_cast<int32_t>((raw ^ sign) - sign);
}

}  // namespace

bool uac2_hid_parse_consumer_volume(const uint8_t *desc,
                                    std::size_t length,
                                    Uac2HidVolumeReport *out) noexcept {
    if (desc == nullptr || out == nullptr || length == 0U) {
        return false;
    }
    out->up_bit = -1;
    out->down_bit = -1;
    out->relative_bit = -1;
    out->relative_bits = 0;
    out->report_id = 0;

    uint32_t usage_page = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    int report_id = 0;
    int id_bits = 0;
    int payload_bit = 0;
    uint32_t usages[32];
    int usage_count = 0;
    int usage_min = -1;
    int usage_max = -1;

    std::size_t index = 0;
    while (index < length) {
        const uint8_t prefix = desc[index++];
        if (prefix == 0xFEU) {
            if (index + 2 > length) {
                break;
            }
            const uint8_t data_size = desc[index++];
            ++index;
            if (index + data_size > length) {
                break;
            }
            index += data_size;
            continue;
        }
        const uint8_t size_code = prefix & 0x03U;
        const int size = size_code == 3 ? 4 : size_code;
        const uint8_t type = static_cast<uint8_t>((prefix >> 2) & 0x03U);
        const uint8_t tag = static_cast<uint8_t>((prefix >> 4) & 0x0FU);
        if (index + static_cast<std::size_t>(size) > length) {
            break;
        }
        const uint32_t data = size > 0 ? read_le(desc + index, size) : 0U;
        index += static_cast<std::size_t>(size);

        if (type == 1U) {
            if (tag == 0U) {
                usage_page = data;
            } else if (tag == 7U) {
                report_size = data;
            } else if (tag == 8U) {
                report_id = static_cast<int>(data);
                id_bits = report_id != 0 ? 8 : 0;
                payload_bit = 0;
            } else if (tag == 9U) {
                report_count = data;
            }
        } else if (type == 2U) {
            const uint32_t full = data > 0xFFFFU
                    ? data
                    : ((usage_page << 16) | (data & 0xFFFFU));
            if (tag == 0U) {
                if (usage_count < 32) {
                    usages[usage_count++] = full;
                }
            } else if (tag == 1U) {
                usage_min = static_cast<int>(data & 0xFFFFU);
            } else if (tag == 2U) {
                usage_max = static_cast<int>(data & 0xFFFFU);
            }
        } else if (type == 0U) {
            if (tag == 8U && report_size > 0U && report_count > 0U) {
                const bool constant = (data & 0x1U) != 0U;
                const bool variable = (data & 0x2U) != 0U;
                uint32_t fields[64];
                int field_count = 0;
                if (usage_count > 0) {
                    field_count = usage_count < 64 ? usage_count : 64;
                    for (int u = 0; u < field_count; ++u) {
                        fields[u] = usages[u];
                    }
                } else if (usage_min >= 0 && usage_max >= usage_min) {
                    for (int u = usage_min; u <= usage_max && field_count < 64; ++u) {
                        fields[field_count++] =
                                (usage_page << 16) | (static_cast<uint32_t>(u) & 0xFFFFU);
                    }
                }
                if (!constant && variable) {
                    for (uint32_t field = 0; field < report_count; ++field) {
                        uint32_t usage = 0;
                        if (field_count > 0) {
                            usage = field < static_cast<uint32_t>(field_count)
                                    ? fields[field]
                                    : fields[field_count - 1];
                        }
                        const int bit = id_bits + payload_bit +
                                static_cast<int>(field * report_size);
                        if (usage == kUsageVolumeInc && report_size == 1U &&
                            out->up_bit < 0) {
                            out->up_bit = bit;
                            out->report_id = report_id;
                        } else if (usage == kUsageVolumeDec && report_size == 1U &&
                                   out->down_bit < 0) {
                            out->down_bit = bit;
                            out->report_id = report_id;
                        } else if (usage == kUsageVolume && out->relative_bit < 0 &&
                                   (report_size == 8U || report_size == 16U)) {
                            out->relative_bit = bit;
                            out->relative_bits = static_cast<int>(report_size);
                            out->report_id = report_id;
                        }
                    }
                }
                const uint64_t advance =
                        static_cast<uint64_t>(report_size) * report_count;
                if (advance > 65535U) {
                    break;
                }
                payload_bit += static_cast<int>(advance);
            }
            usage_count = 0;
            usage_min = -1;
            usage_max = -1;
        }
    }
    return out->up_bit >= 0 || out->down_bit >= 0 || out->relative_bit >= 0;
}

namespace {

void vol_note(const char *fmt, ...) {
    char buf[320];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    VLOGI("%s", buf);
    FILE *file = std::fopen("/tmp/ampdac-volume.log", "a");
    if (file != nullptr) {
        std::fputs(buf, file);
        std::fputc('\n', file);
        std::fclose(file);
        (void)chmod("/tmp/ampdac-volume.log", 0644);
    }
}

int audio_control_interface(libusb_device *device) {
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS &&
        libusb_get_config_descriptor(device, 0, &config) != LIBUSB_SUCCESS) {
        return -1;
    }
    int found = -1;
    for (int i = 0; i < config->bNumInterfaces && found < 0; ++i) {
        const libusb_interface_descriptor &alt = config->interface[i].altsetting[0];
        if (alt.bInterfaceClass == kAudioClass &&
            alt.bInterfaceSubClass == kAudioControlSubclass) {
            found = alt.bInterfaceNumber;
        }
    }
    libusb_free_config_descriptor(config);
    return found;
}

bool device_is_uac2(libusb_device *device) {
    return audio_control_interface(device) >= 0;
}

void apply_volume_step(libusb_device_handle *handle,
                       FeatureVolume *volume,
                       int direction,
                       bool *mute_cleared) {
    if (handle == nullptr || volume == nullptr || direction == 0 ||
        volume->channel_count < 1) {
        return;
    }
    int16_t current = volume->current;
    if (!get_s16(handle, volume->ac_interface, volume->unit_id, kCsVolume,
                 volume->channels[0], &current)) {
        /* KA13 answers 0 dB after a control claim, and a failed read
         * used to drop the press. Write the level the buttons already
         * chose. */
        vol_note("volume GET_CUR failed, writing remembered level");
        current = volume->current;
    } else {
        volume->current = current;
    }
    const int32_t next32 = static_cast<int32_t>(current) +
            direction * static_cast<int32_t>(volume->step);
    int16_t next = static_cast<int16_t>(next32);
    if (next32 > volume->maximum) {
        next = volume->maximum;
    }
    if (next32 < volume->minimum) {
        next = volume->minimum;
    }
    if (next == current) {
        return;
    }
    if (volume->mute_channel >= 0 && mute_cleared != nullptr && !*mute_cleared) {
        uint8_t mute = 1;
        if (get_u8(handle, volume->ac_interface, volume->unit_id, kCsMute,
                   volume->mute_channel, &mute)) {
            if (mute == 0) {
                *mute_cleared = true;
            } else if (set_u8(handle, volume->ac_interface, volume->unit_id, kCsMute,
                              volume->mute_channel, 0)) {
                *mute_cleared = true;
                vol_note("hardware volume unmute");
            }
        }
    }
    bool wrote = true;
    for (int i = 0; i < volume->channel_count; ++i) {
        if (!set_s16(handle, volume->ac_interface, volume->unit_id, kCsVolume,
                     volume->channels[i], next)) {
            int16_t got = 0;
            if (!get_s16(handle, volume->ac_interface, volume->unit_id, kCsVolume,
                         volume->channels[i], &got) ||
                got != next) {
                wrote = false;
            }
        }
    }
    if (!wrote) {
        int16_t got = current;
        if (get_s16(handle, volume->ac_interface, volume->unit_id, kCsVolume,
                    volume->channels[0], &got)) {
            volume->current = got;
        }
        vol_note("hardware volume SET_CUR failed, device is at %d", volume->current);
        return;
    }
    volume->current = next;
    vol_note("hardware volume %d (%+.2f dB)",
             static_cast<int>(next),
             static_cast<double>(next) / 256.0);
}

bool token_ok(const std::string &text, std::size_t limit) {
    if (text.empty() || text.size() > limit) {
        return false;
    }
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20U || c == '\'' || c == '\\' || c == ';') {
            return false;
        }
    }
    return true;
}

bool card_id_ok(const std::string &text) {
    if (!token_ok(text, 32)) {
        return false;
    }
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

constexpr const char *kHwLevelPath = "/var/lib/ampdac.hwlevel";

void publish_level(int level) {
    if (level > 0) {
        level = 0;
    }
    if (level < -16128) {
        level = -16128;
    }
    std::ofstream out(kHwLevelPath, std::ios::trunc);
    if (!out) {
        return;
    }
    out << level << '\n';
    out.close();
    (void)chmod(kHwLevelPath, 0644);
}

int load_published_level(int fallback) {
    std::ifstream in(kHwLevelPath);
    int level = fallback;
    if (!(in >> level)) {
        return fallback;
    }
    if (level > 0) {
        level = 0;
    }
    if (level < -16128) {
        level = -16128;
    }
    return level;
}

/** Kernel card id and its playback mixer, while snd-usb-audio owns it. */
bool locate_kernel_mixer(uint16_t vid, uint16_t pid,
                         std::string *card_out, std::string *control_out) {
    if (vid == 0 || card_out == nullptr || control_out == nullptr) {
        return false;
    }
    char want[16];
    std::snprintf(want, sizeof(want), "%04x:%04x", vid, pid);
    std::string card;
    for (int n = 0; n < 16; ++n) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/asound/card%d/usbid", n);
        std::ifstream in(path);
        std::string line;
        if (!std::getline(in, line)) {
            continue;
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line != want) {
            continue;
        }
        std::snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", n);
        if (access(path, F_OK) != 0) {
            return false;
        }
        std::snprintf(path, sizeof(path), "/proc/asound/card%d/id", n);
        std::ifstream id_in(path);
        if (!std::getline(id_in, card)) {
            return false;
        }
        while (!card.empty() && (card.back() == '\r' || card.back() == ' ')) {
            card.pop_back();
        }
        break;
    }
    if (!card_id_ok(card)) {
        return false;
    }

    const std::string list_cmd = std::string("/usr/bin/amixer -c ") + card + " scontents";
    FILE *pipe = popen(list_cmd.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    std::string pending;
    bool pending_volume = false;
    std::string chosen;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        const std::string line(buf);
        const std::string prefix = "Simple mixer control '";
        if (line.compare(0, prefix.size(), prefix) == 0) {
            if (pending_volume && pending == "PCM") {
                chosen = pending;
                break;
            }
            if (pending_volume && chosen.empty()) {
                chosen = pending;
            }
            const auto end = line.find('\'', prefix.size());
            pending = end == std::string::npos
                    ? std::string()
                    : line.substr(prefix.size(), end - prefix.size());
            pending_volume = false;
            continue;
        }
        if (line.find("pvolume") != std::string::npos) {
            pending_volume = true;
        }
    }
    pclose(pipe);
    if (pending_volume && pending == "PCM") {
        chosen = pending;
    } else if (pending_volume && chosen.empty()) {
        chosen = pending;
    }
    if (!token_ok(chosen, 64)) {
        return false;
    }
    *card_out = card;
    *control_out = chosen;
    return true;
}

/** Absolute feature-unit level currently shown by the kernel mixer. */
bool read_kernel_level(uint16_t vid, uint16_t pid, int *level_out,
                       int *raw_out, int *max_out) {
    std::string card;
    std::string control;
    if (!locate_kernel_mixer(vid, pid, &card, &control)) {
        return false;
    }
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "/usr/bin/amixer -c %s sget '%s'",
                  card.c_str(), control.c_str());
    FILE *pipe = popen(cmd, "r");
    if (pipe == nullptr) {
        return false;
    }
    bool have_level = false;
    bool have_raw = false;
    int level = 0;
    int raw = 0;
    int max_raw = 63;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        const char *limits = std::strstr(buf, "Limits:");
        int lo = 0;
        int hi = 0;
        if (limits != nullptr &&
            std::sscanf(limits, "Limits: Playback %d - %d", &lo, &hi) == 2 &&
            hi >= lo) {
            max_raw = hi;
        }
        if (std::strstr(buf, "Playback") == nullptr) {
            continue;
        }
        const char *play = std::strstr(buf, "Playback ");
        int value = 0;
        if (play != nullptr && std::sscanf(play, "Playback %d", &value) == 1) {
            raw = value;
            have_raw = true;
        }
        const char *db = std::strstr(buf, "dB]");
        if (db == nullptr) {
            continue;
        }
        const char *bracket = db;
        while (bracket > buf && *bracket != '[') {
            --bracket;
        }
        double db_value = 0;
        if (*bracket == '[' && std::sscanf(bracket + 1, "%lf", &db_value) == 1) {
            const double scaled = db_value * 256.0;
            level = static_cast<int>(scaled + (scaled < 0 ? -0.5 : 0.5));
            have_level = true;
        }
    }
    pclose(pipe);
    if (!have_level && have_raw) {
        level = (raw - max_raw) * 256;
        have_level = true;
    }
    if (!have_level) {
        return false;
    }
    if (level_out != nullptr) {
        *level_out = level;
    }
    if (raw_out != nullptr) {
        *raw_out = raw;
    }
    if (max_out != nullptr) {
        *max_out = max_raw;
    }
    return true;
}

/** Put the shared level into the kernel mixer. raw max is 0 dB. */
bool write_kernel_level(uint16_t vid, uint16_t pid, int level) {
    std::string card;
    std::string control;
    if (!locate_kernel_mixer(vid, pid, &card, &control)) {
        return false;
    }
    int current = 0;
    int raw = 0;
    int max_raw = 63;
    const bool known = read_kernel_level(vid, pid, &current, &raw, &max_raw);
    if (!known || max_raw <= 0) {
        max_raw = 63;
    }
    int next = max_raw + level / 256;
    if (next < 0) {
        next = 0;
    }
    if (next > max_raw) {
        next = max_raw;
    }
    if (known && next == raw) {
        return true;
    }
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "/usr/bin/amixer -q -c %s set '%s' mute",
                  card.c_str(), control.c_str());
    (void)std::system(cmd);
    std::snprintf(cmd, sizeof(cmd), "/usr/bin/amixer -q -c %s set '%s' %d",
                  card.c_str(), control.c_str(), next);
    if (std::system(cmd) != 0) {
        return false;
    }
    std::snprintf(cmd, sizeof(cmd), "/usr/bin/amixer -q -c %s set '%s' unmute",
                  card.c_str(), control.c_str());
    (void)std::system(cmd);
    vol_note("kernel volume %d (%+.2f dB)",
             level, static_cast<double>(level) / 256.0);
    return true;
}

/** Step the kernel playback mixer of this USB device by one hardware tick. */
bool step_kernel_mixer(uint16_t vid, uint16_t pid, int direction) {
    if (vid == 0 || direction == 0) {
        return false;
    }
    std::string card;
    std::string control;
    if (!locate_kernel_mixer(vid, pid, &card, &control)) {
        return false;
    }
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "/usr/bin/amixer -q -c %s set '%s' %s",
                  card.c_str(), control.c_str(), direction > 0 ? "1+" : "1-");
    if (std::system(cmd) != 0) {
        return false;
    }
    vol_note("hardware volume via %s %s %s",
             card.c_str(), control.c_str(), direction > 0 ? "1+" : "1-");
    return true;
}

bool kernel_card_present(uint16_t vid, uint16_t pid) {
    if (vid == 0) {
        return false;
    }
    char want[16];
    std::snprintf(want, sizeof(want), "%04x:%04x", vid, pid);
    for (int n = 0; n < 16; ++n) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/asound/card%d/usbid", n);
        std::ifstream in(path);
        std::string line;
        if (!std::getline(in, line)) {
            continue;
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line != want) {
            continue;
        }
        std::snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", n);
        return access(path, F_OK) == 0;
    }
    return false;
}

/** One level on both drivers. A fresh kernel card comes up at 0 dB;
 *  write the shared level, and write it again if that 0 dB comes back
 *  during the switch. A card that was already up is read, not overwritten,
 *  unless it is sitting at full scale over a quieter saved level. */
void follow_kernel_card() {
    constexpr uint16_t kVid = 0x2972;
    constexpr uint16_t kPid = 0x0062;
    static bool primed = false;
    static bool up = false;
    static std::chrono::steady_clock::time_point guard_until{};

    const bool now = kernel_card_present(kVid, kPid);
    const auto tick = std::chrono::steady_clock::now();
    if (!primed) {
        primed = true;
        up = now;
        if (!now) {
            g_ka13_level.store(load_published_level(
                    g_ka13_level.load(std::memory_order_relaxed)),
                    std::memory_order_relaxed);
            return;
        }
        int live = 0;
        int raw = 0;
        int max_raw = 0;
        const int saved = load_published_level(
                g_ka13_level.load(std::memory_order_relaxed));
        if (read_kernel_level(kVid, kPid, &live, &raw, &max_raw) &&
            max_raw > 0 && raw >= max_raw && saved < 0) {
            g_ka13_level.store(saved, std::memory_order_relaxed);
            (void)write_kernel_level(kVid, kPid, saved);
            guard_until = tick + std::chrono::seconds(4);
            return;
        }
        if (read_kernel_level(kVid, kPid, &live, &raw, &max_raw)) {
            g_ka13_level.store(live, std::memory_order_relaxed);
            publish_level(live);
        }
        return;
    }
    if (now && !up) {
        const int level = load_published_level(
                g_ka13_level.load(std::memory_order_relaxed));
        g_ka13_level.store(level, std::memory_order_relaxed);
        (void)write_kernel_level(kVid, kPid, level);
        guard_until = tick + std::chrono::seconds(4);
    }
    if (!now) {
        guard_until = {};
    }
    up = now;
    if (now && tick < guard_until) {
        int live = 0;
        int raw = 0;
        int max_raw = 0;
        const int want = g_ka13_level.load(std::memory_order_relaxed);
        if (read_kernel_level(kVid, kPid, &live, &raw, &max_raw) &&
            max_raw > 0 && raw >= max_raw && want < 0) {
            (void)write_kernel_level(kVid, kPid, want);
        }
    }
}

struct VolumeService {
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::thread thread;

    libusb_device_handle *audio = nullptr;
    int audio_ac = -1;
    bool external_ac = false;

    libusb_context *usb = nullptr;
    libusb_device_handle *own = nullptr;
    HidCandidate hid{};
    FeatureVolume feature{};
    bool hid_ready = false;
    bool ac_ready = false;
    bool have_feature = false;
    bool have_memory = false;
    bool remembered_mute = false;
    uint16_t device_vid = 0;
    uint16_t device_pid = 0;
    std::string key_dev;
    std::string hid_dev;
    uint8_t hid_bits = 0;
    bool mute_cleared = false;
    bool prev_up = false;
    bool prev_down = false;
    bool up_repeating = false;
    bool down_repeating = false;
    uint8_t last_report = 0xFF;
    std::chrono::steady_clock::time_point last_up{};
    std::chrono::steady_clock::time_point last_down{};

    libusb_transfer *hid_xfer = nullptr;
    uint8_t hid_buf[64]{};
    int hid_completed = 0;
    int hid_status = 0;
    int hid_got = 0;
    bool hid_armed = false;
    bool hid_yielded = true;

    libusb_device_handle *session_handle = nullptr;
    libusb_transfer *session_xfer = nullptr;
    int session_iface = -1;
    bool session_claimed = false;
    bool session_hid_released = true;
    std::atomic<bool> session_hid_stop{false};
    std::atomic<bool> session_hid_idle{true};
    uint8_t session_buf[64]{};
    uint8_t pending_report[64]{};
    int pending_len = 0;
    bool report_pending = false;

    static void hid_transfer_done(libusb_transfer *xfer) {
        auto *self = static_cast<VolumeService *>(xfer->user_data);
        self->hid_status = xfer->status;
        self->hid_got = xfer->actual_length;
        self->hid_completed = 1;
    }

    bool arm_hid() {
        if (own == nullptr) {
            return false;
        }
        if (hid_xfer == nullptr) {
            hid_xfer = libusb_alloc_transfer(0);
            if (hid_xfer == nullptr) {
                return false;
            }
        }
        int packet = hid.packet_bytes;
        if (packet < 1 || packet > static_cast<int>(sizeof(hid_buf))) {
            packet = static_cast<int>(sizeof(hid_buf));
        }
        std::memset(hid_buf, 0, sizeof(hid_buf));
        hid_completed = 0;
        hid_status = 0;
        hid_got = 0;
        libusb_fill_interrupt_transfer(
                hid_xfer,
                own,
                hid.endpoint,
                hid_buf,
                packet,
                hid_transfer_done,
                this,
                kHidIdleMs);
        const int rc = libusb_submit_transfer(hid_xfer);
        if (rc != LIBUSB_SUCCESS) {
            vol_note("volume HID submit failed (%s)", libusb_error_name(rc));
            return false;
        }
        hid_armed = true;
        return true;
    }

    static void session_hid_done(libusb_transfer *xfer) {
        auto *self = static_cast<VolumeService *>(xfer->user_data);
        const bool stopping = self->session_hid_stop.load(std::memory_order_acquire);
        if (!stopping &&
            xfer->status == LIBUSB_TRANSFER_COMPLETED &&
            xfer->actual_length > 0) {
            const int n = xfer->actual_length > static_cast<int>(sizeof(self->pending_report))
                    ? static_cast<int>(sizeof(self->pending_report))
                    : xfer->actual_length;
            std::lock_guard<std::mutex> lock(self->mu);
            std::memcpy(self->pending_report, xfer->buffer, static_cast<std::size_t>(n));
            self->pending_len = n;
            self->report_pending = true;
            self->cv.notify_all();
        }
        if (stopping ||
            xfer->status == LIBUSB_TRANSFER_CANCELLED ||
            xfer->status == LIBUSB_TRANSFER_NO_DEVICE ||
            xfer->status == LIBUSB_TRANSFER_ERROR ||
            xfer->status == LIBUSB_TRANSFER_STALL) {
            self->session_hid_idle.store(true, std::memory_order_release);
            self->cv.notify_all();
            return;
        }
        if (libusb_submit_transfer(xfer) != LIBUSB_SUCCESS) {
            self->session_hid_idle.store(true, std::memory_order_release);
            self->cv.notify_all();
        }
    }

    /** Drop our own interrupt pipe and HID claim. Do not give the interface to usbhid. */
    void yield_hid_claim() {
        if (hid_armed && hid_xfer != nullptr && usb != nullptr && !hid_completed) {
            (void)libusb_cancel_transfer(hid_xfer);
            for (int i = 0; i < 40 && !hid_completed; ++i) {
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 20000;
                (void)libusb_handle_events_timeout_completed(usb, &tv, &hid_completed);
            }
        }
        hid_armed = false;
        if (own != nullptr && hid_ready && hid.interface_number >= 0) {
            (void)libusb_release_interface(own, hid.interface_number);
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            hid_yielded = true;
            session_hid_released = false;
            cv.notify_all();
        }
        vol_note("volume HID yielded to the playback handle");
    }

    bool claim_session_interface(libusb_device_handle *handle) {
        if (handle == nullptr || hid.endpoint == 0 || hid.interface_number < 0) {
            std::lock_guard<std::mutex> lock(mu);
            session_hid_released = true;
            cv.notify_all();
            return false;
        }
        (void)libusb_detach_kernel_driver(handle, hid.interface_number);
        const int rc = libusb_claim_interface(handle, hid.interface_number);
        if (rc != LIBUSB_SUCCESS) {
            vol_note("session HID claim failed (%s)", libusb_error_name(rc));
            std::lock_guard<std::mutex> lock(mu);
            session_hid_released = true;
            cv.notify_all();
            return false;
        }
        session_handle = handle;
        session_iface = hid.interface_number;
        session_claimed = true;
        vol_note("session HID iface %d claimed", hid.interface_number);
        return true;
    }

    void arm_session_hid(libusb_device_handle *handle) {
        if (!session_claimed || session_handle != handle || session_xfer != nullptr) {
            return;
        }
        session_xfer = libusb_alloc_transfer(0);
        if (session_xfer == nullptr) {
            return;
        }
        int packet = hid.packet_bytes;
        if (packet < 1 || packet > static_cast<int>(sizeof(session_buf))) {
            packet = static_cast<int>(sizeof(session_buf));
        }
        std::memset(session_buf, 0, sizeof(session_buf));
        session_hid_stop.store(false, std::memory_order_release);
        session_hid_idle.store(false, std::memory_order_release);
        libusb_fill_interrupt_transfer(
                session_xfer,
                handle,
                hid.endpoint,
                session_buf,
                packet,
                session_hid_done,
                this,
                kHidIdleMs);
        const int rc = libusb_submit_transfer(session_xfer);
        if (rc != LIBUSB_SUCCESS) {
            session_hid_idle.store(true, std::memory_order_release);
            libusb_free_transfer(session_xfer);
            session_xfer = nullptr;
            vol_note("session HID submit failed (%s)", libusb_error_name(rc));
            return;
        }
        vol_note("session HID armed ep 0x%02X", hid.endpoint);
    }

    void reclaim_hid() {
        if (own == nullptr || !hid_ready || hid.interface_number < 0) {
            hid_yielded = false;
            return;
        }
        (void)libusb_detach_kernel_driver(own, hid.interface_number);
        if (libusb_claim_interface(own, hid.interface_number) != LIBUSB_SUCCESS) {
            return;
        }
        hid_yielded = false;
        (void)arm_hid();
    }

    void retire_hid() {
        if (hid_armed && hid_xfer != nullptr && usb != nullptr && !hid_completed) {
            (void)libusb_cancel_transfer(hid_xfer);
            for (int i = 0; i < 40 && !hid_completed; ++i) {
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 50000;
                (void)libusb_handle_events_timeout_completed(usb, &tv, &hid_completed);
            }
        }
        /* libusb_close finishes a transfer that is still pending and runs
         * its callback before returning. */
        close_own();
        if (hid_xfer != nullptr) {
            libusb_free_transfer(hid_xfer);
            hid_xfer = nullptr;
        }
        hid_armed = false;
        hid_completed = 0;
    }

    void close_own() {
        int ac = -1;
        bool release_ac = false;
        bool release_hid = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            release_ac = ac_ready;
            ac = feature.ac_interface;
            ac_ready = false;
            release_hid = hid_ready;
            hid_ready = false;
            cv.notify_all();
        }
        if (own != nullptr && release_ac) {
            (void)libusb_release_interface(own, ac);
        }
        if (own != nullptr && release_hid && !hid_yielded) {
            (void)libusb_release_interface(own, hid.interface_number);
            (void)libusb_attach_kernel_driver(own, hid.interface_number);
        }
        if (own != nullptr) {
            libusb_close(own);
            own = nullptr;
        }
        if (usb != nullptr) {
            libusb_exit(usb);
            usb = nullptr;
        }
        have_feature = false;
        device_vid = 0;
        device_pid = 0;
        cv.notify_all();
    }

    bool open_own() {
        if (libusb_init(&usb) != LIBUSB_SUCCESS) {
            usb = nullptr;
            return false;
        }
        libusb_device **list = nullptr;
        const ssize_t count = libusb_get_device_list(usb, &list);
        libusb_device *chosen = nullptr;
        uint16_t want_vid = 0;
        uint16_t want_pid = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (audio != nullptr) {
                libusb_device_descriptor desc{};
                if (libusb_get_device_descriptor(libusb_get_device(audio), &desc) ==
                    LIBUSB_SUCCESS) {
                    want_vid = desc.idVendor;
                    want_pid = desc.idProduct;
                }
            }
        }
        if (count >= 0) {
            for (ssize_t i = 0; i < count; ++i) {
                if (!device_is_uac2(list[i])) {
                    continue;
                }
                libusb_device_descriptor desc{};
                if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS) {
                    continue;
                }
                if (want_vid != 0 &&
                    (desc.idVendor != want_vid || desc.idProduct != want_pid)) {
                    continue;
                }
                chosen = list[i];
                break;
            }
        }
        if (chosen == nullptr || libusb_open(chosen, &own) != LIBUSB_SUCCESS) {
            if (list != nullptr) {
                libusb_free_device_list(list, 1);
            }
            libusb_exit(usb);
            usb = nullptr;
            own = nullptr;
            return false;
        }
        libusb_free_device_list(list, 1);

        HidCandidate found;
        if (!find_hid_volume(own, &found)) {
            libusb_close(own);
            own = nullptr;
            libusb_exit(usb);
            usb = nullptr;
            return false;
        }
        hid = found;
        hid_ready = true;
        libusb_device_descriptor opened{};
        if (libusb_get_device_descriptor(libusb_get_device(own), &opened) ==
            LIBUSB_SUCCESS) {
            device_vid = opened.idVendor;
            device_pid = opened.idProduct;
        }
        (void)libusb_control_transfer(
                own,
                kHostToClassInterface,
                0x0AU,
                0,
                static_cast<uint16_t>(hid.interface_number),
                nullptr,
                0,
                kControlTimeoutMs);

        /* Claiming the audio-control interface detaches snd-usb-audio.
         * The kernel card then has no PCM and Volumio reports
         * "No such file or directory". The knob uses the playback handle
         * while libampdac is open, and the ALSA mixer while the kernel is. */
        const int ac = audio_control_interface(libusb_get_device(own));
        feature.ac_interface = ac;

        libusb_device_handle *probe = nullptr;
        int probe_ac = ac;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (ac_ready) {
                probe = own;
                probe_ac = feature.ac_interface;
            } else if (audio != nullptr) {
                probe = audio;
                probe_ac = audio_ac;
            }
        }
        if (probe != nullptr && probe_ac >= 0) {
            FeatureVolume parsed;
            if (find_feature_volume(probe, probe_ac, &parsed) &&
                load_range_and_current(probe, &parsed)) {
                feature = parsed;
                have_feature = true;
                vol_note("volume service HID iface %d ep 0x%02X unit %u "
                         "range %d..%d step %d current %d (%+.2f dB)",
                         hid.interface_number,
                         hid.endpoint,
                         feature.unit_id,
                         static_cast<int>(feature.minimum),
                         static_cast<int>(feature.maximum),
                         static_cast<int>(feature.step),
                         static_cast<int>(feature.current),
                         static_cast<double>(feature.current) / 256.0);
            }
        }
        if (!have_feature) {
            vol_note("volume service HID iface %d ep 0x%02X",
                     hid.interface_number,
                     hid.endpoint);
        }
        return true;
    }

    void release_ac_if_yielded() {
        std::lock_guard<std::mutex> lock(mu);
        if (external_ac && ac_ready && own != nullptr) {
            (void)libusb_release_interface(own, feature.ac_interface);
            ac_ready = false;
            cv.notify_all();
        }
    }

    void on_report(const uint8_t *buf, int length) {
        if (length > 0 && buf[0] != last_report) {
            vol_note("hid report %02x", buf[0]);
            last_report = buf[0];
        }
        if (hid.report.report_id != 0) {
            if (length < 1 || buf[0] != static_cast<uint8_t>(hid.report.report_id)) {
                return;
            }
        }
        bool up = bit_on(buf, length, hid.report.up_bit);
        bool down = bit_on(buf, length, hid.report.down_bit);
        if (up && down) {
            up = false;
            down = false;
        }
        const auto now = std::chrono::steady_clock::now();
        int direction = 0;
        if (up) {
            if (!prev_up) {
                direction = +1;
                last_up = now;
                up_repeating = false;
            } else {
                const int gap = up_repeating ? kHoldRepeatMs : kHoldDelayMs;
                if (now - last_up >= std::chrono::milliseconds(gap)) {
                    direction = +1;
                    last_up = now;
                    up_repeating = true;
                }
            }
        } else {
            up_repeating = false;
        }
        if (direction == 0 && down) {
            if (!prev_down) {
                direction = -1;
                last_down = now;
                down_repeating = false;
            } else {
                const int gap = down_repeating ? kHoldRepeatMs : kHoldDelayMs;
                if (now - last_down >= std::chrono::milliseconds(gap)) {
                    direction = -1;
                    last_down = now;
                    down_repeating = true;
                }
            }
        } else if (!down) {
            down_repeating = false;
        }
        prev_up = up;
        prev_down = down;

        if (direction == 0 && hid.report.relative_bit >= 0 && hid.report.relative_bits > 0) {
            const int bit = hid.report.relative_bit;
            const int bits = hid.report.relative_bits;
            if ((bit + bits) <= length * 8) {
                uint32_t raw = 0;
                for (int b = 0; b < bits; ++b) {
                    if (bit_on(buf, length, bit + b)) {
                        raw |= 1U << b;
                    }
                }
                const int32_t delta = sign_extend(raw, bits);
                if (delta > 0) {
                    direction = +1;
                } else if (delta < 0) {
                    direction = -1;
                }
            }
        }
        if (direction == 0) {
            return;
        }
        apply_direction(direction);
    }

    /* A replaced /dev node keeps the old file descriptor open and silent.
     * stat() failing means the path we opened is gone; that must reopen
     * too, otherwise the next press is discarded. */
    static bool node_is_stale(int fd, const std::string &path) {
        if (fd < 0) {
            return false;
        }
        struct stat held {};
        if (::fstat(fd, &held) != 0 || held.st_nlink == 0) {
            return true;
        }
        if (path.empty()) {
            return true;
        }
        struct stat live {};
        if (::stat(path.c_str(), &live) != 0) {
            return true;
        }
        return live.st_ino != held.st_ino;
    }

    void apply_direction(int direction) {
        if (direction == 0) {
            return;
        }
        uint16_t vid = 0;
        uint16_t pid = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            vid = device_vid;
            pid = device_pid;
            /* While libusb owns the DAC the kernel mixer is not the
             * signal path. A press has to move the feature unit. */
            if (audio != nullptr) {
                if (!have_feature) {
                    FeatureVolume parsed;
                    if (!find_feature_volume(audio, audio_ac, &parsed) ||
                        !load_range_and_current(audio, &parsed)) {
                        vol_note("volume button ignored, feature unit unread");
                        return;
                    }
                    feature = parsed;
                    feature.ac_interface = audio_ac;
                    have_feature = true;
                }
                feature.ac_interface = audio_ac;
                apply_volume_step(audio, &feature, direction, &mute_cleared);
                note_feature_level(feature);
                return;
            }
        }
        if (step_kernel_mixer(vid, pid, direction)) {
            int level = 0;
            int raw = 0;
            int max_raw = 0;
            if (read_kernel_level(vid, pid, &level, &raw, &max_raw)) {
                if (level > 0) level = 0;
                if (level < -16128) level = -16128;
                g_ka13_level.store(level, std::memory_order_relaxed);
                publish_level(level);
            }
        }
    }

    void note_feature_level(const FeatureVolume &feature) {
        g_ka13_level.store(feature.current, std::memory_order_relaxed);
        publish_level(feature.current);
    }

    int open_ka13_keys() {
        int chosen = -1;
        for (int n = 0; n < 32; ++n) {
            char path[80];
            std::snprintf(path, sizeof(path),
                          "/sys/class/input/event%d/device/id/vendor", n);
            std::ifstream vendor_in(path);
            std::string vendor;
            if (!std::getline(vendor_in, vendor) || vendor != "2972") {
                continue;
            }
            std::snprintf(path, sizeof(path),
                          "/sys/class/input/event%d/device/id/product", n);
            std::ifstream product_in(path);
            std::string product;
            if (!std::getline(product_in, product) || product != "0062") {
                continue;
            }
            std::snprintf(path, sizeof(path), "/dev/input/event%d", n);
            const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0) {
                continue;
            }
            if (chosen >= 0) {
                ::close(chosen);
            }
            chosen = fd;
            key_dev = path;
            std::snprintf(path, sizeof(path),
                          "/sys/class/input/event%d/device/name", n);
            std::ifstream name_in(path);
            std::string name;
            std::getline(name_in, name);
            if (name.find("Consumer") != std::string::npos) {
                break;
            }
        }
        if (chosen >= 0) {
            std::lock_guard<std::mutex> lock(mu);
            device_vid = 0x2972;
            device_pid = 0x0062;
        }
        return chosen;
    }

    /* usbhid publishes the same interrupt reports here. The input node
     * stays silent when the kernel does not map the report onto a key. */
    int open_ka13_hidraw() {
        for (int n = 0; n < 16; ++n) {
            char path[80];
            std::snprintf(path, sizeof(path), "/sys/class/hidraw/hidraw%d/device", n);
            char link[256];
            const ssize_t got = ::readlink(path, link, sizeof(link) - 1);
            if (got <= 0) {
                continue;
            }
            link[got] = '\0';
            if (std::strstr(link, "2972:0062") == nullptr) {
                continue;
            }
            std::snprintf(path, sizeof(path), "/dev/hidraw%d", n);
            const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if (fd >= 0) {
                hid_dev = path;
                vol_note("KA13 volume keys follow %s", path);
                return fd;
            }
        }
        return -1;
    }

    void on_hid_report(const uint8_t *buf, int length) {
        if (buf == nullptr || length < 1) {
            return;
        }
        uint8_t bits = buf[0];
        if ((bits & 0x06U) == 0 && length > 1 && (buf[1] & 0x06U) != 0) {
            bits = buf[1];
        }
        const uint8_t changed = static_cast<uint8_t>(bits ^ hid_bits);
        hid_bits = bits;
        if ((changed & 0x02U) != 0 && (bits & 0x02U) != 0) {
            vol_note("hid volume up");
            apply_direction(+1);
        }
        if ((changed & 0x04U) != 0 && (bits & 0x04U) != 0) {
            vol_note("hid volume down");
            apply_direction(-1);
        }
    }

    void run() {
        /* KA13 volume buttons are ordinary consumer keys. usbhid owns
         * the HID interface; claiming it from here detaches that driver
         * and stalls isochronous feedback on this host. KA17, K7 and the
         * SMSL apply their own knobs and are left alone. */
        int fd = -1;
        int hid = -1;
        while (!stop.load(std::memory_order_acquire)) {
            follow_kernel_card();
            release_ac_if_yielded();
            if (node_is_stale(fd, key_dev) || node_is_stale(hid, hid_dev)) {
                vol_note("KA13 volume node replaced, reopening");
                if (fd >= 0) {
                    ::close(fd);
                }
                if (hid >= 0) {
                    ::close(hid);
                }
                fd = -1;
                hid = -1;
                hid_bits = 0;
                key_dev.clear();
                hid_dev.clear();
            }
            if (fd < 0) {
                fd = open_ka13_keys();
                if (fd >= 0) {
                    vol_note("KA13 volume keys follow the kernel input device");
                }
            }
            if (hid < 0) {
                hid = open_ka13_hidraw();
            }
            if (fd < 0 && hid < 0) {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait_for(lock, std::chrono::milliseconds(500),
                            [&] { return stop.load(); });
                continue;
            }
            struct pollfd pfd[2];
            nfds_t np = 0;
            int ev_slot = -1;
            int hid_slot = -1;
            if (fd >= 0) {
                ev_slot = static_cast<int>(np);
                pfd[np].fd = fd;
                pfd[np].events = POLLIN;
                pfd[np].revents = 0;
                ++np;
            }
            if (hid >= 0) {
                hid_slot = static_cast<int>(np);
                pfd[np].fd = hid;
                pfd[np].events = POLLIN;
                pfd[np].revents = 0;
                ++np;
            }
            const int pr = ::poll(pfd, np, 200);
            if (stop.load(std::memory_order_acquire)) {
                break;
            }
            if (pr < 0) {
                if (fd >= 0) ::close(fd);
                if (hid >= 0) ::close(hid);
                fd = -1;
                hid = -1;
                continue;
            }
            if (ev_slot >= 0 &&
                (pfd[ev_slot].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                ::close(fd);
                fd = -1;
            }
            if (hid_slot >= 0 &&
                (pfd[hid_slot].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                ::close(hid);
                hid = -1;
                hid_bits = 0;
            }
            if (pr == 0) {
                continue;
            }
            if (ev_slot >= 0 && (pfd[ev_slot].revents & POLLIN) != 0) {
                struct input_event ev;
                while (::read(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
                    if (ev.type != EV_KEY || ev.value == 0) {
                        continue;
                    }
                    vol_note("key %u value %d", ev.code, ev.value);
                    /* The same press is also the hidraw report. Applying
                     * both moves the level by two steps. */
                    if (hid >= 0 &&
                        (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN)) {
                        continue;
                    }
                    if (ev.code == KEY_VOLUMEUP) {
                        apply_direction(+1);
                    } else if (ev.code == KEY_VOLUMEDOWN) {
                        apply_direction(-1);
                    }
                }
            }
            if (hid_slot >= 0 && (pfd[hid_slot].revents & POLLIN) != 0) {
                uint8_t buf[64];
                const ssize_t n = ::read(hid, buf, sizeof(buf));
                if (n > 0) {
                    if (buf[0] != hid_bits) {
                        vol_note("hid raw %d %02x", static_cast<int>(n), buf[0]);
                    }
                    on_hid_report(buf, static_cast<int>(n));
                }
            }
        }
        if (fd >= 0) {
            ::close(fd);
        }
        if (hid >= 0) {
            ::close(hid);
        }
    }

};

VolumeService *g_service = nullptr;

}  // namespace

void uac2_volume_service_start() {
    if (g_service != nullptr) {
        return;
    }
    g_ka13_level.store(
            load_published_level(g_ka13_level.load(std::memory_order_relaxed)),
            std::memory_order_relaxed);
    auto *service = new (std::nothrow) VolumeService();
    if (service == nullptr) {
        return;
    }
    g_service = service;
    try {
        service->thread = std::thread([service] { service->run(); });
    } catch (...) {
        g_service = nullptr;
        delete service;
    }
}

void uac2_volume_service_stop() noexcept {
    VolumeService *service = g_service;
    if (service == nullptr) {
        return;
    }
    g_service = nullptr;
    service->stop.store(true, std::memory_order_release);
    service->cv.notify_all();
    if (service->thread.joinable()) {
        service->thread.join();
    }
    delete service;
}

void uac2_volume_service_yield_ac() {
    VolumeService *service = g_service;
    if (service == nullptr) {
        return;
    }
    std::unique_lock<std::mutex> lock(service->mu);
    service->external_ac = true;
    service->cv.notify_all();
    service->cv.wait_for(lock, std::chrono::milliseconds(800), [&] {
        return !service->ac_ready || service->stop.load();
    });
}

void uac2_volume_service_attach(libusb_device_handle *handle, int ac_interface) {
    VolumeService *service = g_service;
    if (service == nullptr || handle == nullptr) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(service->mu);
        service->audio = handle;
        service->audio_ac = ac_interface;
        service->external_ac = true;
        service->session_hid_released = false;
        if (service->have_feature) {
            service->feature.ac_interface = ac_interface;
        }
        service->cv.notify_all();
        service->cv.wait_for(lock, std::chrono::milliseconds(1500), [&] {
            return service->hid_yielded || service->stop.load();
        });
    }
    (void)service->claim_session_interface(handle);
    if (service->have_feature || ac_interface < 0) return;
    FeatureVolume parsed;
    if (!find_feature_volume(handle, ac_interface, &parsed) ||
        !load_range_and_current(handle, &parsed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(service->mu);
    if (service->audio != handle) return;
    service->feature = parsed;
    service->feature.ac_interface = ac_interface;
    service->have_feature = true;
}

void uac2_volume_service_arm_session_hid(libusb_device_handle *handle) {
    VolumeService *service = g_service;
    if (service == nullptr || handle == nullptr) {
        return;
    }
    service->arm_session_hid(handle);
}

void uac2_volume_service_detach() noexcept {
    VolumeService *service = g_service;
    if (service == nullptr) {
        return;
    }
    libusb_transfer *xfer = nullptr;
    libusb_device_handle *handle = nullptr;
    int iface = -1;
    bool claimed = false;
    {
        std::lock_guard<std::mutex> lock(service->mu);
        service->audio = nullptr;
        service->audio_ac = -1;
        service->session_hid_stop.store(true, std::memory_order_release);
        xfer = service->session_xfer;
        handle = service->session_handle;
        iface = service->session_iface;
        claimed = service->session_claimed;
        service->cv.notify_all();
    }
    if (xfer != nullptr && !service->session_hid_idle.load(std::memory_order_acquire)) {
        (void)libusb_cancel_transfer(xfer);
        std::unique_lock<std::mutex> lock(service->mu);
        service->cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return service->session_hid_idle.load(std::memory_order_acquire) ||
                   service->stop.load();
        });
    }
    if (claimed && handle != nullptr && iface >= 0) {
        (void)libusb_release_interface(handle, iface);
    }
    {
        std::lock_guard<std::mutex> lock(service->mu);
        if (service->session_xfer != nullptr &&
            service->session_hid_idle.load(std::memory_order_acquire)) {
            libusb_free_transfer(service->session_xfer);
            service->session_xfer = nullptr;
        }
        service->session_handle = nullptr;
        service->session_iface = -1;
        service->session_claimed = false;
        service->session_hid_released = true;
        service->cv.notify_all();
    }
}

void uac2_volume_service_ac_free() noexcept {
    VolumeService *service = g_service;
    if (service == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(service->mu);
    service->audio = nullptr;
    service->audio_ac = -1;
    service->external_ac = false;
    service->cv.notify_all();
}

void uac2_volume_service_remember(libusb_device_handle *handle, int ac_interface) {
    VolumeService *service = g_service;
    if (service == nullptr || handle == nullptr || ac_interface < 0) {
        return;
    }
    FeatureVolume parsed;
    if (!find_feature_volume(handle, ac_interface, &parsed) ||
        !load_range_and_current(handle, &parsed)) {
        vol_note("volume remember failed");
        return;
    }
    bool mute_on = false;
    if (parsed.mute_channel >= 0) {
        uint8_t mute = 0;
        if (get_u8(handle, ac_interface, parsed.unit_id, kCsMute,
                   parsed.mute_channel, &mute)) {
            mute_on = mute != 0;
        }
    }
    std::lock_guard<std::mutex> lock(service->mu);
    service->feature = parsed;
    service->feature.ac_interface = ac_interface;
    service->have_feature = true;
    service->have_memory = true;
    service->remembered_mute = mute_on;
    service->mute_cleared = !mute_on;
    vol_note("volume remember %d (%+.2f dB) range %d..%d step %d mute %s",
             static_cast<int>(parsed.current),
             static_cast<double>(parsed.current) / 256.0,
             static_cast<int>(parsed.minimum),
             static_cast<int>(parsed.maximum),
             static_cast<int>(parsed.step),
             mute_on ? "on" : "off");
}

void uac2_volume_service_lock_full(libusb_device_handle *handle) {
    if (handle == nullptr) {
        return;
    }
    const int ac = audio_control_interface(libusb_get_device(handle));
    if (ac < 0) {
        return;
    }
    FeatureVolume parsed;
    if (!find_feature_volume(handle, ac, &parsed) ||
        !load_range_and_current(handle, &parsed)) {
        vol_note("volume full-scale unread");
        return;
    }
    if (parsed.mute_channel >= 0) {
        (void)set_u8(handle, ac, parsed.unit_id, kCsMute, parsed.mute_channel, 0);
    }
    int level = load_published_level(g_ka13_level.load(std::memory_order_relaxed));
    if (level > parsed.maximum) level = parsed.maximum;
    if (level < parsed.minimum) level = parsed.minimum;
    g_ka13_level.store(level, std::memory_order_relaxed);
    publish_level(level);
    bool wrote = parsed.channel_count > 0;
    for (int i = 0; i < parsed.channel_count; ++i) {
        if (!set_s16(handle, ac, parsed.unit_id, kCsVolume,
                     parsed.channels[i], static_cast<int16_t>(level))) {
            wrote = false;
        }
    }
    vol_note("volume set to %d (%+.2f dB) %s",
             level,
             static_cast<double>(level) / 256.0,
             wrote ? "ok" : "failed");
}

void uac2_volume_service_restore(libusb_device_handle *handle) {
    VolumeService *service = g_service;
    if (service == nullptr || handle == nullptr) {
        return;
    }
    FeatureVolume feature;
    bool mute_on = false;
    {
        std::lock_guard<std::mutex> lock(service->mu);
        if (!service->have_memory) {
            return;
        }
        feature = service->feature;
        mute_on = service->remembered_mute;
    }
    if (feature.mute_channel >= 0 && !mute_on) {
        if (set_u8(handle, feature.ac_interface, feature.unit_id, kCsMute,
                   feature.mute_channel, 0)) {
            vol_note("volume unmute");
        }
    }
    bool wrote = feature.channel_count > 0;
    for (int i = 0; i < feature.channel_count; ++i) {
        if (!set_s16(handle, feature.ac_interface, feature.unit_id, kCsVolume,
                     feature.channels[i], feature.current)) {
            wrote = false;
        }
    }
    vol_note("volume restore %d (%+.2f dB) %s",
             static_cast<int>(feature.current),
             static_cast<double>(feature.current) / 256.0,
             wrote ? "ok" : "failed");
    if (feature.mute_channel >= 0) {
        uint8_t mute = 0;
        if (get_u8(handle, feature.ac_interface, feature.unit_id, kCsMute,
                   feature.mute_channel, &mute)) {
            vol_note("volume readback mute %u", mute);
        } else {
            vol_note("volume readback mute failed");
        }
    }
    for (int i = 0; i < feature.channel_count; ++i) {
        int16_t got = 0;
        if (get_s16(handle, feature.ac_interface, feature.unit_id, kCsVolume,
                    feature.channels[i], &got)) {
            vol_note("volume readback ch %u = %d (%+.2f dB)",
                     feature.channels[i],
                     static_cast<int>(got),
                     static_cast<double>(got) / 256.0);
        } else {
            vol_note("volume readback ch %u failed", feature.channels[i]);
        }
    }
}

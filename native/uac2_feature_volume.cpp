#include "uac2_feature_volume.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

#include "libusb/libusb.h"

static constexpr const char *VOL_TAG = "UsbAudioBridge";

#define VLOGD(...) __android_log_print(ANDROID_LOG_DEBUG, VOL_TAG, __VA_ARGS__)
#define VLOGI(...) __android_log_print(ANDROID_LOG_INFO,  VOL_TAG, __VA_ARGS__)
#define VLOGW(...) __android_log_print(ANDROID_LOG_WARN,  VOL_TAG, __VA_ARGS__)

namespace {

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
constexpr int kInterruptTimeoutMs = 200;

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
            (void)libusb_set_auto_detach_kernel_driver(handle, 1);
            if (libusb_claim_interface(handle, alt.bInterfaceNumber) == LIBUSB_SUCCESS) {
                temporary_claim = true;
                n = read_hid_report_descriptor(
                        handle, alt.bInterfaceNumber, desc, sizeof(desc));
            }
        }
        Uac2HidVolumeReport report;
        const bool useful = n > 0 &&
                uac2_hid_parse_consumer_volume(desc, static_cast<std::size_t>(n), &report);
        if (!useful) {
            if (temporary_claim) {
                (void)libusb_release_interface(handle, alt.bInterfaceNumber);
            }
            continue;
        }
        if (!temporary_claim) {
            (void)libusb_set_auto_detach_kernel_driver(handle, 1);
            if (libusb_claim_interface(handle, alt.bInterfaceNumber) != LIBUSB_SUCCESS) {
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
    int16_t current = 0;
    if (!get_s16(handle, volume->ac_interface, volume->unit_id, kCsVolume,
                 volume->channels[0], &current)) {
        vol_note("volume GET_CUR failed");
        return;
    }
    volume->current = current;
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
    bool mute_cleared = false;
    bool prev_up = false;
    bool prev_down = false;
    bool up_repeating = false;
    bool down_repeating = false;
    uint8_t last_report = 0xFF;
    std::chrono::steady_clock::time_point last_up{};
    std::chrono::steady_clock::time_point last_down{};

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
        if (own != nullptr && release_hid) {
            (void)libusb_release_interface(own, hid.interface_number);
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
        (void)libusb_set_auto_detach_kernel_driver(own, 1);

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
        (void)libusb_control_transfer(
                own,
                kHostToClassInterface,
                0x0AU,
                0,
                static_cast<uint16_t>(hid.interface_number),
                nullptr,
                0,
                kControlTimeoutMs);

        const int ac = audio_control_interface(libusb_get_device(own));
        feature.ac_interface = ac;
        bool want_ac = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            want_ac = !external_ac && ac >= 0;
        }
        if (want_ac) {
            const int rc = libusb_claim_interface(own, ac);
            std::lock_guard<std::mutex> lock(mu);
            if (rc == LIBUSB_SUCCESS && external_ac) {
                (void)libusb_release_interface(own, ac);
                ac_ready = false;
                cv.notify_all();
            } else if (rc == LIBUSB_SUCCESS) {
                ac_ready = true;
            } else {
                vol_note("volume service AC claim failed (%s)", libusb_error_name(rc));
            }
        }

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
        if (direction == 0 || !have_feature) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu);
        libusb_device_handle *handle = nullptr;
        if (audio != nullptr) {
            handle = audio;
            feature.ac_interface = audio_ac;
        } else if (ac_ready && own != nullptr) {
            handle = own;
        }
        if (handle != nullptr) {
            apply_volume_step(handle, &feature, direction, &mute_cleared);
        }
    }

    void run() {
        while (!stop.load(std::memory_order_acquire)) {
            release_ac_if_yielded();
            if (!hid_ready) {
                if (!open_own()) {
                    std::unique_lock<std::mutex> lock(mu);
                    cv.wait_for(lock, std::chrono::milliseconds(500),
                                [&] { return stop.load(); });
                    continue;
                }
            }
            uint8_t buf[64];
            int transferred = 0;
            const int rc = libusb_interrupt_transfer(
                    own,
                    hid.endpoint,
                    buf,
                    hid.packet_bytes,
                    &transferred,
                    kInterruptTimeoutMs);
            if (stop.load(std::memory_order_acquire)) {
                break;
            }
            if (rc == LIBUSB_ERROR_TIMEOUT) {
                continue;
            }
            if (rc == LIBUSB_ERROR_PIPE) {
                (void)libusb_clear_halt(own, hid.endpoint);
                continue;
            }
            if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO ||
                rc == LIBUSB_ERROR_INTERRUPTED) {
                vol_note("volume service HID closed (%s)", libusb_error_name(rc));
                close_own();
                continue;
            }
            if (rc < 0) {
                usleep(20000);
                continue;
            }
            if (transferred > 0) {
                on_report(buf, transferred);
            }
        }
        close_own();
    }
};

VolumeService *g_service = nullptr;

}  // namespace

void uac2_volume_service_start() {
    if (g_service != nullptr) {
        return;
    }
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
    std::lock_guard<std::mutex> lock(service->mu);
    service->audio = handle;
    service->audio_ac = ac_interface;
    service->external_ac = true;
    if (service->have_feature) {
        service->feature.ac_interface = ac_interface;
    }
    service->cv.notify_all();
}

void uac2_volume_service_detach() noexcept {
    VolumeService *service = g_service;
    if (service == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(service->mu);
    service->audio = nullptr;
    service->audio_ac = -1;
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

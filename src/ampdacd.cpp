/**
 * ampdacd — one UAC2 DAC, one client.
 *
 * systemd starts this with CAP_SYS_NICE and CAP_SYS_ADMIN so the libusb
 * event thread can take SCHED_FIFO and the kernel audio driver can be
 * unbound. Players connect to the unix socket and never touch /dev/bus/usb.
 */

#include "ampdac_ipc.h"

void uac2_volume_service_start();
void uac2_volume_service_stop() noexcept;

#include <dirent.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
int g_wake_write = -1;

std::mutex g_fds_mu;
std::vector<int> g_live_fds;
std::atomic<int> g_owner{-1};

std::mutex g_pub_mu;
ampdac_session *g_pub_session = nullptr;
bool g_pub_started = false;
uint8_t g_pub_source_bits = 0;

void publish_session(ampdac_session *session, bool started) {
    std::lock_guard<std::mutex> lock(g_pub_mu);
    g_pub_session = session;
    g_pub_started = started;
}

void note_source_bits(uint8_t bits) {
    std::lock_guard<std::mutex> lock(g_pub_mu);
    g_pub_source_bits = bits;
}

void unpublish_session(ampdac_session *session) {
    std::lock_guard<std::mutex> lock(g_pub_mu);
    if (g_pub_session == session) {
        g_pub_session = nullptr;
        g_pub_started = false;
        g_pub_source_bits = 0;
    }
}

const char *dsd_name(uint32_t hz) {
    switch (hz) {
        case 2822400u: return "DSD64";
        case 5644800u: return "DSD128";
        case 11289600u: return "DSD256";
        case 22579200u: return "DSD512";
        default: return nullptr;
    }
}

std::mutex g_dac_mu;
std::string g_dac_header;
bool g_dac_ok = false;

struct DacId {
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint8_t bus = 0;
    uint8_t address = 0;
    bool known = false;
};

DacId g_dac_id;

bool same_dac(const DacId &a, const DacId &b) {
    return a.known && b.known && a.vid == b.vid && a.pid == b.pid &&
            a.bus == b.bus && a.address == b.address;
}

bool read_file_u32(const char *path, int base, uint32_t *out) {
    FILE *file = std::fopen(path, "r");
    if (file == nullptr) return false;
    unsigned long value = 0;
    const char *fmt = base == 16 ? "%lx" : "%lu";
    const bool ok = std::fscanf(file, fmt, &value) == 1;
    std::fclose(file);
    if (!ok) return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

bool descriptors_are_uac2(const std::string &dir) {
    const std::string path = dir + "/descriptors";
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    unsigned char blob[4096];
    const size_t n = std::fread(blob, 1, sizeof(blob), file);
    std::fclose(file);
    for (size_t i = 0; i + 9 <= n;) {
        const unsigned length = blob[i];
        if (length < 2 || i + length > n) break;
        if (blob[i + 1] == 0x04 && length >= 9 &&
            blob[i + 5] == 0x01 && blob[i + 6] == 0x02 && blob[i + 7] == 0x20) {
            return true;
        }
        i += length;
    }
    return false;
}

std::string format_caps(const ampdac_capabilities &caps);

/** First UAC2 device on the bus. Identity only: no control transfer. */
DacId uac2_on_bus() {
    DacId id;
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (dir == nullptr) return id;
    while (dirent *ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        const std::string path = std::string("/sys/bus/usb/devices/") + ent->d_name;
        if (!descriptors_are_uac2(path)) continue;
        uint32_t vid = 0;
        uint32_t pid = 0;
        uint32_t bus = 0;
        uint32_t address = 0;
        if (!read_file_u32((path + "/idVendor").c_str(), 16, &vid) ||
            !read_file_u32((path + "/idProduct").c_str(), 16, &pid) ||
            !read_file_u32((path + "/busnum").c_str(), 10, &bus) ||
            !read_file_u32((path + "/devnum").c_str(), 10, &address)) {
            continue;
        }
        id.vid = static_cast<uint16_t>(vid);
        id.pid = static_cast<uint16_t>(pid);
        id.bus = static_cast<uint8_t>(bus);
        id.address = static_cast<uint8_t>(address);
        id.known = true;
        break;
    }
    closedir(dir);
    return id;
}

void remember_caps(const ampdac_capabilities &caps) {
    g_dac_header = format_caps(caps);
    g_dac_ok = true;
    g_dac_id.vid = caps.device.vid;
    g_dac_id.pid = caps.device.pid;
    g_dac_id.bus = caps.device.bus;
    g_dac_id.address = caps.device.address;
    g_dac_id.known = true;
}

void read_sysfs_text(const std::string &path, char *dst, std::size_t dst_size) {
    if (dst == nullptr || dst_size == 0) return;
    dst[0] = '\0';
    FILE *file = std::fopen(path.c_str(), "r");
    if (file == nullptr) return;
    if (std::fgets(dst, static_cast<int>(dst_size), file) == nullptr) {
        dst[0] = '\0';
    }
    std::fclose(file);
    std::size_t len = std::strlen(dst);
    while (len > 0 && (dst[len - 1] == '\n' || dst[len - 1] == '\r')) {
        dst[--len] = '\0';
    }
}

/** Name, USB id and alternate settings from the cached descriptor.
 *  No libusb claim, so a playing DAC keeps its volume. */
bool caps_from_sysfs(const DacId &id, ampdac_capabilities *caps) {
    if (caps == nullptr || !id.known) return false;
    std::memset(caps, 0, sizeof(*caps));
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (dir == nullptr) return false;
    std::string devdir;
    while (dirent *ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        const std::string path = std::string("/sys/bus/usb/devices/") + ent->d_name;
        if (!descriptors_are_uac2(path)) continue;
        uint32_t vid = 0;
        uint32_t pid = 0;
        uint32_t bus = 0;
        uint32_t address = 0;
        if (!read_file_u32((path + "/idVendor").c_str(), 16, &vid) ||
            !read_file_u32((path + "/idProduct").c_str(), 16, &pid) ||
            !read_file_u32((path + "/busnum").c_str(), 10, &bus) ||
            !read_file_u32((path + "/devnum").c_str(), 10, &address)) {
            continue;
        }
        if (static_cast<uint16_t>(vid) != id.vid ||
            static_cast<uint16_t>(pid) != id.pid ||
            static_cast<uint8_t>(bus) != id.bus ||
            static_cast<uint8_t>(address) != id.address) {
            continue;
        }
        devdir = path;
        break;
    }
    closedir(dir);
    if (devdir.empty()) return false;

    caps->device.vid = id.vid;
    caps->device.pid = id.pid;
    caps->device.bus = id.bus;
    caps->device.address = id.address;
    read_sysfs_text(devdir + "/manufacturer", caps->device.manufacturer,
                    sizeof(caps->device.manufacturer));
    read_sysfs_text(devdir + "/product", caps->device.product,
                    sizeof(caps->device.product));
    read_sysfs_text(devdir + "/serial", caps->device.serial,
                    sizeof(caps->device.serial));

    FILE *desc = std::fopen((devdir + "/descriptors").c_str(), "rb");
    if (desc == nullptr) return true;
    std::vector<unsigned char> blob(16384);
    const std::size_t n = std::fread(blob.data(), 1, blob.size(), desc);
    std::fclose(desc);

    struct Alt {
        bool live = false;
        uint8_t iface = 0;
        uint8_t alt = 0;
        uint8_t channels = 0;
        uint8_t format_type = 0;
        uint32_t bm_formats = 0;
        uint8_t subslot = 0;
        uint8_t bits = 0;
        bool type_iv = false;
        uint16_t mps = 0;
        uint8_t sync = 0;
        uint8_t feedback_ep = 0;
        bool implicit = false;
    } cur;

    auto finish = [&]() {
        if (!cur.live || cur.mps == 0 || caps->alt_count >= AMPDAC_MAX_ALTS) {
            cur = Alt{};
            return;
        }
        const bool native = cur.type_iv || cur.format_type == 4 ||
                (cur.format_type == 1 &&
                 ((cur.bm_formats & 0x80000000u) != 0 || cur.bm_formats == 0x1Fu));
        for (int seen = 0; seen < caps->alt_count; ++seen) {
            if (caps->alts[seen].interface_number == cur.iface &&
                caps->alts[seen].alt_setting == cur.alt) {
                cur = Alt{};
                return;
            }
        }
        ampdac_alt_info &slot = caps->alts[caps->alt_count];
        slot.interface_number = cur.iface;
        slot.alt_setting = cur.alt;
        slot.channels = cur.channels;
        slot.subslot_bytes = native ? 0 : cur.subslot;
        slot.valid_bits = native ? 0 : cur.bits;
        slot.native_dsd = native ? 1U : 0U;
        unsigned base = cur.mps & 0x07FFu;
        unsigned mult = (cur.mps & 0x1800u) >> 11;
        if (mult > 2) mult = 2;
        slot.bytes_per_uframe = static_cast<uint16_t>(base * (mult + 1));
        caps->alt_count++;
        if (cur.channels > caps->channels_max) caps->channels_max = cur.channels;
        if (!native && cur.bits > 0 && caps->bit_depth_count < AMPDAC_MAX_DEPTHS) {
            bool seen = false;
            for (int i = 0; i < caps->bit_depth_count; ++i) {
                if (caps->bit_depths[i] == cur.bits) seen = true;
            }
            if (!seen) caps->bit_depths[caps->bit_depth_count++] = cur.bits;
        }
        if (native && caps->native_dsd == 0) {
            caps->native_dsd = 1;
            caps->native_dsd_interface = cur.iface;
            caps->native_dsd_alt = cur.alt;
        }
        if (!native && caps->dop == 0 && (cur.bits >= 24 || cur.subslot >= 3)) {
            caps->dop = 1;
            caps->dop_pcm_alt = cur.alt;
        }
        if (caps->sync == AMPDAC_SYNC_NONE && cur.sync != 0) {
            caps->sync = static_cast<ampdac_sync_mode>(cur.sync);
        }
        if (cur.feedback_ep != 0 && caps->feedback_endpoint == 0) {
            caps->feedback = AMPDAC_FEEDBACK_EXPLICIT;
            caps->feedback_endpoint = cur.feedback_ep;
        } else if (cur.implicit && caps->feedback == AMPDAC_FEEDBACK_NONE) {
            caps->feedback = AMPDAC_FEEDBACK_IMPLICIT;
        }
        cur = Alt{};
    };

    /* The descriptors file is the device descriptor plus every
     * configuration. Walking all of them lists each alternate twice.
     * Keep the configuration the device is using. */
    uint32_t active_cfg = 1;
    (void)read_file_u32((devdir + "/bConfigurationValue").c_str(), 10, &active_cfg);
    std::size_t begin = 0;
    std::size_t end = n;
    if (n >= 2 && blob[1] == 0x01 && blob[0] >= 2 && blob[0] < n) {
        begin = blob[0];
    }
    for (std::size_t cfg = begin; cfg + 9 <= n && blob[cfg + 1] == 0x02;) {
        unsigned total = static_cast<unsigned>(blob[cfg + 2]) |
                (static_cast<unsigned>(blob[cfg + 3]) << 8);
        if (total < 9 || cfg + total > n) break;
        if (blob[cfg + 5] == active_cfg) {
            begin = cfg;
            end = cfg + total;
            break;
        }
        cfg += total;
    }

    for (std::size_t i = begin; i + 1 < end;) {
        const unsigned len = blob[i];
        if (len < 2 || i + len > end) break;
        const unsigned char *d = blob.data() + i;
        const unsigned kind = d[1];
        if (kind == 0x04 && len >= 9) {
            finish();
            const unsigned cls = d[5];
            const unsigned sub = d[6];
            const unsigned proto = d[7];
            if (cls == 0x01 && sub == 0x02 && proto == 0x20 && d[3] != 0) {
                cur.live = true;
                cur.iface = d[2];
                cur.alt = d[3];
            }
        } else if (cur.live && kind == 0x24 && len >= 3) {
            const unsigned subtype = d[2];
            if (subtype == 0x01 && len >= 16) {
                cur.format_type = d[5];
                cur.bm_formats = static_cast<uint32_t>(d[6]) |
                        (static_cast<uint32_t>(d[7]) << 8) |
                        (static_cast<uint32_t>(d[8]) << 16) |
                        (static_cast<uint32_t>(d[9]) << 24);
                cur.channels = d[10];
            } else if (subtype == 0x02 && len >= 4) {
                if (d[3] == 0x01 && len >= 6) {
                    cur.subslot = d[4];
                    cur.bits = d[5];
                } else if (d[3] == 0x04) {
                    cur.type_iv = true;
                }
            }
        } else if (cur.live && kind == 0x05 && len >= 7) {
            const unsigned addr = d[2];
            const unsigned attr = d[3];
            const unsigned mps = static_cast<unsigned>(d[4]) |
                    (static_cast<unsigned>(d[5]) << 8);
            if ((attr & 0x03u) != 1u || (mps & 0x07FFu) == 0) {
                i += len;
                continue;
            }
            const unsigned usage = (attr >> 4) & 0x03u;
            if ((addr & 0x80u) == 0 && (usage == 0 || usage == 2)) {
                cur.mps = static_cast<uint16_t>(mps);
                cur.sync = static_cast<uint8_t>((attr >> 2) & 0x03u);
                if (usage == 2) cur.implicit = true;
            } else if ((addr & 0x80u) != 0 && usage == 1) {
                cur.feedback_ep = static_cast<uint8_t>(addr);
            } else if (usage == 2) {
                cur.implicit = true;
            }
        }
        i += len;
    }
    finish();
    if (caps->sample_rate_count == 0) {
        std::snprintf(caps->notes, sizeof(caps->notes),
                      "PCM rates are read when the DAC is not playing");
    }
    return true;
}

bool kernel_usb_playing() {
    for (int card = 0; card < 32; ++card) {
        char path[96];
        std::snprintf(path, sizeof(path), "/proc/asound/card%d/usbid", card);
        if (access(path, F_OK) != 0) continue;
        const char *subs[] = {
            "/proc/asound/card%d/pcm0p/sub0/status",
            "/proc/asound/card%d/pcm0p/sub1/status",
            "/proc/asound/card%d/pcm1p/sub0/status",
        };
        for (const char *fmt : subs) {
            std::snprintf(path, sizeof(path), fmt, card);
            FILE *file = std::fopen(path, "r");
            if (file == nullptr) continue;
            char line[128];
            const bool running = std::fgets(line, sizeof(line), file) != nullptr &&
                    std::strstr(line, "RUNNING") != nullptr;
            std::fclose(file);
            if (running) return true;
        }
    }
    return false;
}

const char *sync_name(ampdac_sync_mode mode) {
    switch (mode) {
        case AMPDAC_SYNC_ASYNC: return "async, DAC is clock master";
        case AMPDAC_SYNC_ADAPTIVE: return "adaptive, DAC follows the host";
        case AMPDAC_SYNC_SYNC: return "sync, USB SOF is clock master";
        default: return "unspecified";
    }
}

const char *feedback_name(ampdac_feedback_mode mode) {
    switch (mode) {
        case AMPDAC_FEEDBACK_EXPLICIT: return "explicit feedback";
        case AMPDAC_FEEDBACK_IMPLICIT: return "implicit feedback";
        default: return "none";
    }
}

std::string format_number_list(const char *label, const uint32_t *values, int count) {
    std::vector<uint32_t> sorted(values, values + (count > 0 ? count : 0));
    std::sort(sorted.begin(), sorted.end());
    std::string line = label;
    int column = static_cast<int>(line.size());
    for (uint32_t value : sorted) {
        char num[24];
        const int n = std::snprintf(num, sizeof(num), " %u", value);
        if (n < 0) continue;
        if (column + n > 78) {
            line += "\n";
            line.append(std::strlen(label), ' ');
            column = static_cast<int>(std::strlen(label));
        }
        line += num;
        column += n;
    }
    if (sorted.empty()) line += " none reported";
    line += "\n";
    return line;
}

std::string format_caps(const ampdac_capabilities &caps) {
    char line[320];
    std::string out;
    const char *product = caps.device.product[0] != '\0'
            ? caps.device.product : "USB audio";
    std::snprintf(line, sizeof(line), "DAC: %s\n", product);
    out += line;
    if (caps.device.manufacturer[0] != '\0') {
        std::snprintf(line, sizeof(line), "manufacturer: %s\n", caps.device.manufacturer);
        out += line;
    }
    std::snprintf(line, sizeof(line),
                  "USB: %04x:%04x  bus %u  address %u",
                  caps.device.vid, caps.device.pid,
                  caps.device.bus, caps.device.address);
    out += line;
    if (caps.device.serial[0] != '\0') {
        std::snprintf(line, sizeof(line), "  serial %s", caps.device.serial);
        out += line;
    }
    out += "\n";
    std::snprintf(line, sizeof(line), "clock: %s, %s",
                  sync_name(caps.sync), feedback_name(caps.feedback));
    out += line;
    if (caps.feedback_endpoint != 0U) {
        std::snprintf(line, sizeof(line), ", endpoint 0x%02x", caps.feedback_endpoint);
        out += line;
    }
    out += "\n";
    std::snprintf(line, sizeof(line), "channels: up to %u\n", caps.channels_max);
    out += line;

    std::vector<uint32_t> depths;
    for (int i = 0; i < caps.bit_depth_count; ++i) depths.push_back(caps.bit_depths[i]);
    out += format_number_list("PCM bits:", depths.data(), static_cast<int>(depths.size()));
    out += format_number_list("PCM rates:", caps.sample_rates, caps.sample_rate_count);

    if (caps.alt_count <= 0) {
        out += "alts: none reported\n";
    } else {
        out += "alts:\n";
        for (int i = 0; i < caps.alt_count; ++i) {
            const ampdac_alt_info &alt = caps.alts[i];
            if (alt.native_dsd != 0U) {
                std::snprintf(line, sizeof(line),
                              "  interface %u alt %u  native DSD  %u ch  %u bytes/uframe\n",
                              alt.interface_number, alt.alt_setting,
                              alt.channels, alt.bytes_per_uframe);
            } else if (alt.valid_bits > 0U && alt.subslot_bytes > 0U) {
                std::snprintf(line, sizeof(line),
                              "  interface %u alt %u  PCM  %u bit  %u-byte slot  %u ch  %u bytes/uframe\n",
                              alt.interface_number, alt.alt_setting,
                              alt.valid_bits, alt.subslot_bytes,
                              alt.channels, alt.bytes_per_uframe);
            } else {
                std::snprintf(line, sizeof(line),
                              "  interface %u alt %u  PCM  %u ch  %u bytes/uframe\n",
                              alt.interface_number, alt.alt_setting,
                              alt.channels, alt.bytes_per_uframe);
            }
            out += line;
        }
    }

    if (caps.native_dsd != 0U) {
        std::snprintf(line, sizeof(line),
                      "DSD native: yes, interface %u, alt %u\n",
                      caps.native_dsd_interface, caps.native_dsd_alt);
    } else {
        std::snprintf(line, sizeof(line), "DSD native: no\n");
    }
    out += line;
    if (caps.dop != 0U) {
        std::snprintf(line, sizeof(line), "DoP: yes, PCM alt %u\n", caps.dop_pcm_alt);
    } else {
        std::snprintf(line, sizeof(line), "DoP: no\n");
    }
    out += line;
    if (caps.notes[0] != '\0') {
        std::snprintf(line, sizeof(line), "detail: %s\n", caps.notes);
        out += line;
    }
    return out;
}

std::string ensure_dac_header(bool playback_active, ampdac_session *session,
                              bool open_device = false) {
    /* Opening the DAC to refresh this text claims the control interface.
     * On KA13 that puts the feature unit back to 0 dB. A different DAC is
     * identified from the descriptor the kernel already cached. */
    (void)session;
    const DacId bus = uac2_on_bus();
    if (!bus.known) {
        std::lock_guard<std::mutex> lock(g_dac_mu);
        g_dac_ok = false;
        g_dac_id = DacId{};
        g_dac_header = "DAC: no UAC2 device\n";
        return g_dac_header;
    }
    {
        std::lock_guard<std::mutex> lock(g_dac_mu);
        if (g_dac_ok && same_dac(g_dac_id, bus)) return g_dac_header;
    }

    const bool playing = playback_active || kernel_usb_playing();
    if (open_device && !playing) {
        ampdac_capabilities probed{};
        if (ampdac_probe(0, 0, &probed) == AMPDAC_OK &&
            probed.device.vid == bus.vid && probed.device.pid == bus.pid) {
            std::lock_guard<std::mutex> lock(g_dac_mu);
            remember_caps(probed);
            return g_dac_header;
        }
    }

    ampdac_capabilities from_sys{};
    if (!caps_from_sysfs(bus, &from_sys)) {
        std::lock_guard<std::mutex> lock(g_dac_mu);
        g_dac_ok = false;
        g_dac_id = DacId{};
        g_dac_header = "DAC: no UAC2 device\n";
        return g_dac_header;
    }
    std::lock_guard<std::mutex> lock(g_dac_mu);
    remember_caps(from_sys);
    return g_dac_header;
}

std::string format_transport(const ampdac_negotiated &n, const ampdac_stream_stats &st,
                             uint8_t source_bits) {
    char line[192];
    std::string out;
    out += "transport: playing\n";

    const char *layout = n.channels == 2 ? "stereo" : nullptr;
    if (n.dsd) {
        const uint32_t dsd_hz = n.dsd_native ? n.sample_rate_hz * 32u : n.sample_rate_hz * 16u;
        const char *name = dsd_name(dsd_hz);
        if (layout != nullptr && name != nullptr) {
            std::snprintf(line, sizeof(line),
                          "stream: %s %s, USB %u Hz, %s\n",
                          n.dsd_native ? "DSD native" : "DoP",
                          name, n.sample_rate_hz, layout);
        } else {
            std::snprintf(line, sizeof(line),
                          "stream: %s %u Hz, USB %u Hz, %u ch\n",
                          n.dsd_native ? "DSD native" : "DoP",
                          dsd_hz, n.sample_rate_hz, n.channels);
        }
    } else if (source_bits == 16 || source_bits == 24 || source_bits == 32) {
        if (layout != nullptr) {
            std::snprintf(line, sizeof(line),
                          "stream: PCM %u Hz, %u bit, %s\n",
                          n.sample_rate_hz, source_bits, layout);
        } else {
            std::snprintf(line, sizeof(line),
                          "stream: PCM %u Hz, %u bit, %u ch\n",
                          n.sample_rate_hz, source_bits, n.channels);
        }
        out += line;
        if (n.subslot_bytes == 3U) {
            std::snprintf(line, sizeof(line),
                          "USB slot: 3 bytes packed, %u bit, source %u, rate unchanged\n",
                          n.valid_bits, source_bits);
            out += line;
        } else if (source_bits != n.valid_bits) {
            std::snprintf(line, sizeof(line),
                          "USB slot: %u bit, source %u in the top bits, rate unchanged\n",
                          n.valid_bits, source_bits);
            out += line;
        }
        line[0] = '\0';
    } else if (layout != nullptr) {
        std::snprintf(line, sizeof(line),
                      "stream: PCM %u Hz, USB slot %u bit, %s\n",
                      n.sample_rate_hz, n.valid_bits, layout);
    } else {
        std::snprintf(line, sizeof(line),
                      "stream: PCM %u Hz, USB slot %u bit, %u ch\n",
                      n.sample_rate_hz, n.valid_bits, n.channels);
    }
    if (line[0] != '\0') out += line;

    if (n.dsd && n.dsd_native) {
        std::snprintf(line, sizeof(line),
                      "alt: interface %u, alt %u, native DSD\n",
                      n.interface_number, n.alt_setting);
    } else if (n.dsd) {
        std::snprintf(line, sizeof(line),
                      "alt: interface %u, alt %u, DoP, %u valid bits, %u-byte slot\n",
                      n.interface_number, n.alt_setting, n.valid_bits, n.subslot_bytes);
    } else {
        std::snprintf(line, sizeof(line),
                      "alt: interface %u, alt %u, %u valid bits, %u-byte slot\n",
                      n.interface_number, n.alt_setting, n.valid_bits, n.subslot_bytes);
    }
    out += line;

    if (n.sync == AMPDAC_SYNC_ASYNC && n.feedback == AMPDAC_FEEDBACK_EXPLICIT) {
        if (st.feedback_q16_16 == 0u) {
            out += "clock: DAC master, feedback not received yet\n";
        } else {
            const double hz = static_cast<double>(st.feedback_q16_16) / 65536.0 * 8000.0;
            if (n.sample_rate_hz > 0u) {
                const double ppm = (hz - static_cast<double>(n.sample_rate_hz)) /
                        static_cast<double>(n.sample_rate_hz) * 1000000.0;
                std::snprintf(line, sizeof(line),
                              "clock: DAC master, feedback %.1f Hz (%+.0f ppm)\n",
                              hz, ppm);
            } else {
                std::snprintf(line, sizeof(line),
                              "clock: DAC master, feedback %.1f Hz\n", hz);
            }
            out += line;
        }
    } else if (n.sync == AMPDAC_SYNC_ADAPTIVE) {
        out += "clock: host master, DAC adapts\n";
    } else if (n.sync == AMPDAC_SYNC_SYNC) {
        out += "clock: USB master, not the DAC\n";
    } else {
        out += "clock: no feedback\n";
    }

    const unsigned ring_kb = st.ring_bytes / 1024u;
    const unsigned cap_kb = st.ring_capacity / 1024u;
    const unsigned pct = st.ring_capacity == 0u
            ? 0u
            : static_cast<unsigned>(
                    (static_cast<uint64_t>(st.ring_bytes) * 100u) / st.ring_capacity);
    std::snprintf(line, sizeof(line), "ring: %u/%u KB, %u%%\n", ring_kb, cap_kb, pct);
    out += line;

    if (st.underrun_count == 0u) {
        out += "losses: none\n";
    } else {
        std::snprintf(line, sizeof(line),
                      "losses: %llu underruns, silence was sent to the DAC\n",
                      static_cast<unsigned long long>(st.underrun_count));
        out += line;
    }
    return out;
}

/** DAC descriptor, then the live stream or an idle line. */
std::string peek_transport() {
    std::string stream;
    bool live = false;
    std::string out;
    {
        std::lock_guard<std::mutex> lock(g_pub_mu);
        live = g_pub_session != nullptr && g_pub_started;
        if (live) {
            ampdac_negotiated neg{};
            ampdac_stream_stats stats{};
            if (ampdac_negotiated_format(g_pub_session, &neg) == AMPDAC_OK &&
                ampdac_get_stats(g_pub_session, &stats) == AMPDAC_OK) {
                stream = format_transport(neg, stats, g_pub_source_bits);
            }
            /* Hold the publish lock so the session cannot close under describe. */
            out = ensure_dac_header(true, g_pub_session);
        }
    }
    if (!live) out = ensure_dac_header(false, nullptr);
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += "\n";
    if (stream.empty()) out += "playback: idle\n";
    else out += stream;
    return out;
}

void on_signal(int) {
    g_stop.store(true);
    if (g_wake_write >= 0) {
        const char b = 1;
        (void)write(g_wake_write, &b, 1);
    }
}

void track_fd(int fd) {
    std::lock_guard<std::mutex> lock(g_fds_mu);
    g_live_fds.push_back(fd);
}

void untrack_fd(int fd) {
    std::lock_guard<std::mutex> lock(g_fds_mu);
    for (auto it = g_live_fds.begin(); it != g_live_fds.end(); ++it) {
        if (*it == fd) {
            g_live_fds.erase(it);
            return;
        }
    }
}

void shutdown_clients() {
    std::lock_guard<std::mutex> lock(g_fds_mu);
    for (int fd : g_live_fds) {
        ::shutdown(fd, SHUT_RDWR);
    }
}

bool read_hdr_payload(int fd, ampdac_ipc_hdr *hdr, std::vector<uint8_t> *payload) {
    if (ampdac_ipc_read_full(fd, hdr, sizeof(*hdr)) != 0) return false;
    if (hdr->magic != AMPDAC_IPC_MAGIC || hdr->version != AMPDAC_IPC_VERSION) {
        return false;
    }
    if (hdr->length > AMPDAC_IPC_MAX_PAYLOAD) return false;
    payload->assign(hdr->length, 0);
    if (hdr->length > 0 &&
        ampdac_ipc_read_full(fd, payload->data(), hdr->length) != 0) {
        return false;
    }
    return true;
}

bool reply_status(int fd, uint16_t type, int32_t status, const std::string &err) {
    std::string clipped = err;
    if (clipped.size() > 480) clipped.resize(480);
    const bool with_err = status < 0 && !clipped.empty();
    return ampdac_ipc_send(
                   fd, type, status,
                   with_err ? clipped.data() : nullptr,
                   with_err ? static_cast<uint32_t>(clipped.size()) : 0u) == 0;
}

bool reply_bytes(int fd, uint16_t type, int32_t status,
                 const void *data, uint32_t length) {
    return ampdac_ipc_send(fd, type, status, data, length) == 0;
}

void release_session(ampdac_session *&session) {
    if (session != nullptr) {
        unpublish_session(session);
        ampdac_close(session);
        session = nullptr;
    }
}

bool claim_owner(int fd) {
    int expected = -1;
    if (g_owner.compare_exchange_strong(expected, fd)) return true;
    return g_owner.load() == fd;
}

void drop_owner(int fd) {
    int expected = fd;
    g_owner.compare_exchange_strong(expected, -1);
}

void serve(int fd) {
    track_fd(fd);
    bool owner = false;
    ampdac_session *session = nullptr;
    bool started = false;

    std::vector<uint8_t> payload;
    for (;;) {
        ampdac_ipc_hdr hdr{};
        if (!read_hdr_payload(fd, &hdr, &payload)) break;

        if (hdr.type == AMPDAC_IPC_LIST) {
            uint32_t max_devices = 16;
            if (payload.size() >= sizeof(uint32_t)) {
                std::memcpy(&max_devices, payload.data(), sizeof(max_devices));
            }
            if (max_devices == 0 || max_devices > 32) max_devices = 16;
            std::vector<ampdac_device_info> devices(max_devices);
            const int n = ampdac_list_devices(devices.data(), static_cast<int>(max_devices));
            if (n < 0) {
                if (!reply_status(fd, AMPDAC_IPC_LIST, n, ampdac_last_error())) break;
                continue;
            }
            if (!reply_bytes(fd, AMPDAC_IPC_LIST, n, devices.data(),
                             static_cast<uint32_t>(n) * sizeof(ampdac_device_info))) {
                break;
            }
            continue;
        }

        if (hdr.type == AMPDAC_IPC_CONFIGURE_PCM ||
            hdr.type == AMPDAC_IPC_CONFIGURE_DSD) {
            /* Track changes close the socket and open a new one while the
             * previous thread is still releasing USB. Wait for that owner
             * instead of failing the new player on the first try. */
            bool claimed = claim_owner(fd);
            for (int i = 0; !claimed && i < 50; ++i) {
                usleep(100 * 1000);
                claimed = claim_owner(fd);
            }
            if (!claimed) {
                if (!reply_status(fd, hdr.type, AMPDAC_ERR_BUSY,
                                  "another player holds the DAC")) break;
                continue;
            }
            owner = true;
            release_session(session);
            started = false;

            uint16_t vid = 0;
            uint16_t pid = 0;
            if (hdr.type == AMPDAC_IPC_CONFIGURE_PCM) {
                if (payload.size() != sizeof(ampdac_ipc_pcm)) {
                    if (!reply_status(fd, hdr.type, AMPDAC_ERR_INVALID, "bad PCM message")) break;
                    continue;
                }
                ampdac_ipc_pcm msg{};
                std::memcpy(&msg, payload.data(), sizeof(msg));
                vid = msg.vid;
                pid = msg.pid;
                session = ampdac_open(vid, pid);
                if (session == nullptr) {
                    if (!reply_status(fd, hdr.type, AMPDAC_ERR_NO_DEVICE, ampdac_last_error())) break;
                    continue;
                }
                ampdac_pcm_config cfg{};
                cfg.sample_rate_hz = msg.sample_rate_hz;
                cfg.channels = msg.channels;
                cfg.format = static_cast<ampdac_pcm_format>(msg.format);
                cfg.volume = static_cast<float>(msg.volume_ppm) /
                        static_cast<float>(AMPDAC_IPC_VOLUME_UNITY);
                const ampdac_status st = ampdac_configure_pcm(session, &cfg);
                if (st != AMPDAC_OK) {
                    const std::string err = ampdac_last_error();
                    release_session(session);
                    if (!reply_status(fd, hdr.type, st, err)) break;
                    continue;
                }
                uint8_t source_bits = 0;
                if (cfg.format == AMPDAC_PCM_S16LE) source_bits = 16;
                else if (cfg.format == AMPDAC_PCM_S24_3LE) source_bits = 24;
                else if (cfg.format == AMPDAC_PCM_S32LE) source_bits = 32;
                note_source_bits(source_bits);
            } else {
                if (payload.size() != sizeof(ampdac_ipc_dsd)) {
                    if (!reply_status(fd, hdr.type, AMPDAC_ERR_INVALID, "bad DSD message")) break;
                    continue;
                }
                ampdac_ipc_dsd msg{};
                std::memcpy(&msg, payload.data(), sizeof(msg));
                vid = msg.vid;
                pid = msg.pid;
                session = ampdac_open(vid, pid);
                if (session == nullptr) {
                    if (!reply_status(fd, hdr.type, AMPDAC_ERR_NO_DEVICE, ampdac_last_error())) break;
                    continue;
                }
                ampdac_dsd_config cfg{};
                cfg.dsd_rate_hz = msg.dsd_rate_hz;
                cfg.channels = msg.channels;
                cfg.transport = static_cast<ampdac_dsd_transport>(msg.transport);
                const ampdac_status st = ampdac_configure_dsd(session, &cfg);
                if (st != AMPDAC_OK) {
                    const std::string err = ampdac_last_error();
                    release_session(session);
                    if (!reply_status(fd, hdr.type, st, err)) break;
                    continue;
                }
            }
            std::fprintf(stderr, "ampdacd: configured %04x:%04x\n", vid, pid);
            publish_session(session, false);
            if (!reply_status(fd, hdr.type, AMPDAC_OK, {})) break;
            continue;
        }

        if (hdr.type == AMPDAC_IPC_PEEK) {
            const std::string text = peek_transport();
            if (text.empty()) {
                if (!reply_status(fd, hdr.type, AMPDAC_ERR_STATE, {})) break;
                continue;
            }
            if (!reply_bytes(fd, hdr.type, AMPDAC_OK, text.data(),
                             static_cast<uint32_t>(text.size()))) {
                break;
            }
            continue;
        }

        if (hdr.type == AMPDAC_IPC_CLOSE) {
            release_session(session);
            started = false;
            if (!reply_status(fd, hdr.type, AMPDAC_OK, {})) break;
            continue;
        }

        if (!owner) {
            if (!reply_status(fd, hdr.type, AMPDAC_ERR_STATE, "configure the DAC first")) break;
            continue;
        }

        if (hdr.type == AMPDAC_IPC_DROP) {
            const ampdac_status st = session == nullptr
                    ? AMPDAC_OK
                    : ampdac_drop(session);
            if (st != AMPDAC_OK) {
                if (!reply_status(fd, hdr.type, st, ampdac_last_error())) break;
                continue;
            }
            if (!reply_status(fd, hdr.type, AMPDAC_OK, {})) break;
            continue;
        }

        if (hdr.type == AMPDAC_IPC_START) {
            if (session == nullptr) {
                if (!reply_status(fd, hdr.type, AMPDAC_ERR_STATE, "not configured")) break;
                continue;
            }
            if (!started) {
                const ampdac_status st = ampdac_start(session);
                if (st != AMPDAC_OK) {
                    if (!reply_status(fd, hdr.type, st, ampdac_last_error())) break;
                    continue;
                }
                started = true;
                publish_session(session, true);
            }
            if (!reply_status(fd, hdr.type, AMPDAC_OK, {})) break;
            continue;
        }

        if (hdr.type == AMPDAC_IPC_WRITE) {
            if (session == nullptr || !started) {
                if (!reply_status(fd, hdr.type, AMPDAC_ERR_STATE, "not started")) break;
                continue;
            }
            const int n = ampdac_write(session, payload.data(), payload.size());
            if (n < 0) {
                if (!reply_status(fd, hdr.type, n, ampdac_last_error())) break;
                continue;
            }
            if (!reply_status(fd, hdr.type, n, {})) break;
            continue;
        }

        if (hdr.type == AMPDAC_IPC_NEGOTIATED) {
            ampdac_negotiated neg{};
            const ampdac_status st = ampdac_negotiated_format(session, &neg);
            if (st != AMPDAC_OK) {
                if (!reply_status(fd, hdr.type, st, ampdac_last_error())) break;
                continue;
            }
            if (!reply_bytes(fd, hdr.type, AMPDAC_OK, &neg, sizeof(neg))) break;
            continue;
        }

        if (hdr.type == AMPDAC_IPC_STATS) {
            ampdac_stream_stats stats{};
            const ampdac_status st = ampdac_get_stats(session, &stats);
            if (st != AMPDAC_OK) {
                if (!reply_status(fd, hdr.type, st, ampdac_last_error())) break;
                continue;
            }
            if (!reply_bytes(fd, hdr.type, AMPDAC_OK, &stats, sizeof(stats))) break;
            continue;
        }

        if (!reply_status(fd, hdr.type, AMPDAC_ERR_INVALID, "unknown message")) break;
    }

    release_session(session);
    if (owner) drop_owner(fd);
    untrack_fd(fd);
    close(fd);
}

int listen_socket(const std::string &path, bool *unlink_on_exit) {
    *unlink_on_exit = false;
    const char *listen_fds = std::getenv("LISTEN_FDS");
    if (listen_fds != nullptr && std::atoi(listen_fds) >= 1) {
        std::fprintf(stderr, "ampdacd: socket activation on fd 3\n");
        return 3;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    ::unlink(path.c_str());
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "ampdacd: socket path too long\n");
        close(fd);
        return -1;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 8) != 0) {
        std::perror("listen");
        close(fd);
        return -1;
    }
    const mode_t mode = geteuid() == 0 ? 0660 : 0666;
    if (chmod(path.c_str(), mode) != 0) std::perror("chmod");
    if (geteuid() == 0) {
        struct group *grp = getgrnam("audio");
        if (grp != nullptr) {
            if (chown(path.c_str(), 0, grp->gr_gid) != 0) std::perror("chown");
        }
    }
    *unlink_on_exit = true;
    std::fprintf(stderr, "ampdacd: listening on %s\n", path.c_str());
    return fd;
}

}  // namespace

int main(int argc, char **argv) {
    std::string path = ampdac_client_default_socket();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::fprintf(stderr, "usage: ampdacd [--socket PATH]\n");
            return 0;
        } else {
            std::fprintf(stderr, "ampdacd: unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    int wake[2];
    if (pipe(wake) != 0) {
        std::perror("pipe");
        return 1;
    }
    g_wake_write = wake[1];

    bool unlink_on_exit = false;
    const int listen_fd = listen_socket(path, &unlink_on_exit);
    if (listen_fd < 0) return 1;

    uac2_volume_service_start();
    (void)ensure_dac_header(false, nullptr, true);

    while (!g_stop.load()) {
        struct pollfd pfds[2];
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = wake[0];
        pfds[1].events = POLLIN;
        const int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            break;
        }
        if (pfds[1].revents & POLLIN) break;
        if ((pfds[0].revents & POLLIN) == 0) continue;
        const int fd = accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (g_stop.load()) break;
            std::perror("accept");
            break;
        }
        std::thread(serve, fd).detach();
    }

    g_stop.store(true);
    shutdown_clients();
    // shutdown() on a socket passed by systemd makes the socket unit fail
    // with "Got POLLHUP on a listening socket". Close it and leave the inode.
    if (unlink_on_exit) {
        ::shutdown(listen_fd, SHUT_RDWR);
    }
    close(listen_fd);

    for (int i = 0; i < 30; ++i) {
        bool idle = false;
        {
            std::lock_guard<std::mutex> lock(g_fds_mu);
            idle = g_live_fds.empty() && g_owner.load() == -1;
        }
        if (idle) break;
        usleep(100000);
    }
    uac2_volume_service_stop();
    if (unlink_on_exit) ::unlink(path.c_str());
    close(wake[0]);
    close(wake[1]);
    std::fprintf(stderr, "ampdacd: exit\n");
    return 0;
}

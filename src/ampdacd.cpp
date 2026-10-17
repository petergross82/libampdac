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

std::string ensure_dac_header(bool playback_active) {
    if (playback_active || kernel_usb_playing()) {
        std::lock_guard<std::mutex> lock(g_dac_mu);
        if (!g_dac_header.empty()) return g_dac_header;
        return "DAC: descriptor not read while the kernel driver is playing\n";
    }
    {
        std::lock_guard<std::mutex> lock(g_dac_mu);
        if (g_dac_ok && !g_dac_header.empty()) return g_dac_header;
    }
    ampdac_capabilities caps{};
    const ampdac_status st = ampdac_probe(0, 0, &caps);
    std::lock_guard<std::mutex> lock(g_dac_mu);
    if (st != AMPDAC_OK) {
        g_dac_ok = false;
        g_dac_header = std::string("DAC: no UAC2 device (") + ampdac_last_error() + ")\n";
        return g_dac_header;
    }
    g_dac_header = format_caps(caps);
    g_dac_ok = true;
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
        }
    }
    std::string out = ensure_dac_header(live);
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
    (void)ensure_dac_header(false);

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
    ::shutdown(listen_fd, SHUT_RDWR);
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

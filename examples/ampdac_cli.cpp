#include "ampdac.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  ampdac-cli list\n"
                 "  ampdac-cli probe [--vid 0xXXXX --pid 0xYYYY]\n"
                 "  ampdac-cli play FILE [FILE ...] [--dop|--native-dsd] "
                 "[--vid 0xXXXX --pid 0xYYYY]\n"
                 "  ampdac-cli FILE [FILE ...]\n"
                 "      same rate/channels/DSD family → one USB session, gapless\n"
                 "      PCM: no resample (USB clock = file rate, 16-in-32 / 24-in-32)\n"
                 "      DSD: demux-copy; AUTO = Native RAW if advertised, else DoP\n"
                 "      --dop forces DSD-over-PCM (clock = DSD/16, markers 0x05/0xFA)\n"
                 "  ampdac-cli play --rate HZ --channels N --format s16le|s32le|f32le "
                 "[--vid 0xXXXX --pid 0xYYYY] < pcm\n"
                 "  ampdac-cli tone [--rate 48000] [--hz 1000] [--seconds 20] "
                 "[--vid 0xXXXX --pid 0xYYYY]\n");
}

static std::atomic<bool> g_stop{false};
static std::atomic<pid_t> g_child{0};
static std::atomic<pid_t> g_child_next{0};

static void on_sigint(int) {
    g_stop.store(true, std::memory_order_relaxed);
    const pid_t child = g_child.load(std::memory_order_relaxed);
    if (child > 0) {
        kill(child, SIGTERM);
    }
    const pid_t next = g_child_next.load(std::memory_order_relaxed);
    if (next > 0) {
        kill(next, SIGTERM);
    }
}

static int cmd_list() {
    ampdac_device_info devices[32];
    const int n = ampdac_list_devices(devices, 32);
    if (n < 0) {
        std::fprintf(stderr, "list failed: %s (%s)\n",
                     ampdac_status_string(static_cast<ampdac_status>(n)),
                     ampdac_last_error());
        return 1;
    }
    if (n == 0) {
        std::puts("No UAC2 DACs found.");
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        std::printf("%04x:%04x  bus %u addr %u  %s %s  serial=%s\n",
                    devices[i].vid, devices[i].pid,
                    devices[i].bus, devices[i].address,
                    devices[i].manufacturer, devices[i].product,
                    devices[i].serial);
    }
    return 0;
}

static uint16_t parse_hex(const char *s) {
    return static_cast<uint16_t>(std::strtoul(s, nullptr, 0));
}

static const char *sync_name(ampdac_sync_mode m) {
    switch (m) {
        case AMPDAC_SYNC_ASYNC: return "async (DAC clock master)";
        case AMPDAC_SYNC_ADAPTIVE: return "adaptive";
        case AMPDAC_SYNC_SYNC: return "sync (USB SOF master)";
        default: return "none";
    }
}

static const char *fb_name(ampdac_feedback_mode m) {
    switch (m) {
        case AMPDAC_FEEDBACK_EXPLICIT: return "explicit ISO IN";
        case AMPDAC_FEEDBACK_IMPLICIT: return "implicit";
        default: return "none";
    }
}

static void print_caps(const ampdac_capabilities &caps) {
    std::printf("%04x:%04x  bus %u addr %u  %s %s\n",
                caps.device.vid, caps.device.pid,
                caps.device.bus, caps.device.address,
                caps.device.manufacturer, caps.device.product);
    std::printf("  sync:     %s\n", sync_name(caps.sync));
    std::printf("  feedback: %s", fb_name(caps.feedback));
    if (caps.feedback_endpoint) {
        std::printf(" ep=0x%02X", caps.feedback_endpoint);
    }
    std::putchar('\n');
    std::printf("  channels: up to %u\n", caps.channels_max);
    std::printf("  depths:  ");
    for (int i = 0; i < caps.bit_depth_count; ++i) {
        std::printf(" %u", caps.bit_depths[i]);
    }
    std::putchar('\n');
    std::printf("  rates:   ");
    for (int i = 0; i < caps.sample_rate_count; ++i) {
        std::printf(" %u", caps.sample_rates[i]);
    }
    std::putchar('\n');
    std::printf("  dsd:     native=%s", caps.native_dsd ? "yes" : "no");
    if (caps.native_dsd) {
        std::printf(" iface=%u alt=%u", caps.native_dsd_interface, caps.native_dsd_alt);
    }
    std::printf(" dop=%s", caps.dop ? "yes" : "no");
    if (caps.dop) {
        std::printf(" pcm_alt=%u", caps.dop_pcm_alt);
    }
    std::putchar('\n');
    std::printf("  %s\n", caps.notes);
}

static int cmd_probe(int argc, char **argv) {
    uint16_t vid = 0;
    uint16_t pid = 0;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            vid = parse_hex(argv[++i]);
        } else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = parse_hex(argv[++i]);
        } else {
            usage();
            return 1;
        }
    }

    ampdac_capabilities caps{};
    const ampdac_status st = ampdac_probe(vid, pid, &caps);
    if (st != AMPDAC_OK) {
        std::fprintf(stderr, "probe failed: %s (%s)\n",
                     ampdac_status_string(st), ampdac_last_error());
        return 1;
    }
    print_caps(caps);
    return 0;
}

static void drain_ring(ampdac_session *session) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(2000);
    while (clock::now() < deadline) {
        ampdac_stream_stats st{};
        if (ampdac_get_stats(session, &st) != AMPDAC_OK) return;
        if (st.ring_bytes < 512U) return;
        usleep(2000);
    }
}

struct FileAudio {
    uint32_t rate = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    bool dsd = false;
    uint32_t block_align = 0;
    char codec[64] = "";
};

static void print_stats(double elapsed, const ampdac_stream_stats &st);

static int stream_from_file(ampdac_session *session, FILE *in, bool drain_at_end) {
    std::vector<uint8_t> buf(8192);
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto last_log = t0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        const int written = ampdac_write(session, buf.data(), n);
        if (written < 0) {
            std::fprintf(stderr, "write failed: %s (%s)\n",
                         ampdac_status_string(static_cast<ampdac_status>(written)),
                         ampdac_last_error());
            return 1;
        }
        const auto now = clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            ampdac_stream_stats stats{};
            if (ampdac_get_stats(session, &stats) == AMPDAC_OK) {
                print_stats(std::chrono::duration<double>(now - t0).count(), stats);
            }
            last_log = now;
        }
    }
    if (drain_at_end && !g_stop.load(std::memory_order_relaxed)) {
        drain_ring(session);
    }
    return 0;
}

static uint8_t dsd_bit_reverse(uint8_t value) {
    value = static_cast<uint8_t>((value & 0xF0U) >> 4 | (value & 0x0FU) << 4);
    value = static_cast<uint8_t>((value & 0xCCU) >> 2 | (value & 0x33U) << 2);
    value = static_cast<uint8_t>((value & 0xAAU) >> 1 | (value & 0x55U) << 1);
    return value;
}

/** Demux packets → interleaved MSBF DSD bytes (same as Android ffmpeg_bridge). */
static int stream_dsd_from_file(
        ampdac_session *session,
        FILE *in,
        const FileAudio &src,
        bool drain_at_end) {
    const bool planar = std::strstr(src.codec, "planar") != nullptr;
    const bool lsbf = std::strstr(src.codec, "lsbf") != nullptr;
    const uint32_t packet_bytes = src.block_align != 0U
            ? src.block_align
            : 4096U * static_cast<uint32_t>(src.channels);
    if (packet_bytes == 0U || packet_bytes % src.channels != 0U) {
        std::fprintf(stderr, "invalid DSD block_align %u\n", packet_bytes);
        return 1;
    }

    std::vector<uint8_t> packet(packet_bytes);
    std::vector<uint8_t> interleaved(packet_bytes);
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto last_log = t0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const size_t n = std::fread(packet.data(), 1, packet.size(), in);
        if (n == 0) break;
        if (n != packet.size()) {
            break;
        }

        if (planar) {
            const std::size_t plane = packet.size() / src.channels;
            std::size_t out = 0;
            for (std::size_t i = 0; i < plane; ++i) {
                for (uint8_t ch = 0; ch < src.channels; ++ch) {
                    uint8_t value = packet[static_cast<std::size_t>(ch) * plane + i];
                    if (lsbf) value = dsd_bit_reverse(value);
                    interleaved[out++] = value;
                }
            }
        } else {
            for (std::size_t i = 0; i < packet.size(); ++i) {
                uint8_t value = packet[i];
                if (lsbf) value = dsd_bit_reverse(value);
                interleaved[i] = value;
            }
        }

        const int written = ampdac_write(session, interleaved.data(), interleaved.size());
        if (written < 0) {
            std::fprintf(stderr, "write failed: %s (%s)\n",
                         ampdac_status_string(static_cast<ampdac_status>(written)),
                         ampdac_last_error());
            return 1;
        }
        const auto now = clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            ampdac_stream_stats stats{};
            if (ampdac_get_stats(session, &stats) == AMPDAC_OK) {
                print_stats(std::chrono::duration<double>(now - t0).count(), stats);
            }
            last_log = now;
        }
    }
    if (drain_at_end && !g_stop.load(std::memory_order_relaxed)) {
        drain_ring(session);
    }
    return 0;
}

static ampdac_session *finish_open(ampdac_session *session) {
    ampdac_negotiated neg{};
    if (ampdac_negotiated_format(session, &neg) == AMPDAC_OK) {
        if (neg.dsd) {
            std::fprintf(stderr,
                         "USB DSD %s, clock %u Hz, %u ch, iface=%u alt=%u ep=0x%02X "
                         "sync=%s feedback=%s",
                         neg.dsd_native ? "native DSD_U32LE" : "DoP",
                         neg.sample_rate_hz, neg.channels,
                         neg.interface_number, neg.alt_setting,
                         neg.endpoint_address,
                         sync_name(neg.sync), fb_name(neg.feedback));
        } else {
            std::fprintf(stderr,
                         "USB %u Hz, %u ch, %u valid bits in %u-byte subslot, "
                         "iface=%u alt=%u ep=0x%02X sync=%s feedback=%s",
                         neg.sample_rate_hz, neg.channels, neg.valid_bits,
                         neg.subslot_bytes, neg.interface_number, neg.alt_setting,
                         neg.endpoint_address,
                         sync_name(neg.sync), fb_name(neg.feedback));
        }
        if (neg.feedback_endpoint) {
            std::fprintf(stderr, " 0x%02X", neg.feedback_endpoint);
        }
        std::fputc('\n', stderr);
    }
    const ampdac_status start_st = ampdac_start(session);
    if (start_st != AMPDAC_OK) {
        std::fprintf(stderr, "start failed: %s (%s)\n",
                     ampdac_status_string(start_st), ampdac_last_error());
        ampdac_close(session);
        return nullptr;
    }
    return session;
}

static ampdac_session *open_configured(
        uint16_t vid, uint16_t pid,
        uint32_t rate, uint8_t channels, ampdac_pcm_format format) {
    ampdac_session *session = ampdac_open(vid, pid);
    if (session == nullptr) {
        std::fprintf(stderr, "open failed: %s\n", ampdac_last_error());
        return nullptr;
    }
    ampdac_pcm_config cfg{};
    cfg.sample_rate_hz = rate;
    cfg.channels = channels;
    cfg.format = format;
    cfg.volume = 1.0f;
    const ampdac_status st = ampdac_configure_pcm(session, &cfg);
    if (st != AMPDAC_OK) {
        std::fprintf(stderr, "configure failed: %s (%s)\n",
                     ampdac_status_string(st), ampdac_last_error());
        ampdac_close(session);
        return nullptr;
    }
    return finish_open(session);
}

static ampdac_session *open_configured_dsd(
        uint16_t vid, uint16_t pid, uint32_t dsd_rate_hz, uint8_t channels,
        ampdac_dsd_transport transport) {
    ampdac_session *session = ampdac_open(vid, pid);
    if (session == nullptr) {
        std::fprintf(stderr, "open failed: %s\n", ampdac_last_error());
        return nullptr;
    }
    ampdac_dsd_config cfg{};
    cfg.dsd_rate_hz = dsd_rate_hz;
    cfg.channels = channels;
    cfg.transport = transport;
    const ampdac_status st = ampdac_configure_dsd(session, &cfg);
    if (st != AMPDAC_OK) {
        std::fprintf(stderr, "configure DSD failed: %s (%s)\n",
                     ampdac_status_string(st), ampdac_last_error());
        ampdac_close(session);
        return nullptr;
    }
    return finish_open(session);
}

static int capture_stdout(char *const argv[], std::string *out) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    char buf[512];
    ssize_t nread;
    while ((nread = read(fds[0], buf, sizeof(buf))) > 0) {
        out->append(buf, static_cast<size_t>(nread));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

static int probe_file_audio(const char *path, FileAudio *info) {
    char *argv[] = {
            const_cast<char *>("ffprobe"),
            const_cast<char *>("-v"),
            const_cast<char *>("error"),
            const_cast<char *>("-select_streams"),
            const_cast<char *>("a:0"),
            const_cast<char *>("-show_entries"),
            const_cast<char *>("stream=sample_rate,channels,bits_per_raw_sample,bits_per_sample,codec_name,block_align"),
            const_cast<char *>("-of"),
            const_cast<char *>("default=noprint_wrappers=1"),
            const_cast<char *>(path),
            nullptr,
    };
    std::string out;
    if (capture_stdout(argv, &out) != 0) {
        std::fprintf(stderr, "ffprobe failed for %s — install ffmpeg/ffprobe\n", path);
        return 1;
    }
    unsigned rate = 0;
    unsigned channels = 0;
    unsigned raw_bits = 0;
    unsigned bits = 0;
    unsigned block_align = 0;
    char codec[64] = "";
    const char *p = out.c_str();
    while (*p != '\0') {
        if (std::sscanf(p, "sample_rate=%u", &rate) == 1) {
        } else if (std::sscanf(p, "channels=%u", &channels) == 1) {
        } else if (std::sscanf(p, "bits_per_raw_sample=%u", &raw_bits) == 1) {
        } else if (std::sscanf(p, "bits_per_sample=%u", &bits) == 1) {
        } else if (std::sscanf(p, "block_align=%u", &block_align) == 1) {
        } else {
            (void)std::sscanf(p, "codec_name=%63s", codec);
        }
        const char *nl = std::strchr(p, '\n');
        if (nl == nullptr) break;
        p = nl + 1;
    }
    if (rate == 0 || channels == 0) {
        std::fprintf(stderr, "could not parse ffprobe output: %s\n", out.c_str());
        return 1;
    }
    info->rate = rate;
    info->channels = static_cast<uint8_t>(channels);
    info->block_align = block_align;
    std::snprintf(info->codec, sizeof(info->codec), "%s", codec);
    info->dsd = std::strstr(codec, "dsd") != nullptr || raw_bits == 1U || bits == 1U;
    info->bits = static_cast<uint8_t>(raw_bits != 0U ? raw_bits : bits);
    if (info->dsd) {
        info->bits = 1;
    } else if (info->bits == 0U) {
        info->bits = 16;
    }
    return 0;
}

struct Decoder {
    FILE *fp = nullptr;
    pid_t pid = 0;
};

static void enlarge_pipe(int fd) {
#ifdef F_SETPIPE_SZ
    (void)fcntl(fd, F_SETPIPE_SZ, 256 * 1024);
#else
    (void)fd;
#endif
}

static Decoder spawn_ffmpeg(char *const argv[]) {
    Decoder dec{};
    int fds[2];
    if (pipe(fds) != 0) return dec;
    enlarge_pipe(fds[0]);
    enlarge_pipe(fds[1]);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return dec;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        std::fprintf(stderr, "ffmpeg not found in PATH\n");
        _exit(127);
    }
    close(fds[1]);
    dec.pid = pid;
    dec.fp = fdopen(fds[0], "rb");
    if (dec.fp == nullptr) {
        close(fds[0]);
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        dec.pid = 0;
    }
    return dec;
}

static Decoder ffmpeg_decode_pcm(const char *path, bool s16) {
    const char *fmt = s16 ? "s16le" : "s32le";
    const char *codec = s16 ? "pcm_s16le" : "pcm_s32le";
    char *argv[] = {
            const_cast<char *>("ffmpeg"),
            const_cast<char *>("-hide_banner"),
            const_cast<char *>("-loglevel"),
            const_cast<char *>("error"),
            const_cast<char *>("-i"),
            const_cast<char *>(path),
            const_cast<char *>("-map"),
            const_cast<char *>("0:a:0"),
            const_cast<char *>("-f"),
            const_cast<char *>(fmt),
            const_cast<char *>("-c:a"),
            const_cast<char *>(codec),
            const_cast<char *>("-"),
            nullptr,
    };
    return spawn_ffmpeg(argv);
}

static Decoder ffmpeg_decode_dsd(const char *path) {
    char *argv[] = {
            const_cast<char *>("ffmpeg"),
            const_cast<char *>("-hide_banner"),
            const_cast<char *>("-loglevel"),
            const_cast<char *>("error"),
            const_cast<char *>("-i"),
            const_cast<char *>(path),
            const_cast<char *>("-map"),
            const_cast<char *>("0:a:0"),
            const_cast<char *>("-c:a"),
            const_cast<char *>("copy"),
            const_cast<char *>("-f"),
            const_cast<char *>("data"),
            const_cast<char *>("-"),
            nullptr,
    };
    return spawn_ffmpeg(argv);
}

static Decoder start_decoder(const char *path, const FileAudio &src, bool s16) {
    return src.dsd ? ffmpeg_decode_dsd(path) : ffmpeg_decode_pcm(path, s16);
}

static void reap_decoder(Decoder *dec, std::atomic<pid_t> *slot) {
    if (dec == nullptr) return;
    if (dec->fp != nullptr) {
        std::fclose(dec->fp);
        dec->fp = nullptr;
    }
    if (dec->pid > 0) {
        if (slot != nullptr && slot->load(std::memory_order_relaxed) == dec->pid) {
            slot->store(0, std::memory_order_relaxed);
        }
        int status = 0;
        waitpid(dec->pid, &status, 0);
        dec->pid = 0;
    }
}

static bool dac_has_rate(const ampdac_capabilities &caps, uint32_t rate) {
    if (caps.sample_rate_count <= 0) return true;
    for (int i = 0; i < caps.sample_rate_count; ++i) {
        if (caps.sample_rates[i] == rate) return true;
    }
    return false;
}

static uint32_t canonical_dsd_rate_hz(uint32_t probe_hz) {
    if (probe_hz == 2822400U || probe_hz == 5644800U || probe_hz == 11289600U) {
        return probe_hz;
    }
    const uint32_t unpacked = probe_hz * 8U;
    if (unpacked == 2822400U || unpacked == 5644800U || unpacked == 11289600U) {
        return unpacked;
    }
    return 0;
}

static bool same_usb_format(const FileAudio &a, const FileAudio &b) {
    if (a.dsd != b.dsd || a.channels != b.channels) return false;
    if (a.dsd) {
        const uint32_t ra = canonical_dsd_rate_hz(a.rate);
        const uint32_t rb = canonical_dsd_rate_hz(b.rate);
        return ra != 0U && ra == rb;
    }
    return a.rate == b.rate && (a.bits <= 16U) == (b.bits <= 16U);
}

static int play_files(const std::vector<const char *> &paths, uint16_t vid, uint16_t pid,
                      ampdac_dsd_transport dsd_transport) {
    if (paths.empty()) return 1;

    std::vector<FileAudio> tracks(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        if (probe_file_audio(paths[i], &tracks[i]) != 0) return 1;
        if (tracks[i].channels != 1 && tracks[i].channels != 2) {
            std::fprintf(stderr, "need 1 or 2 channels, file has %u (%s)\n",
                         tracks[i].channels, paths[i]);
            return 1;
        }
        if (tracks[i].dsd && tracks[i].channels != 2) {
            std::fprintf(stderr, "DSD is stereo-only in this CLI (%s)\n", paths[i]);
            return 1;
        }
        if (i > 0 && !same_usb_format(tracks[0], tracks[i])) {
            std::fprintf(stderr,
                         "gapless requires the same USB format. "
                         "%s does not match %s (rate/channels/DSD family/bit packing)\n",
                         paths[i], paths[0]);
            return 1;
        }
    }

    const FileAudio &lead = tracks[0];
    ampdac_capabilities caps{};
    const bool have_caps = ampdac_probe(vid, pid, &caps) == AMPDAC_OK;
    if (!lead.dsd) {
        if (have_caps && !dac_has_rate(caps, lead.rate)) {
            std::fprintf(stderr,
                         "file is %u Hz — DAC does not list that rate. "
                         "Will not resample. Rates:",
                         lead.rate);
            for (int i = 0; i < caps.sample_rate_count; ++i) {
                std::fprintf(stderr, " %u", caps.sample_rates[i]);
            }
            std::fputc('\n', stderr);
            return 1;
        }
    } else if (have_caps) {
        const uint32_t dsd_hz = canonical_dsd_rate_hz(lead.rate);
        const bool use_dop =
                dsd_transport == AMPDAC_DSD_DOP ||
                (dsd_transport == AMPDAC_DSD_AUTO && caps.native_dsd == 0U);
        if (use_dop) {
            if (caps.dop == 0U) {
                std::fprintf(stderr,
                             "DAC has no DoP-capable TYPE_I PCM alt "
                             "(need 24/32-bit subslot)\n");
                return 1;
            }
            const uint32_t carrier = dsd_hz / 16U;
            if (carrier != 0U && !dac_has_rate(caps, carrier)) {
                std::fprintf(stderr,
                             "DoP needs PCM clock %u Hz (DSD/16). DAC rates:",
                             carrier);
                for (int i = 0; i < caps.sample_rate_count; ++i) {
                    std::fprintf(stderr, " %u", caps.sample_rates[i]);
                }
                std::fputc('\n', stderr);
                return 1;
            }
        }
    }

    const bool s16 = !lead.dsd && lead.bits <= 16U;
    if (lead.dsd) {
        std::fprintf(stderr,
                     "gapless %zu DSD file(s), probe=%u Hz %u ch — one USB session\n",
                     paths.size(), lead.rate, lead.channels);
    } else {
        std::fprintf(stderr,
                     "gapless %zu file(s), %u-bit %u Hz %u ch — one USB session, no resample\n",
                     paths.size(), lead.bits, lead.rate, lead.channels);
    }

    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    ampdac_session *session = lead.dsd
            ? open_configured_dsd(vid, pid, lead.rate, lead.channels, dsd_transport)
            : open_configured(vid, pid, lead.rate, lead.channels,
                              s16 ? AMPDAC_PCM_S16LE : AMPDAC_PCM_S32LE);
    if (session == nullptr) return 1;

    Decoder current = start_decoder(paths[0], tracks[0], s16);
    g_child.store(current.pid, std::memory_order_relaxed);
    if (current.fp == nullptr) {
        std::fprintf(stderr, "could not start ffmpeg decoder\n");
        ampdac_close(session);
        return 1;
    }

    int rc = 0;
    for (size_t i = 0; i < paths.size() && rc == 0 &&
         !g_stop.load(std::memory_order_relaxed); ++i) {
        Decoder next{};
        bool next_ok = true;
        if (i + 1 < paths.size()) {
            next = start_decoder(paths[i + 1], tracks[i + 1], s16);
            g_child_next.store(next.pid, std::memory_order_relaxed);
            if (next.fp == nullptr) {
                std::fprintf(stderr, "could not start ffmpeg for %s\n", paths[i + 1]);
                next_ok = false;
            }
        }

        std::fprintf(stderr, "track %zu/%zu  %s\n", i + 1, paths.size(), paths[i]);
        const bool last = (i + 1 == paths.size()) || !next_ok;
        rc = tracks[i].dsd
                ? stream_dsd_from_file(session, current.fp, tracks[i], last)
                : stream_from_file(session, current.fp, last);

        reap_decoder(&current, &g_child);
        if (!next_ok || rc != 0 || g_stop.load(std::memory_order_relaxed)) {
            if (!next_ok) rc = 1;
            reap_decoder(&next, &g_child_next);
            break;
        }
        current = next;
        g_child.store(current.pid, std::memory_order_relaxed);
        g_child_next.store(0, std::memory_order_relaxed);
    }

    reap_decoder(&current, &g_child);
    Decoder leftover{};
    leftover.pid = g_child_next.exchange(0, std::memory_order_relaxed);
    if (leftover.pid > 0) {
        kill(leftover.pid, SIGTERM);
        reap_decoder(&leftover, nullptr);
    }
    ampdac_close(session);
    return rc;
}

static int cmd_play(int argc, char **argv) {
    uint32_t rate = 0;
    uint8_t channels = 0;
    ampdac_pcm_format format = AMPDAC_PCM_S16LE;
    uint16_t vid = 0;
    uint16_t pid = 0;
    ampdac_dsd_transport dsd_transport = AMPDAC_DSD_AUTO;
    std::vector<const char *> paths;

    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--channels") == 0 && i + 1 < argc) {
            channels = static_cast<uint8_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            ++i;
            if (std::strcmp(argv[i], "s16le") == 0) format = AMPDAC_PCM_S16LE;
            else if (std::strcmp(argv[i], "s32le") == 0) format = AMPDAC_PCM_S32LE;
            else if (std::strcmp(argv[i], "f32le") == 0) format = AMPDAC_PCM_F32LE;
            else {
                usage();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            vid = parse_hex(argv[++i]);
        } else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = parse_hex(argv[++i]);
        } else if (std::strcmp(argv[i], "--dop") == 0) {
            dsd_transport = AMPDAC_DSD_DOP;
        } else if (std::strcmp(argv[i], "--native-dsd") == 0) {
            dsd_transport = AMPDAC_DSD_NATIVE;
        } else if (argv[i][0] != '-') {
            paths.push_back(argv[i]);
        } else {
            usage();
            return 1;
        }
    }

    if (!paths.empty()) {
        if (rate != 0U || channels != 0U) {
            std::fprintf(stderr,
                         "play FILE uses the file's rate/depth. "
                         "Do not pass --rate/--channels (that would resample).\n");
            return 1;
        }
        return play_files(paths, vid, pid, dsd_transport);
    }

    if (rate == 0 || channels == 0) {
        usage();
        return 1;
    }

    std::signal(SIGINT, on_sigint);
    ampdac_session *session = open_configured(vid, pid, rate, channels, format);
    if (session == nullptr) return 1;
    const int rc = stream_from_file(session, stdin, true);
    ampdac_close(session);
    return rc;
}

static void print_stats(double elapsed, const ampdac_stream_stats &st) {
    std::fprintf(stderr, "t=%.1fs  ring=%u/%u KB  underruns=%llu  ",
                 elapsed,
                 st.ring_bytes / 1024U,
                 st.ring_capacity / 1024U,
                 static_cast<unsigned long long>(st.underrun_count));
    if (st.feedback_q16_16 == 0U) {
        std::fprintf(stderr, "feedback=waiting (ISO still on SOF/Bresenham)\n");
        return;
    }
    const double hz = static_cast<double>(st.feedback_q16_16) / 65536.0 * 8000.0;
    std::fprintf(stderr, "feedback=Q16.16 0x%08X ≈ %.1f Hz (DAC is clock master)\n",
                 st.feedback_q16_16, hz);
}

static int cmd_tone(int argc, char **argv) {
    uint32_t rate = 48000;
    double tone_hz = 1000.0;
    double seconds = 20.0;
    uint16_t vid = 0;
    uint16_t pid = 0;

    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--hz") == 0 && i + 1 < argc) {
            tone_hz = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            vid = parse_hex(argv[++i]);
        } else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = parse_hex(argv[++i]);
        } else {
            usage();
            return 1;
        }
    }
    if (rate == 0 || tone_hz <= 0.0) {
        usage();
        return 1;
    }

    ampdac_session *session = ampdac_open(vid, pid);
    if (session == nullptr) {
        std::fprintf(stderr, "open failed: %s\n", ampdac_last_error());
        return 1;
    }

    ampdac_pcm_config cfg{};
    cfg.sample_rate_hz = rate;
    cfg.channels = 2;
    cfg.format = AMPDAC_PCM_S32LE;
    cfg.volume = 1.0f;

    ampdac_status st = ampdac_configure_pcm(session, &cfg);
    if (st != AMPDAC_OK) {
        std::fprintf(stderr, "configure failed: %s (%s)\n",
                     ampdac_status_string(st), ampdac_last_error());
        ampdac_close(session);
        return 1;
    }

    ampdac_negotiated neg{};
    if (ampdac_negotiated_format(session, &neg) == AMPDAC_OK) {
        std::fprintf(stderr,
                     "tone %.0f Hz at %u Hz stereo s32le (amp=0.1, unity USB volume)\n"
                     "negotiated %u valid bits in %u-byte subslot, iface=%u alt=%u "
                     "ep=0x%02X sync=%s feedback=%s",
                     tone_hz, rate, neg.valid_bits, neg.subslot_bytes,
                     neg.interface_number, neg.alt_setting, neg.endpoint_address,
                     sync_name(neg.sync), fb_name(neg.feedback));
        if (neg.feedback_endpoint) {
            std::fprintf(stderr, " 0x%02X", neg.feedback_endpoint);
        }
        std::fputc('\n', stderr);
    }

    st = ampdac_start(session);
    if (st != AMPDAC_OK) {
        std::fprintf(stderr, "start failed: %s (%s)\n",
                     ampdac_status_string(st), ampdac_last_error());
        ampdac_close(session);
        return 1;
    }

    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    constexpr int kFrames = 2048;
    std::vector<int32_t> pcm(static_cast<size_t>(kFrames) * 2U);
    const double two_pi = 8.0 * std::atan(1.0);
    const double amp = 0.1 * 2147483647.0;
    double phase = 0.0;
    const double step = two_pi * tone_hz / static_cast<double>(rate);

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto last_log = t0;
    int rc = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        if (seconds > 0.0) {
            const double elapsed = std::chrono::duration<double>(clock::now() - t0).count();
            if (elapsed >= seconds) break;
        }
        for (int i = 0; i < kFrames; ++i) {
            const auto s = static_cast<int32_t>(amp * std::sin(phase));
            pcm[static_cast<size_t>(i) * 2U] = s;
            pcm[static_cast<size_t>(i) * 2U + 1U] = s;
            phase += step;
            if (phase >= two_pi) phase -= two_pi;
        }
        const int written = ampdac_write(
                session, pcm.data(), pcm.size() * sizeof(int32_t));
        if (written < 0) {
            std::fprintf(stderr, "write failed: %s (%s)\n",
                         ampdac_status_string(static_cast<ampdac_status>(written)),
                         ampdac_last_error());
            rc = 1;
            break;
        }
        const auto now = clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            ampdac_stream_stats stats{};
            if (ampdac_get_stats(session, &stats) == AMPDAC_OK) {
                print_stats(std::chrono::duration<double>(now - t0).count(), stats);
            }
            last_log = now;
        }
    }

    ampdac_stream_stats stats{};
    if (ampdac_get_stats(session, &stats) == AMPDAC_OK) {
        print_stats(std::chrono::duration<double>(clock::now() - t0).count(), stats);
    }
    drain_ring(session);
    ampdac_close(session);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    if (std::strcmp(argv[1], "list") == 0) {
        return cmd_list();
    }
    if (std::strcmp(argv[1], "probe") == 0) {
        return cmd_probe(argc - 2, argv + 2);
    }
    if (std::strcmp(argv[1], "play") == 0) {
        return cmd_play(argc - 2, argv + 2);
    }
    if (std::strcmp(argv[1], "tone") == 0) {
        return cmd_tone(argc - 2, argv + 2);
    }
    if (argv[1][0] != '-') {
        return cmd_play(argc - 1, argv + 1);
    }
    usage();
    return 1;
}

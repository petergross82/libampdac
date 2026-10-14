/**
 * ampdacd — one UAC2 DAC, one client.
 *
 * systemd starts this with CAP_SYS_NICE and CAP_SYS_ADMIN so the libusb
 * event thread can take SCHED_FIFO and the kernel audio driver can be
 * unbound. Players connect to the unix socket and never touch /dev/bus/usb.
 */

#include "ampdac_ipc.h"

#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

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
            if (!claim_owner(fd)) {
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
            if (!reply_status(fd, hdr.type, AMPDAC_OK, {})) break;
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
    if (unlink_on_exit) ::unlink(path.c_str());
    close(wake[0]);
    close(wake[1]);
    std::fprintf(stderr, "ampdacd: exit\n");
    return 0;
}

#include "ampdac_kernel.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "libusb/libusb.h"

namespace {

std::string usb_sysname(libusb_device *dev) {
    const uint8_t bus = libusb_get_bus_number(dev);
    uint8_t ports[7]{};
    const int depth = libusb_get_port_numbers(dev, ports, 7);
    char buf[64];
    if (depth <= 0) {
        std::snprintf(buf, sizeof(buf), "%u-0", bus);
        return buf;
    }
    int off = std::snprintf(buf, sizeof(buf), "%u-%u", bus, ports[0]);
    for (int i = 1; i < depth && off > 0 && off < static_cast<int>(sizeof(buf) - 8); ++i) {
        off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                             ".%u", ports[i]);
    }
    return buf;
}

std::string iface_sys_id(const std::string &sysname, uint8_t config, uint8_t iface) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s:%u.%u", sysname.c_str(), config, iface);
    return buf;
}

std::string read_driver_name(const std::string &iface_id) {
    const std::string link = "/sys/bus/usb/devices/" + iface_id + "/driver";
    char target[256]{};
    const ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
    if (n <= 0) return {};
    target[n] = '\0';
    const char *slash = std::strrchr(target, '/');
    return slash ? (slash + 1) : std::string(target);
}

bool write_sysfs(const std::string &path, const std::string &value) {
    std::ofstream out(path);
    if (!out) return false;
    out << value;
    out.flush();
    return static_cast<bool>(out);
}

bool sysfs_unbind(const std::string &iface_id, std::string *driver_out) {
    const std::string driver = read_driver_name(iface_id);
    if (driver.empty()) return false;
    const std::string path = "/sys/bus/usb/devices/" + iface_id + "/driver/unbind";
    if (!write_sysfs(path, iface_id)) return false;
    if (driver_out) *driver_out = driver;
    return true;
}

}  // namespace

bool ampdac_kernel_takeover(libusb_device_handle *handle,
                            AmpdacKernelClaim *state,
                            std::string *error) {
    if (handle == nullptr || state == nullptr) return false;
    *state = AmpdacKernelClaim{};

    (void)libusb_set_auto_detach_kernel_driver(handle, 1);

    libusb_device *dev = libusb_get_device(handle);
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != LIBUSB_SUCCESS) {
        if (error) *error = "no active USB configuration";
        return false;
    }

    const std::string sysname = usb_sysname(dev);
    const uint8_t config_value = cfg->bConfigurationValue;
    bool any_busy = false;

    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        const int iface = cfg->interface[i].altsetting[0].bInterfaceNumber;
        const int active = libusb_kernel_driver_active(handle, iface);
        if (active == 0) continue;

        int rc = libusb_detach_kernel_driver(handle, iface);
        if (rc == LIBUSB_SUCCESS) {
            state->detached_ifaces.push_back(iface);
            continue;
        }

        // PipeWire/Pulse often holds the ALSA pcm node, so USBDEVFS_DISCONNECT
        // returns BUSY. Unbind the interface through sysfs (needs write access
        // to the driver unbind node — typically root, or a udev ACL).
        std::string driver;
        const std::string id = iface_sys_id(sysname, config_value,
                                            static_cast<uint8_t>(iface));
        if (sysfs_unbind(id, &driver)) {
            usleep(50 * 1000);
            rc = libusb_detach_kernel_driver(handle, iface);
            if (rc == LIBUSB_SUCCESS ||
                libusb_kernel_driver_active(handle, iface) == 0) {
                state->detached_ifaces.push_back(iface);
                state->sysfs_unbound.push_back({id, driver});
                continue;
            }
        }

        if (rc == LIBUSB_ERROR_BUSY || rc == LIBUSB_ERROR_NOT_FOUND) {
            any_busy = true;
        }
        if (error) {
            std::ostringstream oss;
            oss << "kernel still owns USB iface " << iface
                << " (" << libusb_error_name(rc)
                << "). Stop PipeWire/Pulse playback on this DAC, then retry. "
                   "Root fallback: echo -n '"
                << iface_sys_id(sysname, config_value, static_cast<uint8_t>(iface))
                << "' > /sys/bus/usb/devices/"
                << iface_sys_id(sysname, config_value, static_cast<uint8_t>(iface))
                << "/driver/unbind";
            *error = oss.str();
        }
    }

    libusb_free_config_descriptor(cfg);
    return !any_busy;
}

void ampdac_kernel_restore(const AmpdacKernelClaim &state) {
    for (const auto &u : state.sysfs_unbound) {
        if (u.driver.empty() || u.id.empty()) continue;
        const std::string path = "/sys/bus/usb/drivers/" + u.driver + "/bind";
        (void)write_sysfs(path, u.id);
    }
}

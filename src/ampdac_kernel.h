#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct libusb_device_handle;

/**
 * Linux-only takeover of a USB DAC from snd-usb-audio (and sibling drivers).
 *
 * Android cannot USBDEVFS_DISCONNECT from an app process; this path is compiled
 * only into libampdac. On close, sysfs bind puts the kernel driver back so
 * PipeWire/ALSA see the card again.
 */
struct AmpdacKernelClaim {
    struct UnboundIface {
        std::string id;      ///< e.g. "3-4:1.0"
        std::string driver;  ///< e.g. "snd-usb-audio"
    };

    std::vector<int> detached_ifaces;
    std::vector<UnboundIface> sysfs_unbound;
    /** Sysfs name of the DAC, e.g. "1-1.1.2". Restore binds any audio
     *  interface under it that is still without a driver. */
    std::string sysname;
};

bool ampdac_kernel_takeover(libusb_device_handle *handle,
                            AmpdacKernelClaim *state,
                            std::string *error);

void ampdac_kernel_restore(const AmpdacKernelClaim &state);

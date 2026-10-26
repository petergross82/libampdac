#pragma once

#include <cstddef>
#include <cstdint>

struct libusb_device_handle;

/**
 * HID consumer volume buttons mapped onto a UAC2 Feature Unit.
 *
 * Some DACs (SMSL DS20) apply their own knob inside the device and never
 * ask the host to change a feature-unit control. Others (FiiO KA13) send
 * Consumer Volume Increment / Decrement on a HID interrupt endpoint and
 * expect the host to SET_CUR the feature unit. The level is digital
 * (1/256 dB) and is left at the device's current value until a button
 * edge arrives.
 */
struct Uac2HidVolumeReport {
    int up_bit = -1;
    int down_bit = -1;
    int relative_bit = -1;
    int relative_bits = 0;
    int report_id = 0;
};

bool uac2_hid_parse_consumer_volume(const uint8_t *desc,
                                    std::size_t length,
                                    Uac2HidVolumeReport *out) noexcept;

/**
 * Background reader for HID volume buttons. It claims only the HID
 * interface. While libampdac is playing, that claim moves onto the
 * playback handle so the button read and the isochronous stream share
 * one USB file. A second usbfs client with its own interrupt pipe stops
 * explicit-feedback IN on dwc_otg, and the DAC then stays silent. The
 * level is written through the kernel mixer while snd-usb-audio owns the
 * card, and through the playback handle while libampdac is open.
 */
void uac2_volume_service_start();
void uac2_volume_service_stop() noexcept;

/** Drop our Audio Control claim so a playback session can take it. */
void uac2_volume_service_yield_ac();

/** Playback session now owns the Audio Control interface on this handle. */
void uac2_volume_service_attach(libusb_device_handle *handle, int ac_interface);

/**
 * Queue the HID read on the playback handle. Call only after that handle's
 * event thread is running and feedback IN has been submitted.
 */
void uac2_volume_service_arm_session_hid(libusb_device_handle *handle);

/** Session handle must not be used any more. The AC claim is still the session's. */
void uac2_volume_service_detach() noexcept;

/** Session has released Audio Control. The service may claim it again. */
void uac2_volume_service_ac_free() noexcept;

/**
 * Read the feature-unit level before a streaming alt change. Many DACs
 * mute or drop to minimum when the alt setting changes; the saved level
 * is written back by uac2_volume_service_restore.
 */
void uac2_volume_service_remember(struct libusb_device_handle *handle,
                                  int ac_interface);

/** Write the level captured by uac2_volume_service_remember. */
void uac2_volume_service_restore(struct libusb_device_handle *handle);

/**
 * KA13 volume is the host's. Write the level the buttons last selected
 * (starting at -21 dB, not 0 dB). Other DACs are not passed here.
 */
void uac2_volume_service_lock_full(struct libusb_device_handle *handle);

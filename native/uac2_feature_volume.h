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
 * Background reader for HID volume buttons. It owns the HID interface for
 * the life of the process and writes the UAC2 feature unit directly, so
 * the buttons work while playback is stopped. During playback the audio
 * session owns the Audio Control interface; the service yields that claim
 * and writes volume through the session handle.
 */
void uac2_volume_service_start();
void uac2_volume_service_stop() noexcept;

/** Drop our Audio Control claim so a playback session can take it. */
void uac2_volume_service_yield_ac();

/** Playback session now owns the Audio Control interface on this handle. */
void uac2_volume_service_attach(libusb_device_handle *handle, int ac_interface);

/** Session handle must not be used any more. The AC claim is still the session's. */
void uac2_volume_service_detach() noexcept;

/** Session has released Audio Control. The service may claim it again. */
void uac2_volume_service_ac_free() noexcept;

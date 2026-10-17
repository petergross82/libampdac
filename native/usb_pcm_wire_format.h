#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Describes the exact integer PCM representation accepted by a negotiated
 * USB Audio Class streaming endpoint.
 *
 * UAC2 distinguishes the physical subslot width from the number of meaningful
 * sample bits. For example, a DAC may advertise 24 valid bits in a four-byte
 * subslot. A three-byte subslot is the packed 24-bit fallback used only when
 * the DAC has no four-byte PCM alternate. Keeping both values together
 * prevents producers from inventing a wire layout from the source file's bit
 * depth.
 */
struct UsbPcmWireFormat {
    uint32_t sample_rate_hz = 0;
    uint8_t channel_count = 0;
    uint8_t subslot_bytes = 4;
    uint8_t valid_bits = 32;

    /**
     * Returns whether the format can be represented by the direct USB PCM
     * formatter.
     */
    [[nodiscard]] constexpr bool is_valid() const noexcept
    {
        const bool slot4 = subslot_bytes == 4 && valid_bits > 0 && valid_bits <= 32;
        const bool slot3 = subslot_bytes == 3 && valid_bits > 0 && valid_bits <= 24;
        return sample_rate_hz > 0 &&
               channel_count > 0 &&
               (slot4 || slot3);
    }

    /**
     * Returns the number of bytes occupied by one interleaved PCM frame.
     */
    [[nodiscard]] constexpr std::size_t bytes_per_frame() const noexcept
    {
        return static_cast<std::size_t>(channel_count) * subslot_bytes;
    }
};

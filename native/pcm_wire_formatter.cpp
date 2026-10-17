#include "pcm_wire_formatter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr int64_t kInt32Min = std::numeric_limits<int32_t>::min();
constexpr int64_t kInt32Max = std::numeric_limits<int32_t>::max();

int16_t read_s16le(const uint8_t *source) noexcept
{
    const auto bits = static_cast<uint16_t>(
            static_cast<uint16_t>(source[0]) |
            (static_cast<uint16_t>(source[1]) << 8U));
    return static_cast<int16_t>(bits);
}

int32_t read_s24_3le(const uint8_t *source) noexcept
{
    int32_t sample = static_cast<int32_t>(source[0]) |
                     (static_cast<int32_t>(source[1]) << 8) |
                     (static_cast<int32_t>(source[2]) << 16);
    if ((sample & 0x800000) != 0) {
        sample |= ~0xFFFFFF;
    }
    return sample;
}

int32_t read_s32le(const uint8_t *source) noexcept
{
    const auto bits =
            static_cast<uint32_t>(source[0]) |
            (static_cast<uint32_t>(source[1]) << 8U) |
            (static_cast<uint32_t>(source[2]) << 16U) |
            (static_cast<uint32_t>(source[3]) << 24U);
    return static_cast<int32_t>(bits);
}

void write_s32le(uint8_t *destination, int32_t sample) noexcept
{
    const auto bits = static_cast<uint32_t>(sample);
    destination[0] = static_cast<uint8_t>(bits & 0xFFU);
    destination[1] = static_cast<uint8_t>((bits >> 8U) & 0xFFU);
    destination[2] = static_cast<uint8_t>((bits >> 16U) & 0xFFU);
    destination[3] = static_cast<uint8_t>((bits >> 24U) & 0xFFU);
}

/** Top 24 bits of an MSB-aligned 32-bit sample, packed little-endian. */
void write_packed24(uint8_t *destination, int32_t sample) noexcept
{
    const auto bits = static_cast<uint32_t>(sample) >> 8U;
    destination[0] = static_cast<uint8_t>(bits & 0xFFU);
    destination[1] = static_cast<uint8_t>((bits >> 8U) & 0xFFU);
    destination[2] = static_cast<uint8_t>((bits >> 16U) & 0xFFU);
}

} // namespace

PcmFormatResult PcmWireFormatter::format_subslot3(
        const uint8_t *input,
        std::size_t input_bytes,
        PcmSourceEncoding encoding,
        double gain,
        uint8_t *output,
        std::size_t output_capacity) const noexcept
{
    std::size_t source_bytes = 4U;
    if (encoding == PcmSourceEncoding::S16Le) source_bytes = 2U;
    else if (encoding == PcmSourceEncoding::S24_3Le) source_bytes = 3U;
    if (input_bytes % source_bytes != 0U) {
        return {};
    }

    const std::size_t sample_count = input_bytes / source_bytes;
    if (sample_count > output_capacity / 3U) {
        return {};
    }

    const bool unity = gain == 1.0;
    for (std::size_t index = 0; index < sample_count; ++index) {
        int64_t wide_sample = 0;
        if (encoding == PcmSourceEncoding::S16Le) {
            const auto source = read_s16le(input + index * 2U);
            if (unity) {
                wide_sample = static_cast<int64_t>(source) * 65536LL;
            } else {
                wide_sample = std::llround(
                        static_cast<double>(source) * gain * 65536.0);
            }
        } else if (encoding == PcmSourceEncoding::S24_3Le) {
            const auto source = read_s24_3le(input + index * 3U);
            if (unity) {
                wide_sample = static_cast<int64_t>(source) * 256LL;
            } else {
                wide_sample = std::llround(
                        static_cast<double>(source) * gain * 256.0);
            }
        } else {
            const auto source = read_s32le(input + index * 4U);
            wide_sample = unity
                ? static_cast<int64_t>(source)
                : std::llround(static_cast<double>(source) * gain);
        }

        write_packed24(
                output + index * 3U,
                quantize_to_valid_bits(wide_sample));
    }

    return {sample_count * 3U, true};
}

PcmFormatResult PcmWireFormatter::format_float32_subslot3(
        const uint8_t *input,
        std::size_t input_bytes,
        double gain,
        uint8_t *output,
        std::size_t output_capacity) const noexcept
{
    if (input_bytes % sizeof(float) != 0U) {
        return {};
    }
    const std::size_t sample_count = input_bytes / sizeof(float);
    if (sample_count > output_capacity / 3U) {
        return {};
    }
    for (std::size_t index = 0; index < sample_count; ++index) {
        float decoded = 0.0F;
        std::memcpy(&decoded, input + index * sizeof(float), sizeof(decoded));
        const double normalized = std::isfinite(decoded)
            ? std::clamp(static_cast<double>(decoded) * gain, -1.0, 1.0)
            : 0.0;
        const double scaled = normalized >= 0.0
            ? normalized * static_cast<double>(kInt32Max)
            : normalized * -static_cast<double>(kInt32Min);
        write_packed24(
                output + index * 3U,
                quantize_to_valid_bits(std::llround(scaled)));
    }
    return {sample_count * 3U, true};
}

PcmFormatResult PcmWireFormatter::format(
        const uint8_t *input,
        std::size_t input_bytes,
        PcmSourceEncoding encoding,
        double gain,
        uint8_t *output,
        std::size_t output_capacity) const noexcept
{
    if (input == nullptr || output == nullptr || !format_.is_valid() ||
        !std::isfinite(gain) || gain < 0.0 || gain > 1.0) {
        return {};
    }
    if (format_.subslot_bytes == 3U) {
        return format_subslot3(
                input, input_bytes, encoding, gain, output, output_capacity);
    }

    std::size_t source_bytes = 4U;
    if (encoding == PcmSourceEncoding::S16Le) source_bytes = 2U;
    else if (encoding == PcmSourceEncoding::S24_3Le) source_bytes = 3U;
    if (input_bytes % source_bytes != 0U) {
        return {};
    }

    const std::size_t sample_count = input_bytes / source_bytes;
    if (sample_count > output_capacity / 4U) {
        return {};
    }

    const bool unity = gain == 1.0;
    for (std::size_t index = 0; index < sample_count; ++index) {
        int64_t wide_sample = 0;
        if (encoding == PcmSourceEncoding::S16Le) {
            const auto source = read_s16le(input + index * 2U);
            if (unity) {
                wide_sample = static_cast<int64_t>(source) * 65536LL;
            } else {
                wide_sample = std::llround(
                        static_cast<double>(source) * gain * 65536.0);
            }
        } else if (encoding == PcmSourceEncoding::S24_3Le) {
            /* UAC2 keeps the 24 significant bits at the top of the 4-byte slot. */
            const auto source = read_s24_3le(input + index * 3U);
            if (unity) {
                wide_sample = static_cast<int64_t>(source) * 256LL;
            } else {
                wide_sample = std::llround(
                        static_cast<double>(source) * gain * 256.0);
            }
        } else {
            const auto source = read_s32le(input + index * 4U);
            wide_sample = unity
                ? static_cast<int64_t>(source)
                : std::llround(static_cast<double>(source) * gain);
        }

        write_s32le(
                output + index * 4U,
                quantize_to_valid_bits(wide_sample));
    }

    return {sample_count * 4U, true};
}

PcmFormatResult PcmWireFormatter::format_float32(
        const uint8_t *input,
        std::size_t input_bytes,
        double gain,
        uint8_t *output,
        std::size_t output_capacity) const noexcept
{
    if (input == nullptr || output == nullptr || !format_.is_valid() ||
        !std::isfinite(gain) || gain < 0.0 || gain > 1.0) {
        return {};
    }
    if (format_.subslot_bytes == 3U) {
        return format_float32_subslot3(
                input, input_bytes, gain, output, output_capacity);
    }
    if (input_bytes % sizeof(float) != 0U || output_capacity < input_bytes) {
        return {};
    }

    const std::size_t sample_count = input_bytes / sizeof(float);
    for (std::size_t index = 0; index < sample_count; ++index) {
        float decoded = 0.0F;
        std::memcpy(&decoded, input + index * sizeof(float), sizeof(decoded));
        const double normalized = std::isfinite(decoded)
            ? std::clamp(static_cast<double>(decoded) * gain, -1.0, 1.0)
            : 0.0;
        const double scaled = normalized >= 0.0
            ? normalized * static_cast<double>(kInt32Max)
            : normalized * -static_cast<double>(kInt32Min);
        write_s32le(
                output + index * 4U,
                quantize_to_valid_bits(std::llround(scaled)));
    }
    return {input_bytes, true};
}

int32_t PcmWireFormatter::quantize_to_valid_bits(int64_t sample) const noexcept
{
    sample = std::clamp(sample, kInt32Min, kInt32Max);
    if (format_.valid_bits == 32U) {
        return static_cast<int32_t>(sample);
    }

    const uint8_t padding_bits = static_cast<uint8_t>(32U - format_.valid_bits);
    const int64_t quantum = int64_t{1} << padding_bits;
    const int64_t half_quantum = quantum / 2;

    int64_t units = sample >= 0
        ? (sample + half_quantum) / quantum
        : -(((-sample) + half_quantum) / quantum);

    const int64_t min_units = -(int64_t{1} << (format_.valid_bits - 1U));
    const int64_t max_units = (int64_t{1} << (format_.valid_bits - 1U)) - 1;
    units = std::clamp(units, min_units, max_units);
    return static_cast<int32_t>(units * quantum);
}

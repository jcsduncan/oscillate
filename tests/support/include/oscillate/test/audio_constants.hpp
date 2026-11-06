#pragma once

// Sample rates and block sizes tests share. A test that needs a rate names
// one of these rather than writing 48'000 again; floating-point users convert
// (`static_cast<double>(sample_rate_48k)`, or the *_hz doubles below).

#include <cstddef>
#include <cstdint>

namespace oscillate::test {

// Integral rates, as devices, WAV headers and project records carry them.
inline constexpr std::uint32_t sample_rate_22k = 22'050;
inline constexpr std::uint32_t sample_rate_32k = 32'000;
inline constexpr std::uint32_t sample_rate_44k = 44'100;
inline constexpr std::uint32_t sample_rate_48k = 48'000;
inline constexpr std::uint32_t sample_rate_88k = 88'200;
inline constexpr std::uint32_t sample_rate_96k = 96'000;
inline constexpr std::uint32_t sample_rate_192k = 192'000;

// The same rates as doubles, for DSP that takes a rate in hertz.
inline constexpr double sample_rate_22k_hz = sample_rate_22k;
inline constexpr double sample_rate_32k_hz = sample_rate_32k;
inline constexpr double sample_rate_44k_hz = sample_rate_44k;
inline constexpr double sample_rate_48k_hz = sample_rate_48k;
inline constexpr double sample_rate_88k_hz = sample_rate_88k;
inline constexpr double sample_rate_96k_hz = sample_rate_96k;
inline constexpr double sample_rate_192k_hz = sample_rate_192k;

// The rate most tests run at.
inline constexpr std::uint32_t default_sample_rate = sample_rate_48k;
inline constexpr double default_sample_rate_hz = sample_rate_48k_hz;

// Audio callback block sizes in frames.
inline constexpr std::size_t block_32 = 32;
inline constexpr std::size_t block_64 = 64;
inline constexpr std::size_t block_128 = 128;
inline constexpr std::size_t block_256 = 256;
inline constexpr std::size_t block_512 = 512;
inline constexpr std::size_t block_1024 = 1'024;
inline constexpr std::size_t default_block = block_128;

// Channel counts.
inline constexpr std::uint16_t mono_channels = 1;
inline constexpr std::uint16_t stereo_channels = 2;

} // namespace oscillate::test

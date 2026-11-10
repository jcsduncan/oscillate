#pragma once

#include "oscillate/foundation/split_mix.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Dither for integer output (ADR-0117): TPDF noise of +/-1 LSB added before
// rounding, optionally shaped by error feedback so the quantisation noise
// moves out of the band the ear is most sensitive to. Float output is never
// dithered; integer WAV and FLAC always are.
namespace oscillate::exporting {

enum class DitherShape : std::uint8_t {
  // Plain TPDF: white noise at 1/4 LSB^2 (TPDF 1/6 plus rounding 1/12).
  flat,
  // A three-tap error filter (Wannamaker's E-weighting): 12 to 19 dB less
  // noise up to 5 kHz, up to 11 dB more toward Nyquist.
  shaped,
  // The nine-tap F-weighted filter (Wannamaker): the noise follows the
  // inverse of the ear's threshold, 18 to 25 dB down from 2 to 5 kHz. At
  // 88.2 and 96 kHz both shapes instead take a filter with zeros at 5, 12
  // and 18 kHz that moves the noise above 20 kHz.
  psychoacoustic,
};

inline constexpr std::array<std::string_view, 3> dither_shape_names{"Flat (TPDF)", "Shaped",
                                                                    "Psychoacoustic"};
// The library default keeps rounding error white; the export panel offers
// the shapes.
inline constexpr DitherShape default_dither_shape = DitherShape::flat;

// The error filter's taps for a shape at a rate: the noise transfer is
// 1 - sum(taps[k] z^-(k+1)).
[[nodiscard]] std::span<const double> dither_taps(DitherShape shape,
                                                  std::uint32_t sample_rate) noexcept;

// Quantises interleaved stereo float samples to `bits` (16 or 24) with
// dither, keeping each channel's error history across calls.
class DitherQuantizer {
public:
  static constexpr std::size_t max_taps = 9;

  DitherQuantizer(unsigned bits, DitherShape shape, std::uint32_t sample_rate,
                  std::uint64_t seed) noexcept;

  // One sample of channel `channel` (0 or 1), as a signed integer in the
  // range of `bits`.
  [[nodiscard]] std::int32_t quantize(float sample, std::size_t channel) noexcept;

private:
  std::span<const double> taps_;
  double scale_ = 0.0;
  foundation::SplitMix64 random_;
  std::array<std::array<double, max_taps>, 2> errors_{};
};

} // namespace oscillate::exporting

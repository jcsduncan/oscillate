#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// Tonal balance targets (ADR-0120): what a genre's finished masters look
// like across the spectrum, measured from references. Any device that
// compares or steers a spectrum against a target reads this header: Lens's
// tonal balance view and match EQ, and the adaptive tonal balance device.
namespace oscillate::native_dsp::tonal {

// The grid: third-octave bands on base-2 centres, 1 kHz * 2^(n / 3) for n
// from first_band_step to first_band_step + bands - 1 (24.8 Hz to 16 kHz).
// Each band reaches a sixth of an octave either side of its centre, so the
// bands tile 22.1 Hz to 18.0 kHz without gaps.
inline constexpr std::size_t bands = 29;
inline constexpr int first_band_step = -16;
inline constexpr double bands_per_octave = 3.0;
inline constexpr double pivot_hz = 1'000.0;

[[nodiscard]] double band_hz(std::size_t band) noexcept;
[[nodiscard]] double band_low_hz(std::size_t band) noexcept;
[[nodiscard]] double band_high_hz(std::size_t band) noexcept;

// Per band, in dB.
using Curve = std::array<double, bands>;

// A tonal curve's levels are the long-term average power in each band, in
// dB, loudness-normalised: shifted so that loudness_db is 0. The loudness
// is the spectrum's own K-weighted power, 10 log10 of the sum over bands of
// 10^((level + K) / 10), with K the BS.1770 K-weighting at the band's centre
// (k_weighting_db). Two curves of the same programme at different gains are
// the same curve.
[[nodiscard]] double k_weighting_db(double hz) noexcept;
[[nodiscard]] double loudness_db(const Curve& levels) noexcept;
[[nodiscard]] Curve normalised(const Curve& levels) noexcept;

// A target: per band, the median of its references' normalised curves, and
// the range their middle half covers (low the 25th percentile, high the
// 75th). A target from one reference has the middle half of that
// reference's own curve over time.
struct Target {
  std::string id;   // Stable: "electronica", or "user/<file name>" for a user's.
  std::string name; // As shown: "Electronica".
  std::size_t references = 0;
  Curve median{};
  Curve low{};
  Curve high{};
};

// The factory targets, measured from references (ADR-0120 lists how).
[[nodiscard]] std::span<const Target> factory_targets() noexcept;
[[nodiscard]] const Target* find_factory_target(std::string_view id) noexcept;

// Where a normalised curve sits against a target, per band: below its
// range, inside it (ends included) or above it.
enum class Placement : signed char { below = -1, inside = 0, above = 1 };
using Placements = std::array<Placement, bands>;
[[nodiscard]] Placements assess(const Curve& normalised_levels, const Target& target) noexcept;

} // namespace oscillate::native_dsp::tonal

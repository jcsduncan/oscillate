#pragma once

#include <cmath>
#include <cstddef>

namespace oscillate::native_dsp::effects::reduction_bands {

// Where gain reduction comes from (Ceiling and Press): the detector signal
// split into `count` log-spaced bands from lowest_hz to highest_hz, each
// band's share of the detector's power (0..1, the shares summing to at most
// 1) while reduction is at least active_db, smoothed for display over
// smoothing_seconds and fading to 0 at the same rate once it stops.
inline constexpr std::size_t count = 8;
inline constexpr double lowest_hz = 20.0;
inline constexpr double highest_hz = 20'000.0;
inline constexpr double active_db = 0.1;
inline constexpr double smoothing_seconds = 0.3;

// Band `band` spans edge_hz(band) to edge_hz(band + 1); edge 0 is lowest_hz
// and edge `count` highest_hz. The first and last bands also take what lies
// below and above them.
[[nodiscard]] inline double edge_hz(std::size_t edge) noexcept {
  return lowest_hz *
         std::pow(highest_hz / lowest_hz, static_cast<double>(edge) / static_cast<double>(count));
}

// The band's centre on a log axis.
[[nodiscard]] inline double centre_hz(std::size_t band) noexcept {
  return std::sqrt(edge_hz(band) * edge_hz(band + 1));
}

} // namespace oscillate::native_dsp::effects::reduction_bands

#pragma once

#include "oscillate/clay_dsp/take.hpp"
#include "oscillate/foundation/error.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::clay_dsp {

inline constexpr std::uint32_t min_sample_rate = 22'050;
inline constexpr std::uint32_t max_sample_rate = 192'000;
inline constexpr double max_take_seconds = 3'600.0;

struct AnalysisOptions {
  Mode mode = Mode::melodic;
  // The pitch range notes are searched in.
  double minimum_hz = 55.0;
  double maximum_hz = 1'760.0;
  // Notes shorter than this are dropped.
  double minimum_note_seconds = 0.05;
  // Polyphonic: at most this many simultaneous notes.
  std::size_t max_polyphony = 8;
};

// Planar audio at one rate; `right` may be empty for mono.
struct Audio {
  std::span<const float> left;
  std::span<const float> right;
  std::uint32_t sample_rate = 0;
};

// Offline, worker-side analysis of a whole clip. Deterministic; polls
// `cancelled` between hops.
[[nodiscard]] foundation::Result<Take> analyse(const Audio& audio, const AnalysisOptions& options,
                                               const std::atomic<bool>* cancelled = nullptr);

} // namespace oscillate::clay_dsp

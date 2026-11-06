#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/warp_dsp/source_view.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oscillate::warp_dsp::analysis {

inline constexpr std::size_t max_transients = 20'000;

struct Transient {
  // First sample of the attack, not its peak.
  std::uint64_t frame = 0;
  // Onset-function peak relative to the strongest onset in the source (0..1].
  float strength = 0.0F;
  // Share of the onset's spectral flux above 2 kHz (0..1); high for drums.
  float percussiveness = 0.0F;
};

struct TransientOptions {
  std::uint32_t sample_rate = 48'000;
  // Scales the adaptive threshold; above 1 finds fewer, below 1 more.
  float threshold_scale = 1.0F;
  // Minimum spacing between transients.
  double minimum_gap_seconds = 0.030;
};

// Offline, worker-side detection over the whole view. Bounded by the source
// length (at most one hour) and max_transients: a source with more onsets
// keeps its first max_transients rather than failing, so a long dense mix
// still warps. Polls `cancelled` between analysis frames and while picking
// peaks. Results are sorted, unique and inside the source.
[[nodiscard]] foundation::Result<std::vector<Transient>>
detect_transients(SourceView& source, const TransientOptions& options,
                  const std::atomic<bool>* cancelled = nullptr);

// Fundamental-frequency track (YIN, de Cheveigné & Kawahara 2002).
struct PitchFrame {
  std::uint64_t frame = 0; // centre of the analysis window
  float frequency = 0.0F;  // 0 when unvoiced
  float clarity = 0.0F;    // 1 - CMNDF minimum (0..1)
};

struct PitchOptions {
  std::uint32_t sample_rate = 48'000;
  float minimum_hz = 60.0F;
  float maximum_hz = 1'000.0F;
  // CMNDF threshold for a voiced decision.
  float threshold = 0.15F;
};

[[nodiscard]] foundation::Result<std::vector<PitchFrame>>
track_pitch(SourceView& source, const PitchOptions& options,
            const std::atomic<bool>* cancelled = nullptr);

// Pitch-synchronous marks (one per period) through voiced regions, placed on
// the waveform peak of each period. `period` is in frames.
struct PitchMark {
  std::uint64_t frame = 0;
  float period = 0.0F;
};

inline constexpr std::size_t max_pitch_marks = 1U << 22;

[[nodiscard]] foundation::Result<std::vector<PitchMark>>
place_pitch_marks(SourceView& source, const std::vector<PitchFrame>& track,
                  std::uint32_t sample_rate);

} // namespace oscillate::warp_dsp::analysis

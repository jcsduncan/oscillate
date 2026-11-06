#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/media/audio_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oscillate::analysis {

struct TempoIdentity {
  std::string source_key;
  std::uint64_t generation = 0;
};

struct TempoOptions {
  double minimum_bpm = 60.0;
  double maximum_bpm = 200.0;
  std::size_t max_envelope_bins = 120'000;
  std::size_t max_suggestions = 20'000;
  std::size_t max_onsets = 20'000;
};

// Disposable source-relative suggestions, never applied to project tempo or
// user-authored markers. Confidence is in [0, 1]; no estimate uses bpm == 0.
struct TempoEstimate {
  TempoIdentity identity;
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;
  double bpm = 0.0;
  double confidence = 0.0;
  std::vector<std::uint64_t> suggested_beat_frames;
  bool suggestions_truncated = false;
  std::vector<std::uint64_t> suggested_onset_frames;
  bool onsets_truncated = false;
};

// Worker-only, bounded in memory and work. A caller must compare source key and
// generation again before publishing or persisting the result. Cancellation and
// decoder errors return no partial estimate.
[[nodiscard]] foundation::Result<TempoEstimate>
analyze_tempo(media::AudioReader& reader, TempoIdentity identity, TempoOptions options = {},
              const std::function<bool()>& cancelled = {});

// An explicit-conform prompt may use this predicate; it never changes a map.
[[nodiscard]] bool conflicts_with_project_tempo(const TempoEstimate& estimate,
                                                double project_bpm) noexcept;

} // namespace oscillate::analysis

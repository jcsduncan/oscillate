#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/media/audio_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oscillate::analysis {

// Control-side identity, normally derived from media::CacheKey and a job generation.
// The caller checks both again before installing a completed result.
struct WaveformIdentity {
  std::string source_key;
  std::uint64_t generation = 0;
};

struct WaveformOptions {
  std::uint64_t minimum_frames_per_bucket = 256;
  std::size_t max_total_buckets = 250'000;
  std::size_t max_levels = 6;
};

struct ChannelSummary {
  float minimum = 0.0F;
  float maximum = 0.0F;
  double rms = 0.0;
};

// Half-open source-frame interval. Mono sources use channels[0] only.
struct WaveformBucket {
  std::uint64_t begin_frame = 0;
  std::uint64_t end_frame = 0;
  std::array<ChannelSummary, 2> channels{};
};

struct WaveformLevel {
  std::uint64_t frames_per_bucket = 0;
  std::vector<WaveformBucket> buckets;
};

struct WaveformSummary {
  WaveformIdentity identity;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint64_t frames = 0;
  std::vector<WaveformLevel> levels;
};

// Worker-only; reads source once with fixed-size scratch. Cancellation is advisory
// and checked between read batches. No partial result is returned on failure.
// Callers own reader lifetime and validate identity/generation at publication.
[[nodiscard]] foundation::Result<WaveformSummary>
analyze_waveform(media::AudioReader& reader, WaveformIdentity identity,
                 WaveformOptions options = {}, const std::function<bool()>& cancelled = {});

} // namespace oscillate::analysis

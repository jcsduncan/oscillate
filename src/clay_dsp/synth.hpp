#pragma once

#include "oscillate/clay_dsp/take.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::clay_dsp::detail {

// Raised-cosine gain that rises over [start - fade/2, start + fade/2] and
// falls over [end - fade/2, end + fade/2]; two notes that meet cross-fade
// with gains summing to one.
[[nodiscard]] double edge_gain(double frame, double start, double end, double fade) noexcept;

// Partials on a hop grid, ready to synthesise: hop j is centred on frame
// first_centre + j * hop. Between hops each partial follows a cubic phase
// (McAulay & Quatieri 1986) through the phases and frequencies at both
// ends, with linear amplitude; before the first and after the last hop it
// holds its edge values.
struct PartialTrack {
  std::int64_t first_centre = 0;
  std::int64_t hop = 0;
  std::size_t hops = 0;
  std::size_t partials = 0;
  std::vector<double> omega; // radians per sample, [hop * partials + h]
  std::vector<double> amplitude;
  std::vector<double> phase;
  double start = 0.0; // fade edges, frames
  double end = 0.0;
  double fade = 0.0;

  [[nodiscard]] std::int64_t first_frame() const noexcept;
  [[nodiscard]] std::int64_t last_frame() const noexcept; // one past
};

// Adds gain * track into out, whose first sample is frame `first`.
void add_track(const PartialTrack& track, std::int64_t first, std::span<float> out, double gain);

// The note as analysed, on its own hops, with measured phases.
[[nodiscard]] PartialTrack original_track(const Note& note, std::int64_t hop,
                                          std::uint32_t sample_rate, double fade);

// A piece of audio played back somewhere else, slower or faster and at
// another pitch, by overlap-added Hann grains (50 % overlap) on a fixed grid
// from out_start. With time and pitch at 1 it reproduces the source.
struct GrainSegment {
  std::vector<float> source;
  double source_at_start = 0.0; // source index that plays at out_start
  double time_rate = 1.0;       // source frames per output frame
  double pitch_rate = 1.0;      // read speed inside a grain
  std::int64_t out_start = 0;
  std::int64_t out_end = 0;
  std::int64_t grain_hop = 1;
  double fade = 0.0;
};

void add_segment(const GrainSegment& segment, std::int64_t first, std::span<float> out,
                 double gain);

} // namespace oscillate::clay_dsp::detail

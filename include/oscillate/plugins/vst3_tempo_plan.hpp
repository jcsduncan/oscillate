#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::plugins::detail {

inline constexpr std::uint64_t maximum_live_sample = std::uint64_t{1} << 40U;

struct TempoBoundary {
  std::uint64_t sample;
  double bpm;
};

// Control-side preparation for VST3 block transport. Multiple events that
// round to one sample use the final event. The callback only reads this plan.
[[nodiscard]] inline foundation::Result<std::vector<TempoBoundary>>
compile_vst3_tempo(const timeline::TempoMap& tempo, std::uint32_t sample_rate) {
  using foundation::ErrorCode;
  using foundation::Result;
  if (tempo.events().empty() ||
      !tempo.sample_to_tick({static_cast<std::int64_t>(maximum_live_sample)}, sample_rate))
    return Result<std::vector<TempoBoundary>>::failure(
        {ErrorCode::unsupported, "VST3 processor needs a bounded tempo map"});
  std::vector<TempoBoundary> boundaries;
  boundaries.reserve(tempo.events().size());
  boundaries.push_back({0, tempo.events().front().bpm});
  for (const auto& event : std::span(tempo.events()).subspan(1)) {
    const auto sample = tempo.tick_to_sample(event.tick, sample_rate);
    if (!sample || sample.value().value < 0 ||
        static_cast<std::uint64_t>(sample.value().value) > maximum_live_sample)
      return Result<std::vector<TempoBoundary>>::failure(
          {ErrorCode::unsupported, "VST3 processor tempo event exceeds the live horizon"});
    const auto boundary = static_cast<std::uint64_t>(sample.value().value);
    if (boundary == boundaries.back().sample)
      boundaries.back().bpm = event.bpm;
    else
      boundaries.push_back({boundary, event.bpm});
  }
  return Result<std::vector<TempoBoundary>>::success(std::move(boundaries));
}

} // namespace oscillate::plugins::detail

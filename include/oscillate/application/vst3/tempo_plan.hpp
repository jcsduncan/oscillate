#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/plugins/vst3_tempo.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::plugins::detail {

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

// The project's tempo map as a VST3 node's musical clock.
class TempoMapClock final : public Vst3TempoClock {
public:
  TempoMapClock(timeline::TempoMap tempo, std::uint32_t sample_rate)
      : tempo_(std::move(tempo)), sample_rate_(sample_rate) {}
  [[nodiscard]] bool quarter_notes_at(std::int64_t sample,
                                      double& quarter_notes) const noexcept override {
    timeline::TickPosition tick{};
    if (!tempo_.sample_to_tick_rt({sample}, sample_rate_, tick))
      return false;
    quarter_notes = static_cast<double>(tick.value) / timeline::ticks_per_quarter;
    return true;
  }
  [[nodiscard]] bool bpm_at(std::int64_t sample, double& bpm) const noexcept override {
    return tempo_.bpm_at_sample_rt({sample}, sample_rate_, bpm);
  }

private:
  timeline::TempoMap tempo_;
  std::uint32_t sample_rate_;
};

// A VST3 node's tempo inputs from the project's tempo map.
[[nodiscard]] inline foundation::Result<Vst3Tempo> make_vst3_tempo(const timeline::TempoMap& tempo,
                                                                   std::uint32_t sample_rate) {
  auto boundaries = compile_vst3_tempo(tempo, sample_rate);
  if (!boundaries)
    return foundation::Result<Vst3Tempo>::failure(boundaries.error());
  const bool ramps = std::ranges::any_of(tempo.events(), [](const auto& event) {
    return event.shape_to_next != timeline::TempoShape::step;
  });
  return foundation::Result<Vst3Tempo>::success(
      Vst3Tempo{std::move(boundaries).value(), ramps,
                std::make_shared<const TempoMapClock>(tempo, sample_rate)});
}

} // namespace oscillate::plugins::detail

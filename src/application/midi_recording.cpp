// MIDI recording (ADR-0086): heard positions, the metronome's beats and the
// count-in.

#include "oscillate/application/midi_recording.hpp"

#include <cmath>
#include <cstdint>

namespace oscillate::application {

namespace {
constexpr double nanoseconds = 1e9;
} // namespace

std::uint64_t heard_sample(audio_io::TransportCursor::AudiblePoint point, std::int64_t now_ns,
                           std::uint32_t sample_rate, std::optional<LoopSamples> loop) noexcept {
  const double offset = static_cast<double>(now_ns - point.heard_ns) * sample_rate / nanoseconds;
  const auto frames = static_cast<std::int64_t>(std::llround(offset));
  std::uint64_t sample = point.sample;
  if (frames < 0)
    sample = static_cast<std::uint64_t>(-frames) > sample
                 ? 0
                 : sample - static_cast<std::uint64_t>(-frames);
  else
    sample += static_cast<std::uint64_t>(frames);
  if (loop && loop->end > loop->start && sample >= loop->end && point.sample < loop->end)
    sample = loop->start + (sample - loop->end) % (loop->end - loop->start);
  return sample;
}

std::vector<audio_io::ClickBeat> click_schedule(const project::Snapshot& snapshot,
                                                std::uint64_t end_sample) {
  std::vector<audio_io::ClickBeat> beats;
  timeline::BarBeat at{1, 1, 0};
  while (beats.size() < audio_io::max_click_beats) {
    auto tick = snapshot.meter.tick_at(at);
    if (!tick) {
      // Past the bar's last beat: the next bar.
      at = {at.bar + 1, 1, 0};
      tick = snapshot.meter.tick_at(at);
      if (!tick)
        break;
    }
    const auto sample = snapshot.tempo.tick_to_sample(tick.value(), snapshot.sample_rate);
    if (!sample || sample.value().value < 0)
      break;
    const auto position = static_cast<std::uint64_t>(sample.value().value);
    if (position >= end_sample)
      break;
    beats.push_back({position, at.beat == 1});
    at.beat = static_cast<std::uint8_t>(at.beat + 1);
  }
  return beats;
}

std::optional<CountIn> count_in_at(const project::Snapshot& snapshot, std::uint64_t sample) {
  const auto tick =
      snapshot.tempo.sample_to_tick({static_cast<std::int64_t>(sample)}, snapshot.sample_rate);
  if (!tick)
    return std::nullopt;
  const auto where = snapshot.meter.locate(tick.value());
  if (!where)
    return std::nullopt;
  const auto first = snapshot.meter.tick_at({where.value().bar, 1, 0});
  const auto second = snapshot.meter.tick_at({where.value().bar, 2, 0});
  const auto next = snapshot.meter.tick_at({where.value().bar + 1, 1, 0});
  if (!first || !next)
    return std::nullopt;
  const auto beat_ticks = second ? second.value().value - first.value().value
                                 : next.value().value - first.value().value;
  if (beat_ticks <= 0)
    return std::nullopt;
  const auto beats = (next.value().value - first.value().value) / beat_ticks;
  // The beat's length in frames at the tempo where the take starts.
  const auto from = snapshot.tempo.tick_to_sample(tick.value(), snapshot.sample_rate);
  const auto to =
      snapshot.tempo.tick_to_sample({tick.value().value + beat_ticks}, snapshot.sample_rate);
  if (!from || !to || to.value().value <= from.value().value)
    return std::nullopt;
  return CountIn{static_cast<std::uint32_t>(beats),
                 static_cast<std::uint64_t>(to.value().value - from.value().value)};
}

} // namespace oscillate::application

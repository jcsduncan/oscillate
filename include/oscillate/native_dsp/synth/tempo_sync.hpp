#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Tempo-synced divisions shared by every native engine. Note divisions are
// lengths in quarter-note beats; bar divisions are whole bars of the
// project's meter at the playhead, so "1 Bar" in 3/4 is three beats.
namespace oscillate::native_dsp::synth {

inline constexpr std::size_t sync_division_count = 17;
inline constexpr std::size_t first_bar_division = 13;

inline constexpr std::array<std::string_view, sync_division_count> sync_divisions{
    "1/32", "1/16T", "1/16", "1/16D", "1/8T",  "1/8",    "1/8D",   "1/4T",  "1/4",
    "1/4D", "1/2T",  "1/2",  "1/2D",  "1 Bar", "2 Bars", "4 Bars", "8 Bars"};

// Quarter-note beats before first_bar_division, bars from it.
inline constexpr std::array<double, sync_division_count> sync_lengths{
    0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0,
    1.5,   4.0 / 3.0, 2.0,  3.0,   1.0,       2.0, 4.0,  8.0};

[[nodiscard]] constexpr bool sync_in_bars(std::size_t division) noexcept {
  return division >= first_bar_division;
}

// Quarter-note beats in one division under the transport's meter.
[[nodiscard]] inline double sync_beats(std::size_t division,
                                       const SynthTransport& transport) noexcept {
  const double length = sync_lengths[division];
  return sync_in_bars(division) ? length * transport.beats_per_bar : length;
}

// Whole and fractional divisions elapsed at the block start: counted from
// bar lines for bar divisions, so an LFO stays locked to the bar grid across
// meter changes.
[[nodiscard]] inline double sync_cycles(std::size_t division,
                                        const SynthTransport& transport) noexcept {
  const double length = sync_lengths[division];
  return sync_in_bars(division) ? transport.bar_at_block_start / length
                                : transport.beat_at_block_start / length;
}

// Quarter-note beats per second: the transport's tempo, or 120 BPM when no
// tempo is known.
[[nodiscard]] inline double sync_beats_per_second(const SynthTransport& transport,
                                                  double rate) noexcept {
  return transport.beats_per_frame > 0.0 ? transport.beats_per_frame * rate : 2.0;
}

[[nodiscard]] inline double sync_seconds(std::size_t division, const SynthTransport& transport,
                                         double rate) noexcept {
  return sync_beats(division, transport) / sync_beats_per_second(transport, rate);
}

[[nodiscard]] inline double sync_hz(std::size_t division, const SynthTransport& transport,
                                    double rate) noexcept {
  return 1.0 / sync_seconds(division, transport, rate);
}

// The meter as bar lines, for the audio callback: a copy of the meter map's
// segments that answers "which bar, and how long is it" by binary search,
// with no allocation and no error path.
class BarGrid {
public:
  BarGrid() = default;
  explicit BarGrid(const timeline::MeterMap& meter);
  // Continuous, zero-based bar position and quarter-note beats per bar at a
  // tick. Before the first segment, 4/4 from tick zero.
  void locate(std::int64_t tick, double& bar, double& beats_per_bar) const noexcept;

private:
  struct Segment {
    std::int64_t tick = 0;
    double ticks_per_bar = 0.0;
    double first_bar = 0.0;
  };
  std::vector<Segment> segments_;
};

} // namespace oscillate::native_dsp::synth

#pragma once

#include "oscillate/audio_io/metronome.hpp"
#include "oscillate/audio_io/stereo_block.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// MIDI recording (ADR-0086): where a key press lands in the song, and the
// metronome's beats. The take itself is written by
// ProjectSession::record_notes().
namespace oscillate::application {

// A note played during a take, in song ticks.
struct RecordedNote {
  timeline::TickPosition start;
  timeline::TickPosition end;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
};

// The transport's loop, [start, end) in project samples.
struct LoopSamples {
  std::uint64_t start = 0;
  std::uint64_t end = 0;
};

// The project sample being heard at CLOCK_MONOTONIC time `now_ns`, from
// the last audible point the device published, wrapped into `loop` when the
// song has run past its end (the device loops there).
[[nodiscard]] std::uint64_t heard_sample(audio_io::TransportCursor::AudiblePoint point,
                                         std::int64_t now_ns, std::uint32_t sample_rate,
                                         std::optional<LoopSamples> loop) noexcept;

// Every beat of the song up to `end_sample` as the metronome's clicks, each
// bar's first beat accented, through the meter and tempo maps; at most
// audio_io::max_click_beats.
[[nodiscard]] std::vector<audio_io::ClickBeat> click_schedule(const project::Snapshot& snapshot,
                                                              std::uint64_t end_sample);

// One bar of count-in before `sample`: the bar's beats and the frames
// between them, at the meter and tempo there.
struct CountIn {
  std::uint32_t beats = 0;
  std::uint64_t beat_frames = 0;
};
[[nodiscard]] std::optional<CountIn> count_in_at(const project::Snapshot& snapshot,
                                                 std::uint64_t sample);

} // namespace oscillate::application

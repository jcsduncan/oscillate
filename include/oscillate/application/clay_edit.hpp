#pragma once

#include "oscillate/clay_dsp/macros.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/persistence/groove_file.hpp"
#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Clay in the song (ADR-0081 phase 2): where a clip's source frames sound,
// and the song-side inputs and outputs of the macros (grids, follow targets,
// MIDI). Pure functions of a snapshot; the session applies their results.
namespace oscillate::application::clay {

// Where an audio clip's source frames sound in the song, through its warp.
class ClipTime {
public:
  [[nodiscard]] static foundation::Result<ClipTime> of(const project::Snapshot& snapshot,
                                                       const project::Clip& clip);

  // Project-rate song frame of a source frame. Frames outside the clip's
  // source range extend its edge segments' rates.
  [[nodiscard]] double song_frame(double source_frame) const noexcept;
  // The inverse.
  [[nodiscard]] double source_frame(double song_frame) const noexcept;
  [[nodiscard]] double song_seconds(double source_frame) const noexcept;
  [[nodiscard]] double source_frame_at_seconds(double seconds) const noexcept;
  // The song tick at a source frame (the tempo map's).
  [[nodiscard]] foundation::Result<timeline::TickPosition> tick(double source_frame) const;

  [[nodiscard]] const project::SourceRecord& source() const noexcept { return source_; }
  [[nodiscard]] std::uint32_t project_rate() const noexcept { return project_rate_; }
  [[nodiscard]] std::int64_t first_source_frame() const noexcept;
  [[nodiscard]] std::int64_t end_source_frame() const noexcept;
  [[nodiscard]] double first_song_seconds() const noexcept;
  [[nodiscard]] double end_song_seconds() const noexcept;

private:
  ClipTime(project::SourceRecord source, processing::ClipFramePlan plan, timeline::TempoMap tempo,
           std::uint32_t project_rate)
      : source_(std::move(source)), plan_(std::move(plan)), tempo_(std::move(tempo)),
        project_rate_(project_rate) {}
  project::SourceRecord source_;
  processing::ClipFramePlan plan_;
  timeline::TempoMap tempo_;
  std::uint32_t project_rate_;
};

// The audio clip and its source record.
struct ClipRef {
  const project::Track* track = nullptr;
  const project::Clip* clip = nullptr;
  const project::SourceRecord* source = nullptr;
};
[[nodiscard]] foundation::Result<ClipRef> find_clip(const project::Snapshot& snapshot,
                                                    project::ClipId clip);

// A fresh take in `mode` for the current analysis, keyed to the source's
// reviewed key, else the song's.
[[nodiscard]] project::ClayTake new_take(const project::Snapshot& snapshot,
                                         const project::Clip& clip, project::ClayMode mode);

// The key a clip's source is in: its reviewed correction or detection.
[[nodiscard]] std::optional<project::MusicalKey> source_key(const project::Snapshot& snapshot,
                                                            project::SourceId source);

// Grid steps per quarter note of a division name ("1/4", "1/8", "1/16",
// "1/32"); nullopt for an unknown name.
[[nodiscard]] std::optional<int> steps_per_quarter(std::string_view division);

// The song's grid in the clip's source frames: steps of `steps_per_quarter`
// from bar 1, pushed by `groove`'s timings when one is given (its own grid
// then sets the step).
[[nodiscard]] foundation::Result<clay_dsp::TimeGrid>
quantize_grid(const project::Snapshot& snapshot, const ClipTime& time, int steps_per_quarter,
              const std::optional<persistence::GrooveTemplate>& groove);

// The notes of an instrument track's MIDI clips that sound under the audio
// clip, in its source frames.
[[nodiscard]] std::vector<clay_dsp::TargetNote>
follow_targets(const project::Snapshot& snapshot, const ClipTime& time, project::TrackId track);

// A MIDI clip of a take's notes for an instrument track: notes in
// clip-local ticks, and the pitch bend as points of the engine's bend
// parameter (-1..1 of its bend range).
struct MidiClipOut {
  timeline::TickPosition start{0};
  timeline::TickPosition length{0};
  std::vector<project::MidiNote> notes;
  struct Bend {
    std::int64_t tick = 0; // clip-local
    double value = 0.0;    // -1..1
  };
  std::vector<Bend> bends;
  // Semitones the bends need (the engine's bend range is raised to it).
  double bend_range = 0.0;
};
[[nodiscard]] foundation::Result<MidiClipOut> midi_clip_out(const ClipTime& time,
                                                            const clay_dsp::MidiOut& midi);

// The engine parameter ids To MIDI writes: its pitch bend and bend range.
inline constexpr std::string_view bend_parameter_suffix = ".perf.bend";
inline constexpr std::string_view bend_range_name = "Bend Range";
inline constexpr std::string_view to_midi_engine = "nova";

} // namespace oscillate::application::clay

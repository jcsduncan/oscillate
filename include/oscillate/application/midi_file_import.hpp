#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Standard MIDI File import (ADR-0045): a parsed file becomes project
// operations — instrument tracks, MIDI clips with exact ticks, pedal lanes
// and optionally the file's tempo and meter — applied as one undo step.
namespace oscillate::application {

struct MidiImportOptions {
  enum class Target : std::uint8_t {
    // One instrument track per MIDI track and channel that plays notes or
    // pedals; channel 10 plays the built-in drum synth.
    new_tracks,
    // Every note and pedal into `track`, an existing instrument track.
    track,
  };
  Target target = Target::new_tracks;
  std::optional<project::TrackId> track;
  // Replace the project's tempo and meter from the import position on with
  // the file's. Ignored for SMPTE-timed files, whose tempo events do not
  // time their notes.
  bool import_tempo_and_meter = true;
  // Zero-based bar where the file's tick zero lands.
  std::int64_t zero_based_bar = 0;
};

struct MidiImportResult {
  std::vector<project::TrackId> tracks; // Created or filled, in order.
  std::size_t clips = 0;
  std::size_t notes = 0;
  std::size_t pedal_points = 0;
  bool tempo_imported = false;
  bool meter_imported = false;
  // What was repaired or left out, for the user.
  std::vector<std::string> notices;
};

// The next numbers the session would hand out; the plan advances them.
struct MidiImportIds {
  std::uint64_t next_track = 1;
  std::uint64_t next_clip = 1;
  std::uint64_t next_note = 1;
};

struct MidiImportPlan {
  std::vector<project::Operation> operations;
  MidiImportResult result;
};

// Pure: no session state beyond `snapshot` and `ids`. Musical files convert
// file ticks exactly (a rational rescale of absolute positions, so nothing
// drifts); SMPTE files place notes by elapsed time on the project's tempo.
[[nodiscard]] foundation::Result<MidiImportPlan> plan_midi_import(const project::Snapshot& snapshot,
                                                                  const midi_file::File& file,
                                                                  const MidiImportOptions& options,
                                                                  MidiImportIds ids);

} // namespace oscillate::application

#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace oscillate::application {

// Pure control-side algebra for a clip's reviewed warp markers. Every
// function computes a complete replacement ClipProcessing; the caller applies
// it with ProjectSession::set_audio_clip_processing, so each gesture is one
// validated, atomic, undoable revision. Positions are project-rate output
// frames local to the clip, resolved through the authoritative clip frame
// plan (the same map playback and export use).

struct WarpMarkerPosition {
  std::uint64_t source_frame = 0; // absolute, at the source's own rate
  std::uint64_t output_frame = 0; // clip-local project-rate frame
};

struct ClipWarpLayout {
  std::uint64_t project_start = 0; // project frame of output frame 0
  std::uint64_t output_frames = 0; // rendered clip length
  std::uint64_t source_first = 0;  // absolute source frame at output 0
  std::uint64_t source_frames = 0; // source-rate frames in the trimmed range
  std::uint32_t source_rate = 0;
  std::uint32_t project_rate = 0;
  bool musical = false; // markers use ticks, not samples
  std::vector<WarpMarkerPosition> markers;
};

[[nodiscard]] foundation::Result<ClipWarpLayout> clip_warp_layout(const project::Snapshot& snapshot,
                                                                  project::ClipId clip);

// Adds a marker at `output_frame`, pinned to the source frame that currently
// plays there, so the sound does not change until the marker is moved.
[[nodiscard]] foundation::Result<project::ClipProcessing>
add_warp_marker(const project::Snapshot& snapshot, project::ClipId clip,
                std::uint64_t output_frame);

// Moves one marker to a new output frame, keeping its source frame and every
// other marker; the segments on both sides stretch. Invalid order, bounds or
// collisions are rejected by project validation when applied.
struct WarpMarkerMove {
  std::size_t index = 0;
  std::uint64_t output_frame = 0;
};
[[nodiscard]] foundation::Result<project::ClipProcessing>
move_warp_marker(const project::Snapshot& snapshot, project::ClipId clip, WarpMarkerMove move);

[[nodiscard]] foundation::Result<project::ClipProcessing>
remove_warp_marker(const project::Snapshot& snapshot, project::ClipId clip, std::size_t index);

// Applies reviewed suggestions (absolute source frames, e.g. detected
// transients) atomically: each becomes a marker at its current output
// position, so the sound is unchanged. Suggestions outside the trimmed range,
// at an existing marker's source frame, or within `minimum_gap` source frames
// of a kept marker are skipped. Fails if nothing would be added.
[[nodiscard]] foundation::Result<project::ClipProcessing>
apply_warp_suggestions(const project::Snapshot& snapshot, project::ClipId clip,
                       std::span<const std::uint64_t> source_frames, std::uint64_t minimum_gap = 1);

// Worker-only, bounded: detects transients across the whole verified managed
// source (through the same project-rate view and detector the warp engines
// use) and returns them as ascending absolute source-rate frames, at most
// project::max_tempo_markers_per_source. The caller stores them as the
// source's generated onset review (ProjectSession::accept_source_onset_analysis,
// checked against the project revision captured before analysis); nothing
// here changes the project. `cancelled` is polled between analysis frames.
struct SourceOnsets {
  std::vector<std::uint64_t> frames;
  bool truncated = false;
};
[[nodiscard]] foundation::Result<SourceOnsets>
analyse_source_onsets(const std::filesystem::path& project_root, const project::Snapshot& snapshot,
                      project::SourceId source, const std::atomic<bool>* cancelled = nullptr);

// The clip's reviewable onset suggestions: the source's durable generated
// onsets inside the clip's trimmed range, minus any within `minimum_gap`
// source frames of a reviewed marker (dismissed onsets are already absent
// from the review). Empty when the source has no onset review.
[[nodiscard]] std::vector<std::uint64_t> clip_onset_suggestions(const project::Snapshot& snapshot,
                                                                project::ClipId clip,
                                                                std::uint64_t minimum_gap);

// Converts a frame of the project-rate source view (where analysis runs) to
// the nearest absolute source-rate frame.
[[nodiscard]] std::uint64_t source_frame_from_view(std::uint64_t view_frame,
                                                   std::uint32_t source_rate,
                                                   std::uint32_t project_rate) noexcept;

// --- Split, slip and trim of processed clips -----------------------------------
//
// A processed clip's audio is its warp map: clip start, reviewed markers and
// clip end pin source frames to positions on the clip's own timeline, linear
// in project samples between them, and extended beyond the clip by the slope
// of its first and last segment. Split, slip and trim re-window that map; the
// markers stay glued to their audio (as in Ableton Live):
// - a window edge between markers gets the source frame the map plays there,
//   rounded to the nearest frame (exact when the edge is a marker);
// - markers strictly inside the new window keep their source frame and
//   timeline position; markers outside it are left out of the result and
//   counted, so the caller can refuse (trim and slip never drop reviewed
//   markers) or hand them to the other half (split);
// - mode, transpose, formants and remembered settings are kept.
//
// Positions are relative to the clip's placement start in its timebase:
// ticks for a musical clip, project samples for an elapsed clip.
struct ProcessedClipWindow {
  project::Placement placement;
  project::SourceRange source_range;
  project::ClipProcessing processing;
  std::size_t removed_markers = 0; // reviewed markers outside (or on an edge of) the window
};

// The clip re-windowed to [first, last). With `keep_placement` (slip) the
// clip stays where it is on the timeline and the audio moves under it, so
// last - first must equal the placement length; otherwise the placement
// becomes [start + first, start + last). Fails if the window leaves the
// source audio, is empty, or a marker would collide with a new edge.
[[nodiscard]] foundation::Result<ProcessedClipWindow>
rewindow_processed_clip(const project::Snapshot& snapshot, project::ClipId clip, std::int64_t first,
                        std::int64_t last, bool keep_placement);

// The slip, in the clip's timebase, that moves the source frame playing at
// the clip start by `source_frame_delta` (through the extended warp map,
// rounded to the nearest tick or sample).
[[nodiscard]] foundation::Result<std::int64_t>
processed_slip_offset(const project::Snapshot& snapshot, project::ClipId clip,
                      std::int64_t source_frame_delta);

// --- Auto-warp and tempo conform ----------------------------------------------
//
// Both read the source's durable tempo review (BPM: the user correction, else
// the detection; beats: user and generated beat frames) and need a musical
// clip. The result is a reviewable proposal; applying it is one undoable
// edit (ProjectSession::apply_audio_clip_warp) of placement and processing.
struct ClipWarpProposal {
  project::Placement placement;
  project::ClipProcessing processing;
  std::uint64_t settings_generation = 0; // of the clip the proposal was made for
  std::size_t beat_markers = 0;          // markers placed on reviewed beats
  std::size_t skipped_beats = 0;         // beats that would cross a reviewed marker
  double source_bpm = 0.0;
};

// Auto-warp: every reviewed beat inside the clip becomes a marker on the
// project's quarter-note grid. The first beat snaps to the nearest grid beat
// from where it plays now; each later beat lands one quarter note after the
// previous. The clip end then follows the last beat interval, unless a
// reviewed marker lies after the last beat (its timing is kept, and so is the
// clip length). Existing reviewed markers are never moved or removed; a beat
// within 30 ms of one, or one whose grid position would cross one, is skipped.
[[nodiscard]] foundation::Result<ClipWarpProposal>
propose_auto_warp(const project::Snapshot& snapshot, project::ClipId clip);

// Tempo conform: a uniform stretch so the source's reviewed BPM plays at the
// project tempo map. The clip keeps its start; its length becomes the source
// range's beats at that BPM, and reviewed marker offsets scale with it.
[[nodiscard]] foundation::Result<ClipWarpProposal>
propose_tempo_conform(const project::Snapshot& snapshot, project::ClipId clip);

// --- Clip tempo (the clip strip, ADR-0053) ------------------------------------
//
// A source's beat grid is its reviewed BPM (the correction, else the
// detection) from its downbeat, every source_beats_per_bar beats a bar. The
// downbeat is the user's; else, for a loop (a source within
// loop_bar_tolerance_beats of a whole number of bars), its first frame; else
// the analysis grid's first beat. A loop is cut on its bar lines, while the
// beat tracker may lock onto off-beats (a top loop's hats). Follow plays the
// clip on the project's bars: it becomes musical, its length the source
// range's beats at that BPM (reviewed marker offsets scale with it), so tempo
// changes and ramps carry it. Own tempo plays it unstretched in real time
// from where it starts now.
inline constexpr double source_beats_per_bar = 4.0;
// A 128th note: covers a loop's sample rounding and a detected BPM a little
// off the true one, while a whole song seldom ends this close to a bar line.
inline constexpr double loop_bar_tolerance_beats = 1.0 / 32.0;
struct SourceBeatGrid {
  double bpm = 0.0;
  std::uint32_t sample_rate = 0;
  std::uint64_t downbeat = 0; // source frame of a bar's first beat
  [[nodiscard]] double period() const noexcept { return 60.0 * sample_rate / bpm; }
};
[[nodiscard]] std::optional<SourceBeatGrid> source_beat_grid(const project::Snapshot& snapshot,
                                                             project::SourceId source);

// Follow on: the clip conforms to `bpm` (the source's reviewed BPM when
// absent) and moves so the first downbeat inside it lands on `downbeat_tick`,
// or, when that is absent, on the bar nearest where it plays now. The
// placement may start before zero; the session then moves it later by whole
// bars (bars before bar 1 are only ever added by moving a clip there).
[[nodiscard]] foundation::Result<ClipWarpProposal>
propose_follow(const project::Snapshot& snapshot, project::ClipId clip,
               std::optional<double> bpm = std::nullopt,
               std::optional<std::uint64_t> downbeat = std::nullopt,
               std::optional<std::int64_t> downbeat_tick = std::nullopt);
// Follow off: elapsed and unstretched (its source range at the project
// rate), starting where it starts now; reviewed markers keep their place in
// the clip proportionally.
[[nodiscard]] foundation::Result<ClipWarpProposal>
propose_own_tempo(const project::Snapshot& snapshot, project::ClipId clip);
// The absolute project tick where the clip plays its first downbeat now
// (for keeping it in place across a tempo correction).
[[nodiscard]] foundation::Result<std::int64_t> clip_downbeat_tick(const project::Snapshot& snapshot,
                                                                  project::ClipId clip);

// The absolute source frame the clip plays at `fraction` (0..1) of its
// length, and the reverse, through its warp map: how the arrangement places
// the downbeat flag and beat lines on the clip.
[[nodiscard]] foundation::Result<std::uint64_t>
source_frame_at_fraction(const project::Snapshot& snapshot, project::ClipId clip, double fraction);
[[nodiscard]] foundation::Result<double> fraction_of_source_frame(const project::Snapshot& snapshot,
                                                                  project::ClipId clip,
                                                                  std::uint64_t source_frame);

// Splits a pitch into the nearest whole semitone and the remaining cents
// (-50..50), and rebuilds it; the UI edits these two parts.
struct TransposeParts {
  int semitones = 0;
  int cents = 0;
};
[[nodiscard]] TransposeParts split_transpose(double semitones) noexcept;
[[nodiscard]] double join_transpose(int semitones, int cents) noexcept;

} // namespace oscillate::application

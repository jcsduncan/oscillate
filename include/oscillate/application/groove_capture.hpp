#pragma once

#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/persistence/groove_file.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::persistence {
class ProjectStore;
}

// Groove from a record (ADR-0079, docs/ui/creative-tools-mockup.html#groove):
// the feel of a part of a real record, as a groove template for the pool.
// The hits of one band (kick, percussion, hats or everything) are found in
// the audio, placed on the record's beat grid, and each step of a one- or
// two-bar cycle takes the mean of its hits' offsets and levels over 4 to 32
// bars. The measurement also says how sure it is, how much it swings, where
// the offbeats sit and which steps are accented, in plain words.
//
// detect_onsets, fit_grid, measure, describe and audition are pure and
// deterministic. capture_audio decodes a library file, capture_clip reads a
// project clip's audio: both are worker-only.
namespace oscillate::application::groove_capture {

enum class Band : std::uint8_t { all, kick, perc, hats };
inline constexpr std::size_t band_count = 4;
inline constexpr std::array<std::string_view, band_count> band_names{"All", "Kick", "Perc", "Hats"};

// The bands' edges in Hz (each filter is two cascaded Butterworth sections).
inline constexpr double rumble_cutoff_hz = 30.0; // All: below this is no hit.
inline constexpr double kick_cutoff_hz = 150.0;
inline constexpr double perc_low_hz = 300.0;
inline constexpr double perc_high_hz = 3000.0;
inline constexpr double hats_cutoff_hz = 6000.0;
// Each band's detector delay in ms: how late detect_onsets would place a
// hit's start through that band's filters and start threshold, measured on
// the design (a 130-to-50 Hz kick for Kick and All, whose downbeat hits are
// a kick's; the mean over 40 bright 12 ms noise bursts for Hats and 40
// 20 ms noise bursts for Perc). detect_onsets takes it off every hit, so
// hits of different bands line up with each other.
inline constexpr std::array<double, band_count> band_latency_ms{0.8, 3.5, 0.6, 0.1};

inline constexpr std::size_t min_bars = 4;
inline constexpr std::size_t max_bars = 32;
inline constexpr std::size_t default_bars = 8;
// A 4/4 bar.
inline constexpr std::size_t beats_per_bar = 4;
// The tempos a grid is fitted between when the record's tempo is unknown.
inline constexpr double min_fit_bpm = 85.0;
inline constexpr double max_fit_bpm = 175.0;
// A step is accented when its level is this far over the mean of all hits.
inline constexpr double accent_ratio = 1.15;
// A step's level is clamped to [min_step_velocity, max_groove_velocity].
inline constexpr double min_step_velocity = 0.25;
// Folded hits kept for the fingerprint view.
inline constexpr std::size_t max_folded_hits = 2048;
// Waveform points of the fingerprint's bar.
inline constexpr std::size_t bar_peak_points = 512;
// How much audio a library file gives at most: the longest window at the
// slowest fitted tempo, plus a bar of lead-in.
inline constexpr double max_capture_seconds = (max_bars + 1) * beats_per_bar * 60.0 / min_fit_bpm;

// A hit of one band: where it starts (a fractional frame of the analysed
// audio) and how loud it is (the band's peak envelope after it, linear).
struct Onset {
  double frame = 0.0;
  double strength = 0.0;
};

// The hits of `band` in `mono` at `rate`, in time order, less the band's
// detector delay (band_latency_ms).
[[nodiscard]] std::vector<Onset> detect_onsets(std::span<const float> mono, std::uint32_t rate,
                                               Band band);

// A straight grid fitted to hits: the tempo, the frame of one downbeat and
// how well the hits sit on its quarters (0 none, 1 every hit exactly on).
struct Grid {
  double bpm = 0.0;
  double downbeat_frame = 0.0;
  double fit = 0.0;
};
// Fits a grid to `beats` (normally the kick's hits): the given tempo, else
// the one between min_fit_bpm and max_fit_bpm whose quarters the hits sit
// on best (the slower of two that fit equally). The downbeat is the beat
// whose hits are strongest, the first at or after `from_frame`.
[[nodiscard]] foundation::Result<Grid> fit_grid(std::span<const Onset> beats, std::uint32_t rate,
                                                std::optional<double> bpm, double from_frame = 0.0);

struct MeasureSettings {
  std::string name;
  // 2 (8ths), 4 (16ths) or 8 (32nds).
  std::uint32_t steps_per_quarter = 4;
  // 16 or 32 steps: one or two bars of 16ths, or one bar of 32nds.
  std::size_t cycle_steps = 16;
  // Bars measured, for the confidence's hit count.
  std::size_t bars = default_bars;
  // One step's length in milliseconds at the record's tempo.
  double step_ms = 0.0;
  // Ticks taken off every hit before it is measured: the kick's
  // (downbeat_shift of the kick's hits), so the groove is each part's feel
  // against the kick, and a clip's or record's own offset cancels. 0 for
  // hits already placed against the grid by hand.
  std::int64_t shift = 0;
};

// The mean offset in ticks of the hits nearest the downbeats of a 4/4 bar
// on a grid of `steps_per_quarter`, else of those nearest the beats; none
// without either. Measured on the kick's hits, it is the reference every
// band's groove is measured against.
[[nodiscard]] std::optional<std::int64_t> downbeat_shift(std::span<const GrooveHit> hits,
                                                         std::uint32_t steps_per_quarter);

// One step of the measured cycle.
struct StepFeel {
  double timing = 0.0;    // Mean offset, a share of a step (+ late).
  double timing_ms = 0.0; // The same in milliseconds.
  double spread = 0.0;    // Standard deviation of the offsets, a share of a step.
  double spread_ms = 0.0;
  double velocity = 1.0; // Mean level against the mean of all hits.
  std::size_t hits = 0;
};

// A hit folded into the cycle: its step plus its offset, and its strength.
struct FoldedHit {
  double position = 0.0;
  double strength = 0.0;
};

struct Measurement {
  persistence::GrooveTemplate groove;
  std::vector<StepFeel> steps;
  std::vector<FoldedHit> hits; // At most max_folded_hits, in time order.
  std::size_t hit_count = 0;
  // 0..1: enough hits for the bars, and each step's hits close together.
  double confidence = 0.0;
  // The grid's pairs: 50 % straight, 66.7 % triplet swing.
  double swing_percent = 50.0;
  // The offbeat 8ths' mean offset in ms (+ late, - pushed).
  double offbeat_ms = 0.0;
  std::size_t accents = 0;
  double step_ms = 0.0;
};

// Hits on the song's timeline (ticks, any strength) to a groove. Steps
// without hits stay straight at level 1. Needs at least four hits.
[[nodiscard]] foundation::Result<Measurement> measure(std::span<const GrooveHit> hits,
                                                      const MeasureSettings& settings);

// "Swung 16ths at 62 %, offbeats 4 ms late, 3 accents."
[[nodiscard]] std::string describe(const Measurement& measurement);

// What to take a groove from, and how.
struct Request {
  std::string name;
  Band band = Band::hats;
  std::size_t bars = default_bars;
  std::uint32_t steps_per_quarter = 4;
  std::size_t cycle_steps = 16;
  // Library files only: the tempo if known (a tag, the name, or the user's).
  std::optional<double> bpm;
  // Library files only: where to start looking (the first downbeat after).
  double start_seconds = 0.0;
};

struct Capture {
  Measurement measurement;
  double bpm = 0.0;
  double grid_fit = 0.0;
  // The first measured bar: its waveform's peaks (0..1, bar_peak_points
  // of them) and its hits' places in it (0..1).
  std::vector<float> bar_peaks;
  std::vector<double> bar_onsets;
};

// A record with no song grid (a library file decoded to mono): fits a grid
// to its kick, then measures `request.band` over `request.bars` bars from
// the first downbeat at or after `request.start_seconds`, against the
// kick's downbeats (else All's, else the band's own).
[[nodiscard]] foundation::Result<Capture> capture_audio(std::span<const float> mono,
                                                        std::uint32_t rate, const Request& request);

// A library file, decoded (WAV, or with the codec adapter AIFF, FLAC and
// MP3) from `request.start_seconds` for at most max_capture_seconds.
// Worker-only; `cancelled` is polled between blocks.
[[nodiscard]] foundation::Result<Capture> capture_file(const std::filesystem::path& file,
                                                       const Request& request,
                                                       const std::function<bool()>& cancelled = {});

// An audio clip in the project: its source is read through `store`, its
// hits placed on the song's timeline through the clip's warp, and the bars
// counted from the first bar line at or after the clip's start. The song's
// grid is used, so `request.bpm` and `request.start_seconds` are ignored;
// the band is measured against the clip's kick as for a record.
// Worker-only.
[[nodiscard]] foundation::Result<Capture> capture_clip(const persistence::ProjectStore& store,
                                                       const project::Snapshot& snapshot,
                                                       project::ClipId clip, const Request& request,
                                                       const std::function<bool()>& cancelled = {});

// A/B audition: `bars` bars of a kick on the beat and hats on every step of
// `groove`'s grid at `bpm`, straight (grooved false) or played through the
// groove's timing and levels. Interleaved stereo at `rate`, whole bars long.
[[nodiscard]] std::vector<float> audition(const persistence::GrooveTemplate& groove, double bpm,
                                          std::uint32_t rate, std::size_t bars, bool grooved);

} // namespace oscillate::application::groove_capture

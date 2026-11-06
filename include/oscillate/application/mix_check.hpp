#pragma once

#include "oscillate/analysis/mix_check.hpp"
#include "oscillate/application/project_session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Mix Check (ADR-0074): one button renders the song once offline, measures
// the master and every audible track's output as it renders
// (analysis/mix_check.hpp), lists what pokes out in plain words with the
// track(s) to blame, and offers one fix per finding that applies as one
// undo step. It never changes anything by itself.
//
// measure() runs on a worker; report() and the fixes on the control thread.
namespace oscillate::application::mix_check {

// ---- Loudness targets ----------------------------------------------------------------

struct Target {
  std::string_view id;
  std::string_view name;
  std::string_view note;
  double lufs_low;
  double lufs_high;
  double true_peak; // dBTP ceiling.
  double lra_low;
  double lra_high;
  double crest_low;
  double crest_high;
};
inline constexpr std::array<Target, 3> targets{{
    {"club", "Club master",
     "Loud and dense, but keep 0.3 dB of true-peak room so the system's limiter does not clip.",
     -9.0, -7.0, -0.3, 4.0, 8.0, 8.0, 12.0},
    {"stream", "Streaming",
     "Services turn loud masters down to about -14 LUFS: pushing harder only loses punch.", -15.0,
     -13.0, -1.0, 5.0, 10.0, 9.0, 14.0},
    {"dj", "Into a DJ mix",
     "Leaves headroom for the mixer and the next track's EQ; the DJ's gain does the rest.", -12.0,
     -9.0, -2.0, 4.0, 9.0, 9.0, 14.0},
}};
inline constexpr std::string_view default_target = "club";
// The target with this id (the default one for an unknown id).
[[nodiscard]] const Target& target(std::string_view id) noexcept;
// A value within this of its target range reads "warn", further "bad".
inline constexpr double target_near = 1.0;
// A master limiter's ceiling is set this far under the true-peak target:
// the limiter holds sample peaks, and the peaks between samples run over.
// An existing ceiling is only ever lowered: to min(current, target - what
// the master measured over it) - this margin, so the measured overshoot is
// taken off whether it comes from peaks between samples or from gain after
// the limiter.
inline constexpr double ceiling_true_peak_margin_db = 0.5;
// The new ceiling (dB) for a master limiter at `current` (none: a new one)
// that let the master's true peak reach `true_peak` against `target`. Never
// above `current`.
[[nodiscard]] double lowered_ceiling_db(std::optional<double> current, double target,
                                        double true_peak) noexcept;

// ---- Fix rules -----------------------------------------------------------------------

// Rumble: a 24 dB/oct Contour high-pass first on the track, this far under
// its lowest fundamental (the sub oscillator's, when it has one): never at
// or above it. When the filter would take the rumble's centre down less
// than contour_min_cut_db, the gap under the fundamental is too small to
// cut without touching it: the finding stands without a fix.
inline constexpr double contour_cutoff_ratio = 0.8;
inline constexpr double contour_min_cut_db = 3.0;
inline constexpr double contour_high_pass_24 = 4.0; // contour.model "HP 24".
// Width: Vista's band covering the loss is narrowed until the track's own
// fold loses at most this much there, never under vista_min_width and
// never wider than 1 (net, with Vista's overall width).
inline constexpr double vista_target_loss_db = 1.0;
inline constexpr double vista_min_width = 0.3;
// Ringing: Temper last, suppressor off, one narrow dynamic cut per peak
// (at most analysis::mix::resonance_max_peaks, the longest-ringing).
inline constexpr double temper_cut_q = 8.0;
inline constexpr double temper_cut_range_db = -9.0;
inline constexpr double temper_cut_threshold_db = -80.0;
inline constexpr double temper_cut_ratio = 20.0;
// Masking: one wide dynamic cut on the less important track, its depth the
// measured loss (at most masking_max_cut_db).
inline constexpr double masking_cut_q = 1.2;
inline constexpr double masking_max_cut_db = 6.0;
// Clicks: fades this long on the offending edges.
inline constexpr double click_fade_seconds = 0.004;
// Re-check: everything outside a fix's band should stay within this.
inline constexpr double unchanged_db = 0.1;
// The Clean-up checks: a track or the master over full scale clips.
inline constexpr double clip_level = 1.0;
// A track is a bass when this share of its power sits under the lock
// crossover.
inline constexpr double bass_low_share = 0.5;
// Drums: the engines that make them; an audio track counts when its crest
// is at least this.
inline constexpr std::array<std::string_view, 4> drum_engines{"anvil", "knock", "crate", "pulse"};
inline constexpr double transient_crest_db = 14.0;

// ---- Findings ------------------------------------------------------------------------

enum class Kind : std::uint8_t {
  sub,
  mono,
  mono_lows,
  ringing,
  kick_bass,
  loudness,
  click,
  energy,
  masking,
  clipping,
  balance,
  dc,
  silent,
};
[[nodiscard]] std::string_view kind_name(Kind kind) noexcept;

struct TrackRef {
  project::TrackId id;
  std::string name;
  std::optional<std::uint32_t> color;
};
struct Band {
  double low = 0.0;
  double high = 0.0;
  std::string label;
};
struct Evidence {
  std::string kind; // "spectrum", "time" or "mono".
  std::string caption;
  std::vector<double> x;
  std::vector<double> before;
  std::vector<double> after;
  std::vector<double> markers;
};

struct EffectFix {
  std::vector<EffectEdit> edits;
};
struct FadeFix {
  std::vector<std::pair<project::ClipId, project::ClipFades>> fades;
};
struct LockFix {
  project::TrackId bass;
};
using FixAction = std::variant<EffectFix, FadeFix, LockFix>;
struct Fix {
  std::string device;
  std::string where;
  std::vector<std::pair<std::string, std::string>> params;
  FixAction action;
};

struct Finding {
  // Stable across re-checks of the same problem: kind, track and band.
  std::string id;
  Kind kind = Kind::sub;
  bool bad = true; // "bad", else "warn".
  std::string title;
  std::string text;
  std::vector<TrackRef> tracks;
  std::optional<Band> band;
  Evidence evidence;
  std::optional<Fix> fix;
  std::string expected;
  // The measured amount (dB, LU or a count), for re-checks and history.
  double metric = 0.0;
  std::string metric_label;
  // Where it is worst, for Listen (project frame).
  std::uint64_t worst_frame = 0;
  bool applied = false;
  std::string result;
};

struct Meter {
  std::string id;
  std::string label;
  std::string value;
  std::string note;
  std::string state; // "ok", "warn" or "bad".
};

struct BarEnergy {
  std::size_t bar = 0; // One-based, as the ruler counts.
  double lufs = analysis::mix::silence_db;
};

struct LoudnessSummary {
  double integrated = analysis::mix::silence_db;
  double true_peak = analysis::mix::silence_db;
  double range = 0.0;
  double crest = 0.0;
};

// ---- Measurements --------------------------------------------------------------------

// One track's measurements: its output as it fed the mix (after its
// inserts, fader, pan and automation), from project frame 0.
struct TrackMeasure {
  project::Track track;
  analysis::mix::Profile profile{};
  bool drum = false;
  bool bass = false;
  bool kick = false;
  // The lowest fundamental it sounds (0 when unknown): its lowest played
  // note moved by its oscillators', sub's and note effects' octaves and
  // transposes, or lower when its own spectrum confirms a lower peak on
  // those notes (a sub an octave or two down).
  double lowest_fundamental_hz = 0.0;
  std::vector<double> played_hz{};   // Pitches sounded, for ringing (empty for drums).
  std::vector<double> hit_seconds{}; // Kick hits.
  // Each unfaded clip edge that clicks: clip, which edge, project frame, dB.
  struct Click {
    project::ClipId clip;
    bool start = true;
    std::uint64_t frame = 0;
    double ratio_db = 0.0;
    std::vector<double> samples; // Around the edge, for the evidence.
  };
  std::vector<Click> clicks{};
};

// A kick against another track that is not a drum, under the kick's hits.
struct KickPair {
  project::TrackId kick;
  project::TrackId other;
  analysis::mix::Cancellation cancellation;
};

struct Measurements {
  std::shared_ptr<const project::Snapshot> snapshot;
  std::string project_name;
  analysis::mix::Profile master;
  analysis::mix::Loudness loudness;
  double drum_crest_db = 0.0;
  bool have_drums = false;
  std::map<project::TrackId, TrackMeasure> tracks;
  std::vector<project::TrackId> order; // Arrangement order.
  std::vector<BarEnergy> bars;
  std::vector<KickPair> kick_pairs;
};

// Called with (fraction 0..1, stage text); return false to cancel.
using Progress = std::function<bool(double, const std::string&)>;

// Worker only. Renders `pinned` once (exporting::render_master_taps) and
// measures the master and every audible track as the song renders, all
// streamed: nothing is written to disk and no song-length signal is kept.
// One pass costs about one offline render, so a re-check simply measures
// everything again (reusing unchanged tracks would save no rendering).
[[nodiscard]] foundation::Result<Measurements> measure(const PinnedSnapshot& pinned,
                                                       const Progress& progress);

// ---- Report --------------------------------------------------------------------------

struct Report {
  std::uint64_t revision = 0;
  std::string project_name;
  std::size_t track_count = 0;
  double duration_seconds = 0.0;
  std::vector<double> freqs;
  std::vector<double> spectrum;
  std::vector<double> mono;
  analysis::mix::BandPowers bands{};
  std::vector<Meter> meters;
  LoudnessSummary loudness;
  std::vector<BarEnergy> energy;
  std::vector<analysis::mix::Section> sections;
  std::string energy_note;
  std::vector<Finding> findings;
  std::vector<std::string> fine;
};

// Control thread. Findings for `target` from measurements of a snapshot;
// fixes refer to that snapshot's slots and clips.
[[nodiscard]] Report report(const Measurements& measured, std::string_view target_id);

// True when two snapshots sound alike for a check: the same tracks,
// devices, clips, routing, master and timing, whatever is soloed (Listen
// solos) and whatever only presents (names, colours, markers aside).
[[nodiscard]] bool same_mix(const project::Snapshot& a, const project::Snapshot& b);

// One undo step. Refused when the mix has changed since `measured` (the
// slots and clips the fix names may be gone).
[[nodiscard]] foundation::Result<void> apply(ProjectSession& session, const Finding& finding,
                                             const project::Snapshot& measured);

// After a re-check: what became of `finding`, given the reports before and
// after. Fills applied/result and the evidence's after series.
void judge(Finding& finding, const Report& before, const Report& after, const Measurements& now);

// ---- History -------------------------------------------------------------------------

// Each check's summary, kept in the project's disposable cache directory
// (never in its saved state): the previous one is diffed against.
struct HistoryFinding {
  std::string id;
  std::string title;
  double metric = 0.0;
  std::string label;
};
struct History {
  std::int64_t checked_at = 0; // Unix seconds.
  LoudnessSummary loudness;
  std::vector<HistoryFinding> findings;
};
[[nodiscard]] History summarise(const Report& report, std::int64_t checked_at);
struct Change {
  std::string direction; // "fixed", "better", "worse", "new" or "same".
  std::string text;
};
[[nodiscard]] std::vector<Change> since_last(const History& previous, const History& current,
                                             std::string_view target_id);
// The cache directory's file. Missing or unreadable history is no history.
[[nodiscard]] std::optional<History> load_history(const persistence::ProjectStore& store);
[[nodiscard]] foundation::Result<void> save_history(const persistence::ProjectStore& store,
                                                    const History& history);
[[nodiscard]] std::string to_json(const History& history);
[[nodiscard]] std::optional<History> from_json(std::string_view text);

// ---- Reference -----------------------------------------------------------------------

struct ReferenceTrack {
  std::string name;
  analysis::mix::Profile profile;
  double lufs = analysis::mix::silence_db;
};
// Worker only: decodes a WAV (or, in codec builds, AIFF, FLAC or MP3) of at
// most reference_max_seconds and measures it.
inline constexpr double reference_max_seconds = 20.0 * 60.0;
[[nodiscard]] foundation::Result<ReferenceTrack>
measure_reference(const std::filesystem::path& path, const std::function<bool()>& cancelled);

} // namespace oscillate::application::mix_check

#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/tonal_analysis.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The master assistant (ADR-0124): measures a mix, compares it with a
// reference and a loudness target, and proposes a master chain of the
// project's own devices with concrete settings, each with one plain line
// saying why. It also runs a chain offline over the mix, so a proposal's
// limiter gain is learned on the audio it will play and its outcome is
// measured, not guessed. Pure analysis and planning on stereo float audio:
// no project model and no threads of its own. Worker threads only; nothing
// here runs on the audio callback.
namespace oscillate::native_dsp::assistant {

// ---- Goals ---------------------------------------------------------------------------

enum class Character : std::uint8_t { clean, punchy, warm };
inline constexpr std::size_t character_count = 3;
inline constexpr std::array<std::string_view, character_count> character_names{"Clean", "Punchy",
                                                                               "Warm"};

// The loudness targets offered by name; any other is custom.
struct LoudnessPreset {
  std::string_view id;
  std::string_view name;
  double lufs;
  double ceiling_dbtp;
};
inline constexpr std::array<LoudnessPreset, 3> loudness_presets{{
    {"streaming", "Streaming", -14.0, -1.0},
    {"club", "Club", -9.0, -0.3},
    {"loud", "Loud", -7.0, -0.3},
}};
// A custom target's range: what Ceiling's Target and Ceiling reach.
inline constexpr double custom_lufs_min = -24.0;
inline constexpr double custom_lufs_max = -5.0;
inline constexpr double custom_ceiling_min_dbtp = -6.0;
inline constexpr double custom_ceiling_max_dbtp = 0.0;
// A proposal lands within this of its loudness target.
inline constexpr double loudness_tolerance_lu = 0.5;

struct Goals {
  double lufs = loudness_presets[0].lufs;
  double ceiling_dbtp = loudness_presets[0].ceiling_dbtp;
  Character character = Character::clean;
  // Gain after the chain (the master fader), in dB: the chain aims under
  // the ceiling and at the loudness less this, so the master leaves at both.
  double after_chain_db = 0.0;
};

// ---- Regions -------------------------------------------------------------------------

// Crest factor, transient density and width are measured in three regions,
// the limiter audit's: the lows, the mids and the top.
inline constexpr std::size_t regions = 3;
inline constexpr std::array<std::string_view, regions> region_names{"lows", "mids", "highs"};
inline constexpr double low_split_hz = 150.0;
inline constexpr double high_split_hz = 2'500.0;
// Low-end mono-ness is judged under this.
inline constexpr double mono_bass_hz = 120.0;

// ---- Analysis ------------------------------------------------------------------------

// Loudness steps (BS.1770's 100 ms) and the windows built on them.
inline constexpr double step_seconds = 0.1;
// Crest factor: each step's peak over its RMS, averaged over the steps
// within crest_gate_db of the loudest few (crest_reference_share of them),
// so fades and gaps do not count.
inline constexpr double crest_gate_db = 20.0;
inline constexpr double crest_reference_share = 0.05;
// Onsets: each region's energy per hop (longer in the lows, where a hop
// must hold a cycle or two); a hop is an onset when it rises onset_rise_db
// over the mean of the onset_history_hops before it, within onset_range_db
// of the region's loudest hop so far and above onset_floor_dbfs, at least
// onset_spacing_seconds after the last.
inline constexpr std::array<double, 3> onset_hop_seconds{0.02, 0.005, 0.005};
inline constexpr std::size_t onset_history_hops = 10;
inline constexpr double onset_rise_db = 6.0;
inline constexpr double onset_range_db = 20.0;
inline constexpr double onset_floor_dbfs = -50.0;
inline constexpr double onset_spacing_seconds = 0.1;
// Tonal spread: each segment's lows-to-mids and top-to-mids balance; the
// segments within spread_gate_lu of the loudest count.
inline constexpr double segment_seconds = 4.0;
inline constexpr double spread_gate_lu = 10.0;
// The loud level a compressor's threshold is set from: this percentile of
// the gated steps' RMS.
inline constexpr double loud_percentile = 0.9;
// Below this a level is silence.
inline constexpr double floor_db = -120.0;

struct RegionMeasure {
  double crest_db = 0.0;
  double onsets_per_second = 0.0;
  // Side over mid power, dB (minus infinity is mono; floor_db stands in).
  double width_db = floor_db;
  // The region's share of the mid's power, dB.
  double share_db = floor_db;
  // The loud level (loud_percentile of gated steps), dBFS RMS of the mid.
  double loud_db = floor_db;
};

struct Analysis {
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;
  std::optional<double> integrated_lufs;
  double max_momentary_lufs = floor_db;
  double max_short_term_lufs = floor_db;
  double true_peak_dbtp = floor_db;
  double sample_peak_dbfs = floor_db;
  double crest_db = 0.0;
  // dBFS RMS of both channels at loud_percentile of the gated steps.
  double loud_db = floor_db;
  // The mid's normalised tonal curve (ADR-0120), its fine band powers, and
  // per band the side over the mid (dB).
  tonal::Curve tonal{};
  tonal::FineCurve fine_power{};
  tonal::Curve width_db{};
  std::array<RegionMeasure, regions> region{};
  // Side over mid under mono_bass_hz, dB.
  double low_side_db = floor_db;
  // How far the balance between regions moves from segment to segment (dB,
  // the larger standard deviation of the two balances).
  double tonal_spread_db = 0.0;
  // K-weighted mean power per 100 ms step, both channels summed.
  std::vector<float> step_power;

  [[nodiscard]] double seconds() const noexcept;
  [[nodiscard]] double lufs() const noexcept { return integrated_lufs.value_or(floor_db); }
};

// Builds an Analysis in one pass over planar stereo blocks of any size.
class Analyser {
public:
  explicit Analyser(std::uint32_t sample_rate);
  Analyser(Analyser&&) noexcept;
  Analyser& operator=(Analyser&&) noexcept;
  Analyser(const Analyser&) = delete;
  Analyser& operator=(const Analyser&) = delete;
  ~Analyser();

  void push(std::span<const float> left, std::span<const float> right);
  [[nodiscard]] Analysis finish() &&;

private:
  struct State;
  std::unique_ptr<State> state_;
};

[[nodiscard]] Analysis analyse(std::span<const float> left, std::span<const float> right,
                               std::uint32_t sample_rate);

// The tonal distance of a normalised curve from a target's: the RMS of
// their difference over tonal_judged_low_hz..tonal_judged_high_hz once both
// are centred there, and the band where it is largest (signed).
inline constexpr double tonal_judged_low_hz = 40.0;
inline constexpr double tonal_judged_high_hz = 16'000.0;
struct TonalDistance {
  double rms_db = 0.0;
  double worst_db = 0.0;
  std::size_t worst_band = 0;
};
[[nodiscard]] TonalDistance tonal_distance(const tonal::Curve& levels,
                                           const tonal::Curve& target) noexcept;
// The centred difference per band (levels minus target; 0 outside the
// judged range).
[[nodiscard]] tonal::Curve tonal_difference(const tonal::Curve& levels,
                                            const tonal::Curve& target) noexcept;

// The loudest window of `seconds` (a whole number of steps) by K-weighted
// power: its first frame and length. The whole programme when shorter.
struct Window {
  std::uint64_t first = 0;
  std::uint64_t frames = 0;
};
[[nodiscard]] Window loudest_window(const Analysis& analysis, double seconds) noexcept;

// ---- References ----------------------------------------------------------------------

// What a reference track says besides its tonal curve: per-region width
// and crest, and its low side. Only references measured from audio have
// them; a genre target says nothing about width.
struct Traits {
  std::array<double, regions> width_db{};
  double crest_db = 0.0;
  double low_side_db = floor_db;
};

struct Reference {
  std::string name;
  tonal::Target target;
  // Measured from the reference tracks' own audio.
  std::optional<Traits> traits;
};
// A genre target (ADR-0120) as the reference.
[[nodiscard]] Reference reference_from(const tonal::Target& target);
// Reference tracks: their tonal curves make a target (median and range),
// their traits are averaged. Needs at least one analysed track with
// measurable loudness.
[[nodiscard]] foundation::Result<Reference> reference_from(std::string name,
                                                           std::span<const Analysis> tracks);

// ---- Roles ---------------------------------------------------------------------------

// The rebalance roles (ADR-0123), in project::MixRole order.
inline constexpr std::size_t role_count = 4;
inline constexpr std::array<std::string_view, role_count> role_names{"Vocals", "Drums", "Bass",
                                                                     "Music"};
// A role's mid power per region (mean square), summed over its tracks.
using RoleRegions = std::array<double, regions>;
using RolePowers = std::array<std::optional<RoleRegions>, role_count>;
// A mono signal's power per region (summed squares), split as the
// Analyser splits the mid.
class RegionMeter {
public:
  explicit RegionMeter(std::uint32_t sample_rate);
  RegionMeter(RegionMeter&&) noexcept;
  RegionMeter& operator=(RegionMeter&&) noexcept;
  RegionMeter(const RegionMeter&) = delete;
  RegionMeter& operator=(const RegionMeter&) = delete;
  ~RegionMeter();
  void push(std::span<const float> mono);
  [[nodiscard]] RoleRegions powers() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};
// Nudges: a region this far from the target, made this much by one role,
// moves that role's gain by the region's distance times its share, at most
// nudge_max_db, in nudge_step_db steps (none under one step).
inline constexpr double nudge_region_db = 2.0;
inline constexpr double nudge_share = 0.6;
inline constexpr double nudge_max_db = 2.0;
inline constexpr double nudge_step_db = 0.5;

// ---- Proposal ------------------------------------------------------------------------

// One device: its engine, every parameter's value (by id), and one plain
// line with the measurement behind it.
struct Device {
  std::string engine;
  std::string name;
  std::vector<std::pair<std::string, double>> values;
  std::string reason;
};
struct Nudge {
  std::size_t role = 0;
  double gain_db = 0.0;
  std::string reason;
};
struct Proposal {
  Goals goals;
  std::vector<Nudge> nudges;
  // In chain order: gain and width (Trim), corrective EQ, dynamics,
  // colour, mono bass (Vista), limiter.
  std::vector<Device> devices;
  // What was weighed and left out, one line each.
  std::vector<std::string> notes;
  // The mix as measured, for the limiter's line.
  double mix_lufs = floor_db;
  double mix_true_peak_dbtp = floor_db;
};
// The limiter's line for a gain (and a trim before it).
[[nodiscard]] std::string ceiling_reason(const Proposal& proposal, double gain_db, double trim_db);

// The rules (each constant is one decision; ADR-0124 gives the reasons).
// Corrective EQ from this tonal distance; Keel instead of Shape when the
// balance moves this much over time.
inline constexpr double eq_min_distance_db = 1.0;
inline constexpr double keel_spread_db = 2.0;
inline constexpr double eq_amount = 0.8;
inline constexpr double eq_smoothing_octaves = 1.0 / 3.0;
inline constexpr double eq_low_hz = 30.0;
inline constexpr double eq_high_hz = 16'000.0;
inline constexpr double eq_max_boost_db = 4.0;
inline constexpr double eq_max_cut_db = 6.0;
inline constexpr double keel_amount = 0.7;
inline constexpr double keel_limit_db = 4.0;
inline constexpr double keel_speed_seconds = 8.0;
// Dynamics: the loudness the limiter alone may cost (its gain less the
// loudness it adds, LU) per character. Beyond it (by at least
// dynamics_min_db) a compressor is tried for the rest, at most
// dynamics_max_db: Press, and Strata too when the regions' crests differ by
// strata_crest_spread_db. Each is learned to the target and the one whose
// output keeps the most crest wins; a compressor must keep
// dynamics_crest_margin_db more than the limiter alone (Warm, which wants
// its glue, may keep that much less).
inline constexpr std::array<double, character_count> limiter_share_db{3.0, 4.0, 3.5};
inline constexpr double dynamics_min_db = 1.0;
inline constexpr double dynamics_max_db = 4.0;
inline constexpr double strata_crest_spread_db = 4.0;
inline constexpr double dynamics_crest_margin_db = 0.25;
// Colour: Gild's air when the top sits this far under the target (not
// Clean); Strike is tried when Punchy, the lows hit this often and the
// limiter costs this much, and kept when its output keeps more crest.
inline constexpr double gild_top_hz = 6'000.0;
inline constexpr double gild_top_deficit_db = 3.0;
inline constexpr double strike_min_onsets = 1.0;
inline constexpr double strike_reduction_db = 3.0;
// Width: Vista's mono bass when the lows' side is above this (Mix Check's
// mono-lows line; Vista's band split turns the mid's phase, so it is used
// only for that); Trim's
// wideband width when a reference track's mids and top sit this far from
// the mix's on average, within these limits.
inline constexpr double vista_low_side_db = -12.0;
inline constexpr double width_min_change_db = 2.0;
inline constexpr double width_min = 0.7;
inline constexpr double width_max = 1.3;
// Short of the target with the limiter's gain at its top, its clip room
// grows by the shortfall, up to Ceiling's range, at most this many times.
inline constexpr double clip_max_db = 6.0;
inline constexpr std::size_t clip_widen_steps = 3;

// The choices made by measuring: what the limiter alone was measured to
// cost (learn_gain), and the dynamics and transient shaping to propose.
enum class Dynamics : std::uint8_t { none, press, strata };
struct Choices {
  double limiter_loss_db = 0.0;
  Dynamics dynamics = Dynamics::none;
  bool strike = false;
};
// The variants worth measuring once the limiter's cost is known: none
// first, then each compressor and Strike the rules allow.
[[nodiscard]] std::vector<Choices> candidates(const Analysis& mix, const Goals& goals,
                                              double limiter_loss_db);

// The proposal for a measured mix. Pure and deterministic: the same inputs
// give the same proposal. Ceiling's gain is the plain estimate here;
// learn_gain sets it on the audio.
[[nodiscard]] Proposal derive(const Analysis& mix, const Reference& reference, const Goals& goals,
                              const RolePowers& roles = {}, const Choices& choices = {});

// ---- Running a chain -----------------------------------------------------------------

// A stereo programme that can be read again from any frame.
class Source {
public:
  virtual ~Source() = default;
  [[nodiscard]] virtual std::uint32_t sample_rate() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t frames() const noexcept = 0;
  // Reads up to left.size() frames from `at`; returns how many.
  [[nodiscard]] virtual foundation::Result<std::size_t>
  read(std::uint64_t at, std::span<float> left, std::span<float> right) = 0;
};
// A source that is written first, then read.
class Store : public Source {
public:
  [[nodiscard]] virtual foundation::Result<void> append(std::span<const float> left,
                                                        std::span<const float> right) = 0;
};
class MemoryStore final : public Store {
public:
  explicit MemoryStore(std::uint32_t sample_rate) noexcept : sample_rate_(sample_rate) {}
  MemoryStore(std::uint32_t sample_rate, std::vector<float> left, std::vector<float> right);
  [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return sample_rate_; }
  [[nodiscard]] std::uint64_t frames() const noexcept override { return left_.size(); }
  [[nodiscard]] foundation::Result<std::size_t> read(std::uint64_t at, std::span<float> left,
                                                     std::span<float> right) override;
  [[nodiscard]] foundation::Result<void> append(std::span<const float> left,
                                                std::span<const float> right) override;
  [[nodiscard]] std::span<const float> left() const noexcept { return left_; }
  [[nodiscard]] std::span<const float> right() const noexcept { return right_; }

private:
  std::uint32_t sample_rate_;
  std::vector<float> left_;
  std::vector<float> right_;
};
using StoreFactory = std::function<foundation::Result<std::unique_ptr<Store>>(std::uint32_t)>;
// True to stop.
using Cancelled = std::function<bool()>;

// One native effect at its values (and document, for engines that have one).
struct ChainDevice {
  const effects::EffectEngine* engine = nullptr;
  std::vector<double> values;
  std::optional<project::NativeSynthDocument> document;
};
// A proposal's devices at the engines' defaults plus the proposed values.
[[nodiscard]] foundation::Result<std::vector<ChainDevice>>
chain_from(std::span<const Device> devices);

// Stereo audio captured from a run.
struct Capture {
  std::vector<float> left;
  std::vector<float> right;
};
// Runs and analyses the chain over the whole source, block by block, with
// each device's latency taken out so the output lines up with the input,
// then `output_gain` (linear). Optionally appends the output to `sink` and
// copies `window` of it to `capture`.
inline constexpr std::size_t run_block_frames = 512;
struct RunOptions {
  double output_gain = 1.0;
  Store* sink = nullptr;
  std::optional<Window> window;
  Capture* capture = nullptr;
  Cancelled cancelled;
};
[[nodiscard]] foundation::Result<Analysis>
run_chain(Source& source, std::span<const ChainDevice> chain, const RunOptions& options);

// Learn: the gain that brings the chain's output to the target loudness.
// The devices before `limiter` run once into a store; the search then runs
// from the limiter on, moving its gain parameter (0 dB up). A loudness the
// chain passes at 0 dB is taken down by `trim`'s parameter (before the
// limiter), when there is one. Secant steps from the plain estimate, until
// within learn_tolerance_lu, learn_max_passes, or the gain's end.
inline constexpr double learn_tolerance_lu = 0.1;
inline constexpr std::size_t learn_max_passes = 8;
struct Knob {
  std::size_t device = 0;
  std::size_t parameter = 0;
};
struct GainSearch {
  Knob limiter;
  std::optional<Knob> trim;
  double target_lufs = 0.0;
  double output_gain = 1.0;
};
struct Learned {
  double gain_db = 0.0;
  double trim_db = 0.0;
  std::size_t passes = 0;
  // The loudness the limiter cost: its input's loudness plus its gain (and
  // the gain after the chain) less the output's.
  double limiter_loss_db = 0.0;
  // The limiter's gain was at the top of its range.
  bool at_most = false;
  // The final chain's output.
  Analysis output;
  Capture window;
};
[[nodiscard]] foundation::Result<Learned> learn_gain(Source& source, std::vector<ChainDevice> chain,
                                                     const GainSearch& search,
                                                     const StoreFactory& stores,
                                                     std::optional<Window> window = std::nullopt,
                                                     const Cancelled& cancelled = {});

// Writes what learn_gain found into the proposal's Ceiling (gain) and
// Trim (gain) values.
void apply_learned(Proposal& proposal, const Learned& learned);

// The whole of it for a programme held by `source`: derive, learn the gain
// on the proposal's own chain, and measure the outcome. Once the limiter's
// cost is known, each candidate is derived and learned and the one keeping
// the most crest is kept; when the gain tops out short of the target, the
// clip room grows by the shortfall and the gain is learned once more.
struct Planned {
  Proposal proposal;
  Learned learned;
};
[[nodiscard]] foundation::Result<Planned>
plan(Source& source, const Analysis& mix, const Reference& reference, const Goals& goals,
     const StoreFactory& stores, const RolePowers& roles = {},
     std::optional<Window> window = std::nullopt, const Cancelled& cancelled = {});

// ---- Outcome -------------------------------------------------------------------------

// A measured master against the goals and the reference.
struct Outcome {
  double lufs = floor_db;
  double true_peak_dbtp = floor_db;
  double tonal_rms_db = 0.0;
  double crest_db = 0.0;
};
[[nodiscard]] Outcome outcome(const Analysis& analysis, const Reference& reference) noexcept;
// One line each: loudness against the target, the true peak against the
// ceiling, the tonal distance and the crest factor, `before` to `after`.
[[nodiscard]] std::vector<std::string> outcome_lines(const Outcome& before, const Outcome& after,
                                                     const Goals& goals);
// Whether a master meets the goals: loudness within loudness_tolerance_lu,
// true peak at or under the ceiling (both less after_chain_db).
[[nodiscard]] bool meets(const Outcome& outcome, const Goals& goals) noexcept;

// Honest level matching for an A/B: the louder of two is turned down to the
// quieter. The gain (dB, at most 0) for the one at `lufs`.
[[nodiscard]] double matched_gain_db(double lufs, double other_lufs) noexcept;

} // namespace oscillate::native_dsp::assistant

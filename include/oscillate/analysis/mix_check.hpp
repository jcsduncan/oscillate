#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Mix Check's measurements (ADR-0074): pure analysis of rendered stereo
// float buffers, with no project model, so every check is testable on a
// synthetic signal. The application layer renders the song once, streams
// every track's output and the master through these builders and meters as
// it renders, and calls these checks. Nothing keeps a song-length signal, so
// a 90-minute mix costs what a 3-minute song does per track and minute of
// sound. Worker threads only; nothing here touches the audio callback.
namespace oscillate::analysis::mix {

// ---- Bands ---------------------------------------------------------------------------

// The ISO 266 third-octave bands from 20 Hz to 20 kHz (base-two centres,
// 1 kHz at band 17).
inline constexpr std::size_t band_count = 31;
inline constexpr std::size_t reference_band = 17; // 1 kHz.
using BandPowers = std::array<float, band_count>;
[[nodiscard]] double band_centre(std::size_t band) noexcept;
[[nodiscard]] double band_low(std::size_t band) noexcept;
[[nodiscard]] double band_high(std::size_t band) noexcept;
// The band holding `hz` (clamped to the first and last band).
[[nodiscard]] std::size_t band_of(double hz) noexcept;

// The hero spectrum's grid: log-spaced points, each the power in the
// twelfth of an octave around it.
inline constexpr std::size_t display_points = 200;
inline constexpr double display_low_hz = 20.0;
inline constexpr double display_high_hz = 20'000.0;
[[nodiscard]] double display_frequency(std::size_t point) noexcept;

// Below this a level is silence: dB readings never go lower, so reports
// stay finite.
inline constexpr double silence_db = -120.0;
[[nodiscard]] double power_db(double power) noexcept;

// ---- Profile -------------------------------------------------------------------------

// Decays: a hop is a hit when its power rises this far over the hop
// before. After each hit every short-STFT bin's power is followed for at
// most decay_follow_seconds (or until the next hit) and averaged over the
// hits; an offset after the hits counts once decay_min_hits reach it.
inline constexpr double onset_rise_db = 6.0;
inline constexpr double decay_follow_seconds = 2.0;
inline constexpr std::size_t decay_min_hits = 4;
// A frequency's decay time: from its loudest hop after the hits until its
// mean power has fallen this far, interpolated between hops (capped at the
// last offset the hits reach).
inline constexpr double decay_drop_db = 12.0;

// Pitch, for audio tracks (no notes say what they play): each hop's
// strongest harmonic series with its fundamental between pitch_low_hz and
// pitch_high_hz, on a grid of pitch_steps_per_octave, summed over
// pitch_harmonics harmonics under pitch_top_hz. The hop is pitched when the
// series holds pitched_share of the power there and at least
// pitch_min_partials of its harmonics each hold pitch_partial_share of the
// series (one lone partial is not a pitch).
inline constexpr double pitch_low_hz = 40.0;
inline constexpr double pitch_high_hz = 1'000.0;
inline constexpr double pitch_top_hz = 5'000.0;
inline constexpr std::size_t pitch_harmonics = 12;
inline constexpr double pitch_steps_per_octave = 48.0;
inline constexpr double pitched_share = 0.5;
inline constexpr std::size_t pitch_min_partials = 3;
inline constexpr double pitch_partial_share = 0.05;

struct ProfileOptions {
  // Follow each hop's pitch (audio tracks): what sits on the harmonics of
  // the pitch sounding at the time is the note, not a resonance.
  bool detect_pitch = false;
};

// One stereo signal's measurements. Spectra are mean powers per FFT bin,
// energy-normalised so a bin sum is a mean square (a full-scale sine sums
// to 0.5). Mid is (L+R)/2 and side (L-R)/2, so mid + side is the mean of
// the two channels' powers and mid alone is what a mono fold keeps.
struct Profile {
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;

  // The fine long-term spectrum (fine_size-point Hann STFT, half overlap).
  std::size_t fine_size = 0;
  std::vector<double> left;
  std::vector<double> right;
  std::vector<double> mid;
  std::vector<double> side;

  // The short STFT (short_size points, hop short_hop): hop_count hops, and
  // for each hop with any power (hop_index, ascending) its power (mid +
  // side) in each third-octave band, for co-activity and masking. Silent
  // hops are left out, so a track that plays five minutes of a 90-minute
  // mix keeps five minutes of hops.
  std::size_t short_size = 0;
  std::size_t short_hop = 0;
  std::size_t hop_count = 0;
  std::vector<std::uint32_t> hop_index;
  std::vector<BandPowers> hop_bands;
  // The short spectrum over only the hops in a decay (not an attack): what
  // keeps sounding after each hit.
  std::vector<double> sustain;
  std::size_t sustain_hops = 0;
  // Per short bin: its decay time after the hits (seconds) and its energy
  // over the followed hops (the weight of its decay in its neighbourhood).
  // Empty below decay_min_hits hits.
  std::vector<float> decay;
  std::vector<float> decay_energy;
  std::size_t decay_hits = 0;
  // Per short bin, with detect_pitch: the share of the decay hops in which
  // it sat on a harmonic of that hop's pitch. Empty otherwise.
  std::vector<float> harmonic_share;

  // BS.1770 K-weighted mean power (both channels summed) per 100 ms step.
  std::uint32_t step_frames = 0;
  std::vector<double> steps;

  // Sample statistics.
  double sample_peak = 0.0; // Largest |sample| of either channel.
  double true_peak = 0.0;   // 4x intersample estimate (bs1770::TruePeak).
  double left_sum = 0.0;
  double right_sum = 0.0;
  double left_squares = 0.0;
  double right_squares = 0.0;

  [[nodiscard]] double fine_bin_hz() const noexcept;
  [[nodiscard]] double short_bin_hz() const noexcept;
  [[nodiscard]] double seconds() const noexcept;
  [[nodiscard]] double hop_seconds() const noexcept;
  [[nodiscard]] double rms_db() const noexcept;
  [[nodiscard]] double peak_db() const noexcept;
  [[nodiscard]] double true_peak_db() const noexcept;
};

// Builds a Profile in one pass over planar stereo blocks of any size.
class ProfileBuilder {
public:
  explicit ProfileBuilder(std::uint32_t sample_rate, ProfileOptions options = {});
  ProfileBuilder(ProfileBuilder&&) noexcept;
  ProfileBuilder& operator=(ProfileBuilder&&) noexcept;
  ProfileBuilder(const ProfileBuilder&) = delete;
  ProfileBuilder& operator=(const ProfileBuilder&) = delete;
  ~ProfileBuilder();

  void push(std::span<const float> left, std::span<const float> right);
  [[nodiscard]] Profile finish() &&;

private:
  struct State;
  std::unique_ptr<State> state_;
};

[[nodiscard]] Profile profile(std::span<const float> left, std::span<const float> right,
                              std::uint32_t sample_rate, ProfileOptions options = {});

// Third-octave band powers of the long-term spectrum: (L+R)/2 power ("stereo",
// mid + side), mid only, left and right.
[[nodiscard]] BandPowers band_powers(const Profile& profile);
[[nodiscard]] BandPowers band_mid(const Profile& profile);
[[nodiscard]] BandPowers band_side(const Profile& profile);
[[nodiscard]] BandPowers band_left(const Profile& profile);
[[nodiscard]] BandPowers band_right(const Profile& profile);
// Power between two frequencies in the long-term spectrum, (L+R)/2.
[[nodiscard]] double power_between(const Profile& profile, double low_hz, double high_hz);

// The hero spectrum (dB per display point): stereo power, or the mono fold.
[[nodiscard]] std::vector<double> display_spectrum(const Profile& profile);
[[nodiscard]] std::vector<double> display_mono_spectrum(const Profile& profile);
// dB at log-spaced points between two frequencies (fine spectrum, or the
// sustain spectrum), for a finding's evidence plot.
[[nodiscard]] std::vector<double> spectrum_points(const Profile& profile, double low_hz,
                                                  double high_hz, std::size_t points,
                                                  bool sustain = false);
[[nodiscard]] std::vector<double> log_points(double low_hz, double high_hz, std::size_t points);

// ---- Loudness ------------------------------------------------------------------------

struct Loudness {
  std::optional<double> integrated; // LUFS; none below 400 ms or under the gate.
  double true_peak_db = silence_db; // dBTP.
  std::optional<double> range;      // LU (EBU Tech 3342).
  // Ungated short-term (3 s) loudness ending at each 100 ms step.
  std::vector<std::optional<double>> short_term;
};
[[nodiscard]] Loudness loudness(const Profile& profile);
// Loudness of the mean K-weighted power over [first, last) steps.
[[nodiscard]] std::optional<double> steps_loudness(const Profile& profile, std::size_t first,
                                                   std::size_t last);
// Peak to RMS in dB.
[[nodiscard]] double crest_db(double peak, double mean_square) noexcept;

// ---- Sub rumble ----------------------------------------------------------------------

// The sub region ends this far under the lowest fundamental (three
// semitones), clear of the note's own main lobe.
inline constexpr double sub_edge_ratio = 0.84;
// The region's bottom: below this nothing is reproduced anyway.
inline constexpr double sub_floor_hz = 16.0;
// The top of the bass's own region the sub is compared with.
inline constexpr double low_region_top_hz = 120.0;
// Energy under the lowest note is fine up to this far below the bass's own
// region; anything above is the excess.
inline constexpr double sub_allowance_db = 15.0;
// A finding from this much excess; "bad" from sub_bad_db.
inline constexpr double sub_excess_threshold_db = 3.0;
inline constexpr double sub_bad_db = 6.0;
// The reported band spans the sub peak's bins within this of it.
inline constexpr double band_extent_db = 6.0;

struct SubRumble {
  double low_hz = 0.0;
  double high_hz = 0.0;
  double excess_db = 0.0;
  double sub_db = silence_db;
  double reference_db = silence_db;
};
// Energy below `lowest_fundamental_hz`, against the bass region above it.
[[nodiscard]] std::optional<SubRumble> find_sub_rumble(const Profile& profile,
                                                       double lowest_fundamental_hz);
// The strongest spectral peak below low_region_top_hz: a bass's
// fundamental when no notes say which it is.
[[nodiscard]] std::optional<double> strongest_low_peak_hz(const Profile& profile);
// The share of the long-term power below low_region_top_hz.
[[nodiscard]] double low_share(const Profile& profile);
// A bass's lowest fundamental as its own spectrum confirms it: the lowest
// peak under low_region_top_hz, at most fundamental_strong_db under the
// strongest there, within fundamental_match_cents of one of `candidates_hz`
// (or fundamental_match_bins of a fine bin, when that is wider: the peak is
// interpolated, so half a bin still tells 27.5 Hz from 30 Hz).
inline constexpr double fundamental_strong_db = 20.0;
inline constexpr double fundamental_match_cents = 50.0;
inline constexpr double fundamental_match_bins = 0.5;
[[nodiscard]] std::optional<double> lowest_matching_peak_hz(const Profile& profile,
                                                            std::span<const double> candidates_hz);

// ---- Mono safety ---------------------------------------------------------------------

// A band loses this much or more when folded to mono to be flagged; "bad"
// from mono_bad_db. Uncorrelated stereo loses 3 dB, anticorrelated more.
inline constexpr double mono_loss_threshold_db = 2.0;
inline constexpr double mono_bad_db = 3.0;
// Bands quieter than this under the loudest band are not judged.
inline constexpr double judged_range_db = 40.0;
// Below the lock crossover the side should sit this far under the mid.
inline constexpr double lock_crossover_hz = 120.0;
inline constexpr double low_side_threshold_db = -12.0;

// Mono fold loss per band (dB, positive is a loss; 0 for silent bands).
[[nodiscard]] std::array<double, band_count> band_mono_loss(const Profile& profile);
struct MonoLoss {
  std::size_t first_band = 0;
  std::size_t last_band = 0;
  double loss_db = 0.0;  // Over the whole span.
  double worst_db = 0.0; // Its worst band.
};
[[nodiscard]] std::optional<MonoLoss> find_mono_loss(const Profile& profile);
// Side over mid below lock_crossover_hz, in dB.
[[nodiscard]] double low_side_db(const Profile& profile);
// Mono loss of a band span (dB).
[[nodiscard]] double span_mono_loss(const Profile& profile, std::size_t first_band,
                                    std::size_t last_band);

// ---- Resonances ----------------------------------------------------------------------

// A resonance is judged against the instrument's own sound: modal drums
// (Knock, Anvil) and tuned engines are made of partials, so a partial is
// only ringing when it keeps sounding much longer than the rest of the
// sound around it. Candidates are narrow peaks in the decay standing at
// least this far over the smoothed spectrum around them, between these
// frequencies, no quieter than resonance_audible_db under the decay's
// loudest bin.
inline constexpr double resonance_threshold_db = 9.0;
inline constexpr double resonance_low_hz = 300.0;
inline constexpr double resonance_high_hz = 16'000.0;
inline constexpr double resonance_audible_db = 40.0;
// The smoothing reaches a third of an octave each side, skipping the
// peak's own few bins.
inline constexpr double resonance_neighbourhood_octaves = 1.0 / 3.0;
inline constexpr std::size_t resonance_peak_bins = 3;
// Peaks closer than this (octaves) are one resonance.
inline constexpr double resonance_merge_octaves = 1.0 / 24.0;
// A decay needs this many hops to be judged.
inline constexpr std::size_t resonance_min_hops = 8;
// A peak this close (cents) to a played note's harmonic is the note; so is
// one on the detected pitch's harmonics in harmonic_share_threshold of the
// decay hops (audio).
inline constexpr double note_harmonic_cents = 50.0;
inline constexpr std::size_t note_harmonics = 32;
inline constexpr double harmonic_share_threshold = 0.5;
// A candidate rings when its decay time is at least resonance_decay_ratio
// times the energy-weighted median decay of the frequencies within
// resonance_decay_octaves of it (its own peak bins left out): the rest of
// that sound. When those carry less than resonance_context_db under the
// candidate's own energy (a lone partial, its neighbours only its leakage),
// the whole sound is the rest. Decays under decay_resolution_hops of a hop
// are below what the hops resolve and count as that.
inline constexpr double resonance_decay_ratio = 3.0;
inline constexpr double resonance_decay_octaves = 1.0;
inline constexpr double resonance_context_db = 10.0;
inline constexpr double decay_resolution_hops = 0.5;
// At most this many peaks per track: the longest-ringing.
inline constexpr std::size_t resonance_max_peaks = 4;

struct Resonance {
  double hz = 0.0;
  double prominence_db = 0.0;
  double level_db = silence_db;
  double decay_seconds = 0.0;
  // decay_seconds over the decay of the sound around it.
  double decay_ratio = 0.0;
};
// The longest-ringing first, at most `limit`. Peaks near `played_hz`'s
// harmonics (the notes the track plays) are not resonances.
[[nodiscard]] std::vector<Resonance>
find_resonances(const Profile& profile, std::span<const double> played_hz, std::size_t limit);

// ---- Kick/bass cancellation ----------------------------------------------------------

// The lows a kick and a bass share: the mono sum (L+R)/2 under
// lock_crossover_hz (fourth-order Butterworth). Streamed, block by block.
class LowBand {
public:
  explicit LowBand(std::uint32_t sample_rate) noexcept;
  // `out` holds left.size() samples.
  void process(std::span<const float> left, std::span<const float> right,
               std::span<float> out) noexcept;

private:
  std::array<std::array<double, 5>, 2> coefficients_{};
  std::array<std::array<double, 2>, 2> state_{};
};

// Each hit is judged over hit_window_seconds after it, in windows of
// dip_window_seconds: in each, the energy of the sum against the kick's and
// the other's own energies. A window counts when both sit within
// overlap_range_db of each other and of the hit's loudest window; the hit's
// dip is its deepest window's. The finding reads the dip_percentile of the
// hits' dips, so one odd hit does not decide it.
inline constexpr double hit_window_seconds = 0.15;
inline constexpr double dip_window_seconds = 0.02;
inline constexpr double overlap_range_db = 20.0;
inline constexpr double dip_percentile = 0.9;
// A dip from this deep is a finding (always "warn": Pump often hides it).
inline constexpr double cancellation_threshold_db = 6.0;
// The evidence: the worst hit's low sum, from dip_plot_before_seconds
// before it to dip_plot_after_seconds after, in steps of
// dip_plot_step_seconds.
inline constexpr double dip_plot_before_seconds = 0.1;
inline constexpr double dip_plot_after_seconds = 0.5;
inline constexpr double dip_plot_step_seconds = 0.005;

struct Cancellation {
  double deepest_db = 0.0; // The dip_percentile of the hits' dips.
  double worst_db = 0.0;   // The worst hit's.
  double at_seconds = 0.0; // The worst hit.
  std::size_t hits = 0;    // Hits where the two overlap.
  // The worst hit's low sum (dB) per plot step from dip_plot_before_seconds
  // before it.
  std::vector<double> envelope;
};

// One kick and one other track (a bass), streamed as the song renders: both
// LowBand outputs, time-aligned, from the same project frame each block.
class CancellationMeter {
public:
  CancellationMeter(std::uint32_t sample_rate, std::vector<std::uint64_t> hit_frames);
  void push(std::span<const float> kick_low, std::span<const float> other_low,
            std::uint64_t first_frame);
  // After the last push. Nothing when the two never overlap under a hit.
  [[nodiscard]] std::optional<Cancellation> finish();

private:
  struct Window {
    double kick = 0.0;
    double other = 0.0;
    double sum = 0.0;
  };
  void close_hit(std::size_t hit);
  void take_envelope(std::size_t pending);

  std::uint32_t sample_rate_;
  std::vector<std::uint64_t> hits_;
  std::uint64_t window_frames_;
  std::size_t windows_;
  std::uint64_t step_frames_;
  std::uint64_t before_frames_;
  std::uint64_t after_frames_;
  std::size_t next_hit_ = 0;              // The first hit not yet closed.
  std::vector<std::vector<Window>> open_; // Per open hit, from next_hit_.
  std::vector<double> dips_;
  std::optional<std::size_t> worst_;
  double worst_db_ = 0.0;
  // The low sum's energy per plot step, the last few hundred ms.
  std::uint64_t steps_first_ = 0; // Step index of steps_.front().
  std::vector<double> steps_;
  std::optional<std::size_t> pending_envelope_; // A hit whose plot is still filling.
  std::vector<double> envelope_;
  std::uint64_t end_frame_ = 0;
};

// ---- Drum crest ----------------------------------------------------------------------

// The drums' crest, streamed: peak against RMS of the mono sum of the tracks
// known to be drums and of those candidates (audio tracks, decided by their
// own crest once measured) that turn out to be. The sum's power is exact
// (every lane's energy and each pair's cross energy); its peak is exact in
// each block where at most crest_exact_lanes candidates sound, else the
// known drums' block peak plus the included candidates' (a bound).
inline constexpr std::size_t crest_exact_lanes = 4;
class DrumCrest {
public:
  explicit DrumCrest(std::size_t candidates);
  // One aligned block: the known drums' mono sum (empty: none) and each
  // candidate's mono block (empty: silent).
  void push(std::span<const float> drums, std::span<const std::span<const float>> candidates);
  // With `included` (one flag per candidate); nothing when no drum sounds.
  [[nodiscard]] std::optional<double> crest_db(const std::vector<bool>& included) const;

private:
  struct Block {
    std::size_t first_lane = 0; // In lanes_.
    std::size_t first_peak = 0; // In peaks_.
    std::uint8_t lanes = 0;
    bool exact = true;
  };
  std::size_t candidates_;
  std::uint64_t frames_ = 0;
  // (1 + candidates)^2: [0] is the known drums, then the candidates.
  std::vector<double> energy_;
  float quiet_peak_ = 0.0F; // Blocks where no candidate sounds.
  bool any_known_ = false;
  std::vector<bool> sounded_;
  std::vector<Block> blocks_;
  std::vector<std::uint32_t> lanes_;
  std::vector<float> peaks_;
};

// ---- Clicks --------------------------------------------------------------------------

// A sample-to-sample jump at an edge is a click when it is at least this
// many times the local slope's RMS around it and above the floor.
inline constexpr double click_ratio = 6.0;
inline constexpr double click_floor = 0.003; // About -50 dBFS.
// The local slope is measured this far each side of the edge.
inline constexpr double click_context_seconds = 0.002;

// The jump's ratio (dB) over the local slope at `edge` (the first sample
// after it; the sample before frame 0 is silence), when it clicks.
[[nodiscard]] std::optional<double> edge_click(std::span<const float> left,
                                               std::span<const float> right, std::size_t edge,
                                               std::uint32_t sample_rate);

// ---- Sections ------------------------------------------------------------------------

// Bars are grouped into phrases of this many; neighbouring phrases within
// section_change_lu of each other are one section.
inline constexpr std::size_t phrase_bars = 8;
inline constexpr double section_change_lu = 2.0;
// Sections within this of the loudest are drops.
inline constexpr double drop_window_lu = 2.0;
// A later drop quieter than an earlier one, or a break louder than a drop,
// by more than this is a finding.
inline constexpr double section_issue_lu = 0.5;

struct Section {
  std::string name;
  std::size_t first_bar = 0; // Zero-based, inclusive.
  std::size_t last_bar = 0;
  double lufs = silence_db;
};
// Power mean of bars' loudness (silent bars are skipped).
[[nodiscard]] double mean_lufs(std::span<const double> bar_lufs, std::size_t first,
                               std::size_t last);
// Sections by energy changes, named Intro / Build / Drop n / Break / Outro.
[[nodiscard]] std::vector<Section> split_sections(std::span<const double> bar_lufs);
// A drop by name contains "drop"; a break "break". With neither, sections
// are named by energy instead (rename_by_energy).
void rename_by_energy(std::vector<Section>& sections);
struct EnergyIssue {
  std::size_t earlier = 0; // Index of the louder reference section.
  std::size_t later = 0;   // The section at fault.
  double lu = 0.0;
  bool break_louder = false;
};
[[nodiscard]] std::optional<EnergyIssue> find_energy_issue(std::span<const Section> sections);

// ---- Masking -------------------------------------------------------------------------

// A track is judged where its character lives: bands within this of its own
// loudest band, above masking_low_hz (the lows belong to Lock and the sub).
inline constexpr double masking_character_db = 15.0;
inline constexpr double masking_low_hz = 150.0;
// It loses this much or more of its audibility there while the other plays.
inline constexpr double masking_threshold_db = 3.0;
// Both play when each hop is within activity_range_db of its own loudest.
inline constexpr double activity_range_db = 30.0;
// At least this long together to judge.
inline constexpr double masking_min_seconds = 4.0;

struct Masking {
  std::size_t first_band = 0;
  std::size_t last_band = 0;
  double loss_db = 0.0;
  double coactive_seconds = 0.0;
  std::size_t worst_hop = 0;
};
// How much `heard` loses in the bands the other covers, while both play:
// 10 log10(1 + P_other / P_heard), from gains-scaled per-hop band powers.
[[nodiscard]] std::optional<Masking> find_masking(const Profile& heard, double heard_gain,
                                                  const Profile& other, double other_gain);
// The same loss per band over co-active hops (0 where `heard` is not judged).
[[nodiscard]] std::array<double, band_count> masking_loss(const Profile& heard, double heard_gain,
                                                          const Profile& other, double other_gain);

// ---- Clean-up ------------------------------------------------------------------------

// Left and right should match within this per judged band.
inline constexpr double balance_threshold_db = 1.5;
// A mean this far from zero is a DC offset.
inline constexpr double dc_threshold = 0.003; // About -50 dBFS.
// Below this peak a track with clips is silent.
inline constexpr double silent_peak_db = -90.0;

// The judged band whose left and right differ most, and by how much (dB;
// positive is louder left).
struct Balance {
  double db = 0.0;
  std::size_t band = 0;
};
[[nodiscard]] Balance worst_balance(const Profile& profile);
[[nodiscard]] double dc_offset(const Profile& profile) noexcept;

// ---- Reference -----------------------------------------------------------------------

// The reference comparison's bands: name and edges (Hz).
struct ReferenceBand {
  const char* name;
  double low_hz;
  double high_hz;
};
inline constexpr std::array<ReferenceBand, 6> reference_bands{{
    {"Sub", 20.0, 60.0},
    {"Low", 60.0, 250.0},
    {"Low-mid", 250.0, 500.0},
    {"Mid", 500.0, 2'000.0},
    {"Presence", 2'000.0, 6'000.0},
    {"Air", 6'000.0, 20'000.0},
}};
struct ReferenceDifference {
  std::string band;
  double diff_db = 0.0;
};
// The mix minus the reference per band after matching their integrated
// loudness, then Width: their side-over-mid ratios' difference.
[[nodiscard]] std::vector<ReferenceDifference> compare_reference(const Profile& mix,
                                                                 double mix_lufs,
                                                                 const Profile& reference,
                                                                 double reference_lufs);

} // namespace oscillate::analysis::mix

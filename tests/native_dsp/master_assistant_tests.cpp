// The master assistant's measurements and proposals (ADR-0124), on
// synthetic programmes whose answers are known: loudness, peaks, crest,
// width and onsets read as built; each rule picks its device from the
// measurement it names; the plan lands on the loudness target under the
// ceiling and moves the balance toward the reference; the same input gives
// the same proposal; the offline chain lines up with its input.
//   oscillate_master_assistant_tests [case...]
#include "oscillate/native_dsp/master_assistant.hpp"

#include "oscillate/foundation/split_mix.hpp"
#include "oscillate/native_dsp/effects/ceiling.hpp"
#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace assistant = oscillate::native_dsp::assistant;
namespace tonal = oscillate::native_dsp::tonal;
namespace dsp = oscillate::native_dsp;
using oscillate::test::take;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr double rate_hz = oscillate::test::sample_rate_48k_hz;
constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr double seconds = 8.0;
constexpr std::uint64_t seed = 0x6d61'7374'6572'0124ULL;
// The level a pre-master mix sits at.
constexpr double mix_lufs = -20.0;
constexpr std::string_view target_id = "all";

// Tolerances.
constexpr double loudness_agreement_lu = 0.01;
constexpr double sine_crest_db = 3.0103; // A sine's peak over its RMS.
constexpr double crest_tolerance_db = 0.1;
constexpr double peak_tolerance_db = 0.05;
constexpr double width_tolerance_db = 1.0;
constexpr double onset_rate_tolerance = 0.25;
constexpr double alignment_tolerance = 1e-6;

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
};

std::size_t frames_of(double length) { return static_cast<std::size_t>(length * rate_hz); }

// Pink-ish noise (white through a -3 dB/oct one-pole cascade), stereo with
// `side` of uncorrelated noise in each channel.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the length, then the side's level.
Stereo pinkish(double length, double side, std::uint64_t salt = 0) {
  constexpr std::array poles{0.99886, 0.99332, 0.96900, 0.86650, 0.55000, -0.7616};
  constexpr std::array gains{0.0555179, 0.0750759, 0.1538520, 0.3104856, 0.5329522, -0.0168980};
  const auto n = frames_of(length);
  Stereo out{std::vector<float>(n), std::vector<float>(n)};
  oscillate::foundation::SplitMix64 random(seed + salt);
  std::array<double, poles.size()> state{};
  const auto pink = [&] {
    const double white = 2.0 * random.unit() - 1.0;
    double sum = 0.0;
    for (std::size_t k = 0; k < poles.size(); ++k) {
      state[k] = poles[k] * state[k] + gains[k] * white;
      sum += state[k];
    }
    return sum;
  };
  for (std::size_t i = 0; i < n; ++i) {
    const double mid = pink();
    const double l = side * (2.0 * random.unit() - 1.0);
    const double r = side * (2.0 * random.unit() - 1.0);
    out.left[i] = static_cast<float>(mid + l);
    out.right[i] = static_cast<float>(mid + r);
  }
  return out;
}

// A kick (a decaying 55 Hz sine) every `period` seconds over the noise.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the period, then the level.
void add_kicks(Stereo& s, double period, double level) {
  constexpr double kick_hz = 55.0;
  constexpr double decay_seconds = 0.12;
  const auto step = frames_of(period);
  const auto length = frames_of(decay_seconds * 6.0);
  for (std::size_t start = 0; start < s.left.size(); start += step) {
    for (std::size_t i = 0; i < length && start + i < s.left.size(); ++i) {
      const double t = static_cast<double>(i) / rate_hz;
      const auto x =
          static_cast<float>(level * std::exp(-t / decay_seconds) * std::sin(two_pi * kick_hz * t));
      s.left[start + i] += x;
      s.right[start + i] += x;
    }
  }
}

double lufs(const Stereo& s) { return assistant::analyse(s.left, s.right, rate).lufs(); }

void scale_to(Stereo& s, double target) {
  const auto gain = static_cast<float>(dsp::dsp::db_to_gain(target - lufs(s)));
  for (auto& x : s.left)
    x *= gain;
  for (auto& x : s.right)
    x *= gain;
}

// A drum-and-noise mix at the pre-master level.
Stereo mix_programme() {
  constexpr double side = 0.15;
  constexpr double noise_level = 0.25;
  constexpr double beat_seconds = 0.5;
  constexpr double kick_level = 3.0;
  auto s = pinkish(seconds, side);
  for (auto* channel : {&s.left, &s.right})
    for (auto& x : *channel)
      x *= static_cast<float>(noise_level);
  add_kicks(s, beat_seconds, kick_level);
  scale_to(s, mix_lufs);
  return s;
}

oscillate::foundation::Result<std::unique_ptr<assistant::Store>> memory(std::uint32_t r) {
  return oscillate::foundation::Result<std::unique_ptr<assistant::Store>>::success(
      std::make_unique<assistant::MemoryStore>(r));
}

const assistant::Reference& reference() {
  static const auto r = assistant::reference_from(*tonal::find_factory_target(target_id));
  return r;
}

const assistant::Device* device(const assistant::Proposal& p, std::string_view engine) {
  const auto found = std::ranges::find(p.devices, engine, &assistant::Device::engine);
  return found == p.devices.end() ? nullptr : &*found;
}

double value(const assistant::Device& d, std::string_view id) {
  const auto found = std::ranges::find(d.values, id, &std::pair<std::string, double>::first);
  REQUIRE_TRUE(found != d.values.end(), id);
  return found->second;
}

// ---- Measurements --------------------------------------------------------------------

// Loudness agrees with the project's loudness analyser; a sine's true peak
// and crest are its amplitude and 3 dB.
void loudness_peak_and_crest_read_as_built() {
  const auto noise = mix_programme();
  dsp::IntegratedLoudnessAnalyzer reference_meter(rate, dsp::LoudnessLayout::stereo);
  std::vector<float> interleaved;
  for (std::size_t i = 0; i < noise.left.size(); ++i) {
    interleaved.push_back(noise.left[i]);
    interleaved.push_back(noise.right[i]);
  }
  REQUIRE_EQ(reference_meter.append(interleaved), dsp::LoudnessStatus::ok, "the meter takes it");
  const auto measured = assistant::analyse(noise.left, noise.right, rate);
  REQUIRE_TRUE(measured.integrated_lufs, "integrated loudness");
  REQUIRE_NEAR(measured.integrated_lufs.value(), reference_meter.finish().integrated_lkfs.value(),
               loudness_agreement_lu, "the same BS.1770 loudness");
  REQUIRE_GE(measured.max_short_term_lufs, *measured.integrated_lufs, "short-term peaks over it");
  REQUIRE_GE(measured.max_momentary_lufs, measured.max_short_term_lufs, "momentary over that");

  constexpr double amplitude_db = -6.0;
  constexpr double tone_hz = 997.0;
  const auto n = frames_of(seconds);
  Stereo tone{std::vector<float>(n), std::vector<float>(n)};
  const double a = dsp::dsp::db_to_gain(amplitude_db);
  for (std::size_t i = 0; i < n; ++i)
    tone.left[i] = tone.right[i] =
        static_cast<float>(a * std::sin(two_pi * tone_hz * static_cast<double>(i) / rate_hz));
  const auto sine = assistant::analyse(tone.left, tone.right, rate);
  REQUIRE_NEAR(sine.true_peak_dbtp, amplitude_db, peak_tolerance_db, "a sine's true peak");
  REQUIRE_NEAR(sine.crest_db, sine_crest_db, crest_tolerance_db, "a sine's crest");
  REQUIRE_LT(sine.width_db[tonal::bands / 2], assistant::floor_db + 1.0, "mono has no side");
}

// Independent channels read 0 dB of width; side added only under 120 Hz
// reads in the low side; kicks every half second read two onsets a second
// in the lows.
void width_mono_bass_and_onsets_read_as_built() {
  const auto n = frames_of(seconds);
  Stereo wide{std::vector<float>(n), std::vector<float>(n)};
  oscillate::foundation::SplitMix64 random(seed);
  for (std::size_t i = 0; i < n; ++i) {
    wide.left[i] = static_cast<float>(2.0 * random.unit() - 1.0);
    wide.right[i] = static_cast<float>(2.0 * random.unit() - 1.0);
  }
  const auto w = assistant::analyse(wide.left, wide.right, rate);
  REQUIRE_NEAR(w.region[1].width_db, 0.0, width_tolerance_db,
               "independent channels are as wide as they are deep");

  auto lows = mix_programme();
  constexpr double bass_hz = 50.0;
  constexpr double bass_level = 0.2;
  for (std::size_t i = 0; i < lows.left.size(); ++i) {
    const auto x = static_cast<float>(
        bass_level * std::sin(two_pi * bass_hz * static_cast<double>(i) / rate_hz));
    lows.left[i] += x;
    lows.right[i] -= x;
  }
  const auto l = assistant::analyse(lows.left, lows.right, rate);
  REQUIRE_GT(l.low_side_db, assistant::vista_low_side_db, "out-of-phase bass is side");

  constexpr double beats_per_second = 2.0;
  const auto mix = mix_programme();
  const auto m = assistant::analyse(mix.left, mix.right, rate);
  REQUIRE_NEAR(m.region[0].onsets_per_second, beats_per_second, onset_rate_tolerance,
               "a kick every half second");
  REQUIRE_LT(m.low_side_db, assistant::vista_low_side_db, "mono kicks keep the lows mono");
}

// The distance ignores gain, names the band furthest out, and is zero on
// the target itself.
void tonal_distance_ignores_gain() {
  const auto& target = reference().target.median;
  REQUIRE_NEAR(assistant::tonal_distance(target, target).rms_db, 0.0, alignment_tolerance,
               "the target is its own");
  constexpr double offset_db = 7.0;
  constexpr double bump_db = 4.0;
  constexpr std::size_t bump_band = 20;
  auto moved = target;
  for (auto& b : moved)
    b += offset_db;
  moved[bump_band] += bump_db;
  const auto d = assistant::tonal_distance(moved, target);
  REQUIRE_EQ(d.worst_band, bump_band, "the bump is the worst band");
  REQUIRE_GT(d.worst_db, 0.0, "and it is over");
  REQUIRE_LT(d.rms_db, bump_db, "spread over the judged bands");
}

// Reference tracks' traits are their mean, and a target is built from
// their curves; a track with no loudness is left out.
void reference_tracks_average_their_traits() {
  constexpr double first_side_db = -20.0;
  constexpr double second_side_db = -30.0;
  constexpr double first_crest_db = 8.0;
  constexpr double second_crest_db = 12.0;
  assistant::Analysis first;
  first.integrated_lufs = mix_lufs;
  first.low_side_db = first_side_db;
  first.crest_db = first_crest_db;
  first.tonal = reference().target.median;
  auto second = first;
  second.low_side_db = second_side_db;
  second.crest_db = second_crest_db;
  auto silent = first;
  silent.integrated_lufs.reset();
  const std::array tracks{first, second, silent};
  const auto r = take(assistant::reference_from("references", tracks));
  REQUIRE_TRUE(r.traits.has_value(), "measured traits");
  REQUIRE_NEAR(r.traits.value().low_side_db, 0.5 * (first_side_db + second_side_db),
               alignment_tolerance, "the mean low side");
  REQUIRE_NEAR(r.traits.value().crest_db, 0.5 * (first_crest_db + second_crest_db),
               alignment_tolerance, "the mean crest");
  REQUIRE_EQ(r.target.references, 2, "the silent track is left out");
}

// The loudest window is where the programme is loudest.
void loudest_window_finds_the_drop() {
  auto s = mix_programme();
  constexpr double quiet_db = -20.0;
  const auto half = s.left.size() / 2;
  const auto g = static_cast<float>(dsp::dsp::db_to_gain(quiet_db));
  for (std::size_t i = 0; i < half; ++i) {
    s.left[i] *= g;
    s.right[i] *= g;
  }
  constexpr double window_seconds = 2.0;
  const auto a = assistant::analyse(s.left, s.right, rate);
  const auto w = assistant::loudest_window(a, window_seconds);
  REQUIRE_GE(w.first, half, "in the loud half");
  REQUIRE_EQ(w.frames, frames_of(window_seconds), "two seconds");
}

// ---- Rules ---------------------------------------------------------------------------

// Each rule fires from the measurement it names, and only then.
void rules_follow_the_measurements() {
  const auto mix = mix_programme();
  const auto a = assistant::analyse(mix.left, mix.right, rate);
  assistant::Goals clean{-14.0, -1.0, assistant::Character::clean, 0.0};
  const auto plain = assistant::derive(a, reference(), clean);
  REQUIRE_EQ(plain.devices.back().engine, "ceiling", "the limiter is last");
  REQUIRE_EQ(device(plain, "gild"), nullptr, "Clean adds no colour");
  REQUIRE_EQ(device(plain, "vista"), nullptr, "mono lows need no mono bass");
  REQUIRE_EQ(device(plain, "trim"), nullptr, "no trim under the target");
  for (const auto& d : plain.devices)
    REQUIRE_FALSE(d.reason.empty(), "every device says why: " + d.engine);
  const auto* ceiling = device(plain, "ceiling");
  REQUIRE_NEAR(value(*ceiling, "ceiling.ceiling"), clean.ceiling_dbtp, alignment_tolerance,
               "at the ceiling");

  auto warm = clean;
  warm.character = assistant::Character::warm;
  REQUIRE_NE(device(assistant::derive(a, reference(), warm), "gild"), nullptr, "Warm adds Gild");

  // Past the target: Trim takes the gain down before the chain.
  auto loud = mix;
  scale_to(loud, clean.lufs + 2.0);
  const auto over =
      assistant::derive(assistant::analyse(loud.left, loud.right, rate), reference(), clean);
  const auto* trim = device(over, "trim");
  REQUIRE_NE(trim, nullptr, "a trim");
  REQUIRE_LT(value(*trim, "trim.gain"), 0.0, "down");
  REQUIRE_EQ(over.devices.front().engine, "trim", "first");

  // A reference track twice as wide: Trim's width.
  auto wide_reference = reference();
  wide_reference.traits = assistant::Traits{};
  for (std::size_t r = 0; r < assistant::regions; ++r)
    wide_reference.traits->width_db[r] =
        a.region[r].width_db + 2.0 * assistant::width_min_change_db;
  const auto widened = assistant::derive(a, wide_reference, clean);
  REQUIRE_NE(device(widened, "trim"), nullptr, "Trim widens");
  REQUIRE_GT(value(*device(widened, "trim"), "trim.width"), 1.0, "wider");

  // Side in the lows: Vista's mono bass.
  auto lows = mix;
  constexpr double bass_hz = 50.0;
  constexpr double bass_level = 0.05;
  for (std::size_t i = 0; i < lows.left.size(); ++i) {
    const auto x = static_cast<float>(
        bass_level * std::sin(two_pi * bass_hz * static_cast<double>(i) / rate_hz));
    lows.left[i] += x;
    lows.right[i] -= x;
  }
  const auto mono =
      assistant::derive(assistant::analyse(lows.left, lows.right, rate), reference(), clean);
  const auto* vista = device(mono, "vista");
  REQUIRE_NE(vista, nullptr, "mono bass");
  REQUIRE_EQ(value(*vista, "vista.mono_bass"), 1.0, "on");

  // The tonal balance moving through the song: Keel, not Shape.
  auto moving = mix;
  constexpr double swing_db = 9.0;
  const auto segment = frames_of(assistant::segment_seconds);
  const auto darker = static_cast<float>(dsp::dsp::db_to_gain(-swing_db));
  float low_state = 0.0F;
  constexpr float smoothing = 0.995F;
  for (std::size_t i = 0; i < moving.left.size(); ++i) {
    low_state = smoothing * low_state + (1.0F - smoothing) * moving.left[i];
    if ((i / segment) % 2 == 1) {
      const float high = moving.left[i] - low_state;
      moving.left[i] = low_state + high * darker;
      moving.right[i] = moving.left[i];
    }
  }
  const auto moved = assistant::analyse(moving.left, moving.right, rate);
  REQUIRE_GE(moved.tonal_spread_db, assistant::keel_spread_db, "the balance moves");
  REQUIRE_NE(device(assistant::derive(moved, reference(), clean), "keel"), nullptr, "Keel");
  REQUIRE_LT(a.tonal_spread_db, assistant::keel_spread_db, "the steady mix stays put");
}

// A role is nudged only when one role makes most of a region that sits
// off the reference; a shared region is left to the EQ.
void nudges_need_a_clear_owner() {
  auto heavy = reference().target.median;
  constexpr double lows_over_db = 3.0;
  for (std::size_t b = 0; b < tonal::bands; ++b)
    if (tonal::band_hz(b) < assistant::low_split_hz)
      heavy[b] += lows_over_db;
  assistant::Analysis a;
  a.integrated_lufs = mix_lufs;
  a.tonal = heavy;
  constexpr std::size_t bass = 2;
  constexpr std::size_t music = 3;
  constexpr double most = 0.85;
  assistant::RolePowers owned;
  owned[bass] = assistant::RoleRegions{most, 0.1, 0.0};
  owned[music] = assistant::RoleRegions{1.0 - most, 0.9, 1.0};
  const assistant::Goals goals{};
  const auto p = assistant::derive(a, reference(), goals, owned);
  REQUIRE_EQ(p.nudges.size(), 1, "one nudge");
  REQUIRE_EQ(p.nudges.front().role, bass, "on the bass");
  REQUIRE_LT(p.nudges.front().gain_db, 0.0, "down");
  REQUIRE_GE(p.nudges.front().gain_db, -assistant::nudge_max_db, "small");

  constexpr double even = 0.5;
  assistant::RolePowers shared;
  shared[bass] = assistant::RoleRegions{even, 0.1, 0.0};
  shared[music] = assistant::RoleRegions{even, 0.9, 1.0};
  REQUIRE_TRUE(assistant::derive(a, reference(), goals, shared).nudges.empty(), "no owner");
}

// ---- Running a chain -----------------------------------------------------------------

// The chain's latency is taken out: Ceiling with no room under a far
// ceiling is a plain delay, so the output is the input.
void the_offline_chain_lines_up() {
  const auto mix = mix_programme();
  assistant::Device plain{"ceiling",
                          "Ceiling",
                          {{"ceiling.gain", 0.0},
                           {"ceiling.ceiling", 0.0},
                           {"ceiling.transients", 0.0},
                           {"ceiling.clip", 0.0}},
                          {}};
  auto chain = take(assistant::chain_from(std::span(&plain, 1)));
  REQUIRE_GT(chain.front().engine->latency_frames(chain.front().values, rate_hz), 0U,
             "Ceiling is latent");
  assistant::MemoryStore source(rate, mix.left, mix.right);
  assistant::Capture all;
  assistant::RunOptions options;
  options.window = assistant::Window{0, mix.left.size()};
  options.capture = &all;
  (void)take(assistant::run_chain(source, chain, options));
  REQUIRE_EQ(all.left.size(), mix.left.size(), "as long as the input");
  double worst = 0.0;
  for (std::size_t i = 0; i < mix.left.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::fabs(all.left[i] - mix.left[i])));
  REQUIRE_LT(worst, alignment_tolerance, "aligned sample for sample");
}

// ---- The plan ------------------------------------------------------------------------

// A loudness preset is reached within the tolerance under its ceiling, and
// the balance moves toward the reference.
void plan_reaches(std::size_t preset_index) {
  const auto& preset = assistant::loudness_presets[preset_index];
  const auto mix = mix_programme();
  const auto a = assistant::analyse(mix.left, mix.right, rate);
  const double before = assistant::tonal_distance(a.tonal, reference().target.median).rms_db;
  const assistant::Goals goals{preset.lufs, preset.ceiling_dbtp, assistant::Character::punchy, 0.0};
  assistant::MemoryStore source(rate, mix.left, mix.right);
  const auto planned = take(assistant::plan(source, a, reference(), goals, memory));
  const auto outcome = assistant::outcome(planned.learned.output, reference());
  REQUIRE_TRUE(assistant::meets(outcome, goals),
               std::string(preset.name) + ": " + std::to_string(outcome.lufs) + " LUFS, " +
                   std::to_string(outcome.true_peak_dbtp) + " dBTP");
  REQUIRE_LT(outcome.tonal_rms_db, before, std::string(preset.name) + ": toward the reference");
}
void plan_reaches_streaming() { plan_reaches(0); }
void plan_reaches_club() { plan_reaches(1); }
void plan_reaches_loud() { plan_reaches(2); }

// The same input plans the same proposal, word for word.
void the_same_input_plans_the_same() {
  const auto mix = mix_programme();
  const auto a = assistant::analyse(mix.left, mix.right, rate);
  const assistant::Goals goals{assistant::loudness_presets[1].lufs,
                               assistant::loudness_presets[1].ceiling_dbtp,
                               assistant::Character::warm, 0.0};
  assistant::MemoryStore source(rate, mix.left, mix.right);
  const auto planned = take(assistant::plan(source, a, reference(), goals, memory));
  const auto again = take(assistant::plan(source, a, reference(), goals, memory));
  REQUIRE_EQ(again.proposal.devices.size(), planned.proposal.devices.size(), "deterministic");
  for (std::size_t d = 0; d < planned.proposal.devices.size(); ++d) {
    REQUIRE_EQ(again.proposal.devices[d].engine, planned.proposal.devices[d].engine,
               "the same devices");
    REQUIRE_TRUE(again.proposal.devices[d].values == planned.proposal.devices[d].values,
                 "the same settings");
    REQUIRE_EQ(again.proposal.devices[d].reason, planned.proposal.devices[d].reason,
               "the same words");
  }
  REQUIRE_TRUE(again.proposal.notes == planned.proposal.notes, "the same notes");
}

// After the master chain (the master fader) the loudness and the ceiling
// still hold: the chain aims under them by the fader's gain.
void the_master_fader_is_allowed_for() {
  const auto mix = mix_programme();
  const auto a = assistant::analyse(mix.left, mix.right, rate);
  constexpr double fader_db = 2.0;
  const assistant::Goals goals{-14.0, -1.0, assistant::Character::clean, fader_db};
  assistant::MemoryStore source(rate, mix.left, mix.right);
  const auto planned = take(assistant::plan(source, a, reference(), goals, memory));
  REQUIRE_TRUE(assistant::meets(assistant::outcome(planned.learned.output, reference()), goals),
               "on target after the fader");
  const auto* ceiling = device(planned.proposal, "ceiling");
  REQUIRE_NEAR(value(*ceiling, "ceiling.ceiling"), goals.ceiling_dbtp - fader_db,
               alignment_tolerance, "the ceiling allows for the fader");
}

// The louder of two is turned down to the quieter; the quieter is untouched.
void level_matching_turns_the_louder_down() {
  constexpr double quiet = -12.0;
  constexpr double loud = -9.0;
  REQUIRE_NEAR(assistant::matched_gain_db(loud, quiet), quiet - loud, alignment_tolerance,
               "down by the difference");
  REQUIRE_NEAR(assistant::matched_gain_db(quiet, loud), 0.0, alignment_tolerance, "untouched");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "master assistant contract passed",
      {{"loudness_peak_and_crest_read_as_built", loudness_peak_and_crest_read_as_built},
       {"width_mono_bass_and_onsets_read_as_built", width_mono_bass_and_onsets_read_as_built},
       {"tonal_distance_ignores_gain", tonal_distance_ignores_gain},
       {"reference_tracks_average_their_traits", reference_tracks_average_their_traits},
       {"loudest_window_finds_the_drop", loudest_window_finds_the_drop},
       {"rules_follow_the_measurements", rules_follow_the_measurements},
       {"nudges_need_a_clear_owner", nudges_need_a_clear_owner},
       {"the_offline_chain_lines_up", the_offline_chain_lines_up},
       {"plan_reaches_streaming", plan_reaches_streaming},
       {"plan_reaches_club", plan_reaches_club},
       {"plan_reaches_loud", plan_reaches_loud},
       {"the_same_input_plans_the_same", the_same_input_plans_the_same},
       {"the_master_fader_is_allowed_for", the_master_fader_is_allowed_for},
       {"level_matching_turns_the_louder_down", level_matching_turns_the_louder_down}});
}

#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/strike.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Strike (ADR-0118) against measured oracles: at zero settings one band
// passes the input bit for bit and several sum flat; the attack and sustain
// gains measured on synthetic hits match their settings at any input level;
// a steady tone is left alone; Attack Time sets how long the lift lasts and
// Sustain Time how much of a decay counts; each band's settings stay in
// their band; solo, stereo link, mix and clip; lookahead lifts the first
// cycle with the latency reported; block splits, export against live,
// hostile input, a bounded tail, restrained presets, a seek, an
// allocation-free callback and the panel's telemetry.
namespace {
using namespace effect_test;
namespace strike = oscillate::native_dsp::effects::strike;
using oscillate::native_dsp::dsp::db_to_gain;
using oscillate::native_dsp::dsp::gain_to_db;

const effects::EffectEngine& engine() { return strike::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr double milliseconds = 1'000.0;
constexpr std::size_t parameter_count = 25;
constexpr std::size_t preset_count = 7;
constexpr std::size_t one_band = 0;
constexpr std::size_t two_bands = 1;
constexpr std::size_t three_bands = 2;
constexpr std::size_t four_bands = 3;
constexpr std::size_t lookahead_off = 0;
constexpr std::size_t lookahead_2ms = 3;
constexpr std::size_t lookahead_5ms = 4;
constexpr double lookahead_5ms_seconds = 0.005;
constexpr double clip_on = 1.0;
constexpr double linear_phase = 1.0;

// The synthetic hit: a sine that starts at full level after silence and
// decays exponentially.
constexpr double hit_hz = 1'000.0;
constexpr double hit_decay_seconds = 0.05;
constexpr double onset_seconds = 0.05;
constexpr double render_seconds = 0.5;
// Input levels the shaping must not depend on (peak dBFS).
constexpr std::array levels_db{-40.0, -20.0, 0.0};
// The attack is read over the hit's first 20 ms, the sustain 200 ms in.
constexpr double attack_window_seconds = 0.02;
constexpr double sustain_read_seconds = 0.2;
// Gains are read where the input is within 20 dB of the hit's peak.
constexpr double strong_fraction = 0.1;
// Measured gains match the settings within this.
constexpr double setting_tolerance_db = 0.25;
// The same setting reads the same at every level within this.
constexpr double level_tolerance_db = 0.01;
// A band left at 0 dB, or a steady tone, moves by no more than this.
constexpr double untouched_db = 0.05;
constexpr double flat_db = 0.01;

std::vector<double> defaults() { return preset(table(), "Default"); }

Stereo drums(std::size_t count);

std::size_t frames(double seconds) { return static_cast<std::size_t>(seconds * rate); }

std::vector<double> with(std::vector<double> values, std::string_view id, double value) {
  values[at(id)] = value;
  return values;
}

std::string band_id(std::size_t band, std::string_view part) {
  return "strike.band_" + std::to_string(band + 1) + "." + std::string(part);
}

std::vector<double> shaped(double attack_db, double sustain_db) {
  return with(with(defaults(), "strike.band_1.attack", attack_db), "strike.band_1.sustain",
              sustain_db);
}

Stereo silence(std::size_t count) {
  return {std::vector<double>(count, 0.0), std::vector<double>(count, 0.0)};
}

// A hit at `from`: a sine at `hz` and `amplitude`, decaying by 1/e over
// `decay` seconds (a constant tone when decay is 0).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
Stereo hit(std::size_t count, std::size_t from, double amplitude, double hz = hit_hz,
           double decay = hit_decay_seconds) {
  Stereo out = silence(count);
  for (std::size_t i = from; i < count; ++i) {
    const double t = static_cast<double>(i - from) / rate;
    const double envelope = decay > 0.0 ? std::exp(-t / decay) : 1.0;
    out.left[i] = out.right[i] = amplitude * envelope * std::sin(2.0 * std::numbers::pi * hz * t);
  }
  return out;
}

Stereo added(Stereo a, const Stereo& b) {
  for (std::size_t i = 0; i < a.size(); ++i) {
    a.left[i] += b.left[i];
    a.right[i] += b.right[i];
  }
  return a;
}

Stereo render(const std::vector<double>& values, const Stereo& input,
              std::size_t block = oscillate::test::block_128) {
  auto runtime = make_effect(engine(), values, rate);
  return process(*runtime, input, {.block = block});
}

double rounded(double x) { return static_cast<double>(static_cast<float>(x)); }

// The gain (dB) applied to `x` over [from, to): out over the reference
// where the reference is strong, the reading furthest from 0 dB.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
double applied_db(const std::vector<double>& out, const std::vector<double>& reference,
                  std::size_t from, std::size_t to) {
  const double strong = strong_fraction * peak(reference, from, to);
  double extreme = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    const double in = rounded(reference[i]);
    if (std::fabs(in) < strong) {
      continue;
    }
    const double db = gain_to_db(out[i] / in);
    if (std::fabs(db) > std::fabs(extreme)) {
      extreme = db;
    }
  }
  return extreme;
}

// The gain applied at one moment: out over the reference at the strongest
// sample within half a cycle of `when`.
double applied_at_db(const std::vector<double>& out, const std::vector<double>& reference,
                     std::size_t when, double hz = hit_hz) {
  const auto half = static_cast<std::size_t>(rate / hz / 2.0) + 1;
  std::size_t best = when;
  for (std::size_t i = when; i < when + half; ++i) {
    if (std::fabs(reference[i]) > std::fabs(reference[best])) {
      best = i;
    }
  }
  return gain_to_db(out[best] / rounded(reference[best]));
}

void descriptor_registry_and_presets() {
  REQUIRE_EQ(effects::find_engine("strike"), &engine(), "Strike is registered");
  require(!engine().sidechain && !engine().generator, "Strike reads only its input");
  REQUIRE_EQ(table().parameters.size(), parameter_count, "the table's size");
  for (const auto* id : {"strike.bands", "strike.phase", "strike.lookahead", "strike.clip"}) {
    require(!table().parameters[at(id)].automatable, std::string(id) + " is not automatable");
  }
  REQUIRE_EQ(table().presets.size(), preset_count, "Default and six presets");
  for (const auto& entry : table().presets) {
    const auto out = render(preset(table(), entry.name), noise(0.5, frames(render_seconds)));
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]),
              "preset renders finite: " + std::string(entry.name));
    }
  }
}

// One band at zero settings is a wire: the input bit for bit, also through
// the lookahead (delayed by exactly its frames).
void zero_settings_pass_untouched() {
  const auto input = noise(0.5, frames(render_seconds));
  const auto out = render(defaults(), input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    require(out.left[i] == rounded(input.left[i]) && out.right[i] == rounded(input.right[i]),
            "zero settings pass the input untouched");
  }
  const auto values = with(defaults(), "strike.lookahead", static_cast<double>(lookahead_5ms));
  const auto delay = engine().latency_frames(values, rate);
  REQUIRE_EQ(delay, static_cast<std::uint32_t>(std::lround(lookahead_5ms_seconds * rate)),
             "five milliseconds ahead");
  const auto late = render(values, input);
  for (std::size_t i = delay; i < input.size(); ++i) {
    require(late.left[i] == rounded(input.left[i - delay]), "the lookahead only delays");
  }
}

// At zero settings the bands sum to one allpass: an impulse's spectrum is
// flat within 0.01 dB from 20 Hz to 20 kHz, for every band count.
void bands_sum_flat() {
  constexpr std::size_t length = 65'536;
  constexpr double impulse_at = 0.5;
  constexpr double lowest_hz = 20.0;
  constexpr double highest_hz = 20'000.0;
  for (const std::size_t bands : {two_bands, three_bands, four_bands}) {
    auto input = silence(length);
    input.left[0] = input.right[0] = impulse_at;
    const auto out = render(with(defaults(), "strike.bands", static_cast<double>(bands)), input);
    std::vector<std::complex<double>> spectrum(length);
    for (std::size_t i = 0; i < length; ++i) {
      spectrum[i] = out.left[i] / impulse_at;
    }
    fft(spectrum);
    for (std::size_t bin = 1; bin < length / 2; ++bin) {
      const double hz = static_cast<double>(bin) * rate / static_cast<double>(length);
      if (hz < lowest_hz || hz > highest_hz) {
        continue;
      }
      REQUIRE_LT(std::fabs(gain_to_db(std::abs(spectrum[bin]))), flat_db,
                 "flat at " + std::to_string(hz) + " Hz with " + std::to_string(bands + 1) +
                     " bands");
    }
  }
}

// The attack gain, read over the hit's onset, is its setting at every
// level: the detector reads ratios, not levels.
void attack_gain_matches_its_setting_at_any_level() {
  const auto count = frames(render_seconds);
  const auto onset = frames(onset_seconds);
  const auto window = onset + frames(attack_window_seconds);
  for (const double setting : {6.0, -6.0, 12.0}) {
    std::vector<double> readings;
    for (const double level : levels_db) {
      const auto input = hit(count, onset, db_to_gain(level));
      const auto out = render(shaped(setting, 0.0), input);
      readings.push_back(applied_db(out.left, input.left, onset, window));
      REQUIRE_LT(std::fabs(readings.back() - setting), setting_tolerance_db,
                 "attack " + std::to_string(setting) + " dB at " + std::to_string(level) +
                     " dBFS reads " + std::to_string(readings.back()));
    }
    for (const double reading : readings) {
      REQUIRE_LT(std::fabs(reading - readings.front()), level_tolerance_db,
                 "the attack does not depend on the level");
    }
  }
}

// The sustain gain, read in the hit's decay, is its setting at every level.
void sustain_gain_matches_its_setting_at_any_level() {
  const auto count = frames(render_seconds);
  const auto onset = frames(onset_seconds);
  const auto read = onset + frames(sustain_read_seconds);
  for (const double setting : {-6.0, 6.0, -12.0}) {
    std::vector<double> readings;
    for (const double level : levels_db) {
      const auto input = hit(count, onset, db_to_gain(level));
      const auto out = render(shaped(0.0, setting), input);
      readings.push_back(applied_at_db(out.left, input.left, read));
      REQUIRE_LT(std::fabs(readings.back() - setting), setting_tolerance_db,
                 "sustain " + std::to_string(setting) + " dB at " + std::to_string(level) +
                     " dBFS reads " + std::to_string(readings.back()));
    }
    for (const double reading : readings) {
      REQUIRE_LT(std::fabs(reading - readings.front()), level_tolerance_db,
                 "the sustain does not depend on the level");
    }
    // The attack is left alone.
    const auto input = hit(count, onset, 1.0);
    const auto out = render(shaped(0.0, setting), input);
    const auto first_cycle = onset + static_cast<std::size_t>(rate / hit_hz);
    REQUIRE_LT(std::fabs(applied_db(out.left, input.left, onset, first_cycle)), untouched_db,
               "sustain leaves the onset alone");
  }
}

// A steady tone has no attack and no decay: after it has settled the gain
// is unity whatever the settings, at 40 Hz as at 10 kHz.
void a_steady_tone_is_left_alone() {
  constexpr double strong_db = 12.0;
  constexpr double settled_seconds = 0.3;
  constexpr double tone_level = 0.5;
  const auto count = frames(render_seconds);
  for (const double hz : {40.0, 1'000.0, 10'000.0}) {
    const auto input = hit(count, 0, tone_level, hz, 0.0);
    for (const double sign : {1.0, -1.0}) {
      const auto out = render(shaped(sign * strong_db, sign * strong_db), input);
      REQUIRE_LT(std::fabs(applied_db(out.left, input.left, frames(settled_seconds), count)),
                 untouched_db, "a steady " + std::to_string(hz) + " Hz tone is left alone");
    }
  }
}

// How long the attack gain stays over half its setting on a tone that
// starts at full level.
double lift_seconds(double attack_time_ms) {
  constexpr double setting_db = 6.0;
  const auto count = frames(render_seconds);
  const auto onset = frames(onset_seconds);
  const auto input = hit(count, onset, 1.0, hit_hz, 0.0);
  const auto out =
      render(with(shaped(setting_db, 0.0), "strike.attack_time", attack_time_ms), input);
  const auto period = static_cast<std::size_t>(rate / hit_hz);
  std::size_t lifted = 0;
  for (std::size_t cycle = onset; cycle + period < count; cycle += period) {
    if (applied_at_db(out.left, input.left, cycle) > setting_db / 2.0) {
      lifted += period;
    }
  }
  return static_cast<double>(lifted) / rate;
}

// Attack Time sets how long the lift lasts: about a third to the whole of
// it, growing with it.
void attack_time_sets_the_length_of_the_lift() {
  constexpr std::array times_ms{2.0, 10.0, 50.0};
  constexpr double least_share = 0.3;
  constexpr double most_share = 1.0;
  double previous = 0.0;
  for (const double time_ms : times_ms) {
    const double lift = lift_seconds(time_ms);
    REQUIRE_GT(lift, previous, "a longer Attack Time lifts longer");
    REQUIRE_GE(lift, least_share * time_ms / milliseconds, "the lift's length");
    REQUIRE_LE(lift, most_share * time_ms / milliseconds, "and no longer than Attack Time");
    previous = lift;
  }
}

// Sustain Time is how fast a decay must fall to count as sustain: a hit
// decaying over 50 ms barely counts under a 30 ms Sustain Time and fully
// under 300 ms or longer.
void sustain_time_sets_how_much_decay_counts() {
  constexpr double setting_db = -6.0;
  constexpr double read_seconds = 0.1;
  constexpr double short_ms = 30.0;
  constexpr std::array times_ms{short_ms, 300.0, 2'000.0};
  const auto count = frames(render_seconds);
  const auto onset = frames(onset_seconds);
  const auto input = hit(count, onset, 1.0);
  double previous = 0.0;
  for (const double time_ms : times_ms) {
    const auto out = render(with(shaped(0.0, setting_db), "strike.sustain_time", time_ms), input);
    const double reading = applied_at_db(out.left, input.left, onset + frames(read_seconds));
    REQUIRE_LE(reading, previous + tolerance::float_processing, "longer counts more");
    if (time_ms == short_ms) {
      REQUIRE_GT(reading, setting_db / 2.0, "a short Sustain Time barely counts the decay");
    } else {
      REQUIRE_LT(std::fabs(reading - setting_db), setting_tolerance_db,
                 "a long one counts all of it");
    }
    previous = reading;
  }
}

// The gain (dB) applied to a tone at `hz` over [from, to): its peak in each
// half cycle against the reference's, the reading furthest from 0 dB. Half
// cycles keep a split band's onset, shared with its neighbours, from
// reading as gain; half cycles where the reference is weak are skipped.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
double peak_ratio_db(const std::vector<double>& out, const std::vector<double>& reference,
                     std::size_t from, std::size_t to, double hz) {
  const auto half = static_cast<std::size_t>(rate / hz / 2.0);
  const double strong = strong_fraction * peak(reference, from, to);
  double extreme = 0.0;
  for (std::size_t start = from; start + half <= to; start += half) {
    if (peak(reference, start, start + half) < strong) {
      continue;
    }
    const double db =
        gain_to_db(peak(out, start, start + half) / peak(reference, start, start + half));
    if (std::fabs(db) > std::fabs(extreme)) {
      extreme = db;
    }
  }
  return extreme;
}

// A hit that fades in over `ramp_cycles` cycles (a half Hann) and then
// decays: band-limited, so its onset stays in its own band.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
Stereo soft_hit(std::size_t count, std::size_t from, double amplitude, double hz) {
  constexpr double ramp_cycles = 8.0;
  const double ramp = ramp_cycles / hz;
  Stereo out = silence(count);
  for (std::size_t i = from; i < count; ++i) {
    const double t = static_cast<double>(i - from) / rate;
    const double envelope = t < ramp ? oscillate::native_dsp::dsp::hann(t, 2.0 * ramp)
                                     : std::exp(-(t - ramp) / hit_decay_seconds);
    out.left[i] = out.right[i] = amplitude * envelope * std::sin(2.0 * std::numbers::pi * hz * t);
  }
  return out;
}

// The analogue magnitude of band `band` of a fourth-order Linkwitz-Riley
// tree split at `crossovers` for a tone at `hz`: every crossover below the
// band a high-pass and its own a low-pass, each 1 / (1 + (f / fc)^4).
double skirt(double hz, std::span<const double> crossovers, std::size_t band) {
  constexpr double order = 4.0;
  double magnitude = 1.0;
  for (std::size_t k = 0; k < crossovers.size() && k <= band; ++k) {
    const double ratio = k < band ? crossovers[k] / hz : hz / crossovers[k];
    magnitude /= 1.0 + std::pow(ratio, order);
  }
  return magnitude;
}

// Four bands at their default crossovers (120 Hz, 1 kHz, 6 kHz): a hit in
// each band, apart in time. Lifting one band's attack lifts its hit by the
// setting and reaches a band-limited hit in another band only through the
// crossovers' skirts (an abrupt onset is a click that reaches every band).
void band_settings_stay_in_their_band() {
  constexpr std::array band_hz{50.0, 350.0, 2'500.0, 12'000.0};
  constexpr std::array crossovers{120.0, 1'000.0, 6'000.0};
  constexpr double setting_db = 9.0;
  constexpr double spacing_seconds = 0.4;
  constexpr double hit_level = 0.25;
  // The abrupt onset shares its first half cycle with the neighbouring
  // bands, which the lift does not reach.
  constexpr double band_tolerance_db = 0.75;
  // Long enough to lift the 50 Hz hit's first cycles.
  constexpr double attack_time_ms = 50.0;
  const auto spacing = frames(spacing_seconds);
  const auto count = spacing * band_hz.size() + frames(onset_seconds);
  Stereo abrupt = silence(count);
  Stereo soft = silence(count);
  for (std::size_t b = 0; b < band_hz.size(); ++b) {
    const auto onset = frames(onset_seconds) + b * spacing;
    abrupt = added(abrupt, hit(count, onset, hit_level, band_hz[b]));
    soft = added(soft, soft_hit(count, onset, hit_level, band_hz[b]));
  }
  auto split = with(defaults(), "strike.bands", static_cast<double>(four_bands));
  split[at("strike.attack_time")] = attack_time_ms;
  const auto abrupt_reference = render(split, abrupt);
  const auto soft_reference = render(split, soft);
  for (std::size_t lifted = 0; lifted < band_hz.size(); ++lifted) {
    const auto values = with(split, band_id(lifted, "attack"), setting_db);
    const auto lifted_out = render(values, abrupt);
    const auto soft_out = render(values, soft);
    for (std::size_t b = 0; b < band_hz.size(); ++b) {
      const auto onset = frames(onset_seconds) + b * spacing;
      if (b == lifted) {
        const double reading = peak_ratio_db(lifted_out.left, abrupt_reference.left, onset,
                                             onset + spacing / 2, band_hz[b]);
        REQUIRE_LT(std::fabs(reading - setting_db), band_tolerance_db,
                   "band " + std::to_string(b + 1) + " lifts its hit: " + std::to_string(reading));
      } else {
        const double reading = peak_ratio_db(soft_out.left, soft_reference.left, onset,
                                             onset + spacing / 2, band_hz[b]);
        const double leak = skirt(band_hz[b], crossovers, lifted);
        const double reach_db = gain_to_db(1.0 + leak * (db_to_gain(setting_db) - 1.0));
        REQUIRE_LT(std::fabs(reading), reach_db + untouched_db,
                   "band " + std::to_string(lifted + 1) + " reaches band " + std::to_string(b + 1) +
                       "'s hit only through the skirt: " + std::to_string(reading));
      }
    }
  }
}

// The amplitude of `hz` in x over [from, from + length) (one DFT bin).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
double amplitude_of(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  std::complex<double> sum{};
  for (std::size_t i = 0; i < length; ++i) {
    sum +=
        x[from + i] * std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
  }
  return 2.0 * std::abs(sum) / static_cast<double>(length);
}

// Solo plays only its band: steady tones in all four, band 2 soloed.
void solo_plays_only_its_band() {
  // Whole cycles in the window for every tone, each at least 1.75 octaves
  // from band 2's crossovers (120 Hz and 1 kHz), where they fall 40 dB.
  constexpr std::array band_hz{25.0, 350.0, 4'000.0, 16'000.0};
  // The soloed tone sits between two crossovers' skirts.
  constexpr double skirt_db = 0.5;
  constexpr double tone_level = 0.2;
  constexpr std::size_t soloed = 1;
  constexpr double muted_db = -40.0;
  constexpr double window_seconds = 0.2;
  const auto count = frames(render_seconds);
  Stereo input = silence(count);
  for (const double hz : band_hz) {
    input = added(input, hit(count, 0, tone_level, hz, 0.0));
  }
  auto values = with(defaults(), "strike.bands", static_cast<double>(four_bands));
  values[at(band_id(soloed, "solo"))] = 1.0;
  const auto out = render(values, input);
  const auto from = count - frames(window_seconds);
  for (std::size_t b = 0; b < band_hz.size(); ++b) {
    const double level =
        gain_to_db(amplitude_of(out.left, from, frames(window_seconds), band_hz[b]) / tone_level);
    if (b == soloed) {
      REQUIRE_LT(std::fabs(level), skirt_db, "the soloed band plays");
    } else {
      REQUIRE_LT(level, muted_db, "band " + std::to_string(b + 1) + " is silent under a solo");
    }
  }
}

// Linked, a hit on the left lifts the right as much; unlinked, the right's
// steady tone is left alone.
void stereo_link_shares_the_gain() {
  constexpr double setting_db = 6.0;
  constexpr double bed_level = 0.1;
  constexpr double bed_hz = 300.0;
  const auto count = frames(render_seconds);
  const auto onset = frames(onset_seconds);
  const auto window = onset + frames(attack_window_seconds);
  const auto left = hit(count, onset, 1.0);
  const auto right = hit(count, 0, bed_level, bed_hz, 0.0);
  const Stereo input{left.left, right.right};
  const auto linked = render(shaped(setting_db, 0.0), input);
  REQUIRE_LT(std::fabs(applied_db(linked.right, input.right, onset, window) - setting_db),
             setting_tolerance_db, "linked, the right follows the left's hit");
  const auto apart = render(with(shaped(setting_db, 0.0), "strike.link", 0.0), input);
  REQUIRE_LT(std::fabs(applied_db(apart.right, input.right, onset, window)), untouched_db,
             "unlinked, the right is left alone");
  REQUIRE_LT(std::fabs(applied_db(apart.left, input.left, onset, window) - setting_db),
             setting_tolerance_db, "while the left still lifts");
}

// Mix blends with the dry bands: half mix of a 6 dB lift is the mean of
// the two gains; no mix is the dry signal exactly.
void mix_blends_with_the_dry_bands() {
  constexpr double setting_db = 6.0;
  constexpr double half = 0.5;
  const auto count = frames(render_seconds);
  const auto onset = frames(onset_seconds);
  const auto window = onset + frames(attack_window_seconds);
  const auto input = hit(count, onset, 1.0);
  const auto half_mix = render(with(shaped(setting_db, 0.0), "strike.mix", half), input);
  const double expected = gain_to_db(1.0 + half * (db_to_gain(setting_db) - 1.0));
  REQUIRE_LT(std::fabs(applied_db(half_mix.left, input.left, onset, window) - expected),
             setting_tolerance_db, "half mix");
  const auto dry = render(with(shaped(setting_db, setting_db), "strike.mix", 0.0), input);
  for (std::size_t i = 0; i < count; ++i) {
    REQUIRE_EQ(dry.left[i], rounded(input.left[i]), "no mix is the input");
  }
}

// Clip keeps the lifted hits near the ceiling where, unclipped, they would
// overshoot it by the lift; under the ceiling it only delays.
void clip_holds_the_ceiling() {
  constexpr double setting_db = 12.0;
  constexpr double ceiling_db = -6.0;
  constexpr double hit_peak_db = -1.0;
  // The 4x filters ring a little over the curve's ceiling.
  constexpr double overshoot_db = 0.5;
  constexpr double quiet_level = 0.05;
  constexpr double passband_tolerance = 1e-3;
  const auto count = frames(render_seconds);
  const auto input = hit(count, frames(onset_seconds), db_to_gain(hit_peak_db));
  const auto loud = shaped(setting_db, 0.0);
  const auto open = render(loud, input);
  REQUIRE_GT(gain_to_db(peak(open.left)), ceiling_db + setting_db / 2.0,
             "unclipped, the lift overshoots");
  auto clipped = with(loud, "strike.clip", clip_on);
  clipped[at("strike.ceiling")] = ceiling_db;
  const auto held = render(clipped, input);
  REQUIRE_LT(gain_to_db(peak(held.left)), ceiling_db + overshoot_db,
             "clipped under the ceiling: " + std::to_string(gain_to_db(peak(held.left))));
  const auto quiet = hit(count, 0, quiet_level, hit_hz, 0.0);
  const auto through = render(with(defaults(), "strike.clip", clip_on), quiet);
  const auto delay = engine().latency_frames(with(defaults(), "strike.clip", clip_on), rate);
  for (std::size_t i = count / 2; i < count; ++i) {
    REQUIRE_LT(std::fabs(through.left[i] - quiet.left[i - delay]), passband_tolerance,
               "under the ceiling clip only delays");
  }
}

// Every combination's latency is the one reported, and an impulse comes
// out exactly that late.
void latency_is_reported_and_exact() {
  constexpr std::size_t length = 8'192;
  constexpr std::size_t impulse_at = 100;
  constexpr double impulse = 0.5;
  for (const std::size_t lookahead : {lookahead_off, lookahead_2ms, lookahead_5ms}) {
    for (const double phase : {0.0, linear_phase}) {
      for (const double clip : {0.0, clip_on}) {
        auto values = with(defaults(), "strike.lookahead", static_cast<double>(lookahead));
        values[at("strike.phase")] = phase;
        values[at("strike.clip")] = clip;
        values[at("strike.bands")] = static_cast<double>(three_bands);
        const auto latency = engine().latency_frames(values, rate);
        REQUIRE_EQ(latency, strike::latency_frames(lookahead, phase != 0.0, clip != 0.0, rate),
                   "the engine reports the header's latency");
        auto runtime = make_effect(engine(), values, rate);
        REQUIRE_EQ(runtime->latency_frames(), latency, "the runtime reports it");
        auto input = silence(length);
        input.left[impulse_at] = input.right[impulse_at] = impulse;
        const auto out = process(*runtime, input);
        std::size_t loudest = 0;
        for (std::size_t i = 0; i < length; ++i) {
          if (std::fabs(out.left[i]) > std::fabs(out.left[loudest])) {
            loudest = i;
          }
        }
        if (phase == 0.0) {
          // The minimum-phase split spreads the impulse; its peak still
          // leads the allpass's ring.
          REQUIRE_GE(loudest, impulse_at + latency, "the impulse arrives no earlier");
        } else {
          REQUIRE_EQ(loudest, impulse_at + latency, "the impulse arrives that late");
        }
      }
    }
  }
}

// Linear phase: at zero settings the bands sum to the input, delayed by the
// bank's latency, for every band count.
void linear_phase_sums_to_the_input_delayed() {
  constexpr double sum_tolerance = 1e-6;
  const auto input = drums(frames(render_seconds));
  for (const std::size_t bands : {one_band, two_bands, three_bands, four_bands}) {
    auto values = with(defaults(), "strike.phase", linear_phase);
    values[at("strike.bands")] = static_cast<double>(bands);
    const auto delay = engine().latency_frames(values, rate);
    const auto out = render(values, input);
    for (std::size_t i = delay; i < input.size(); ++i) {
      REQUIRE_LT(std::fabs(out.left[i] - rounded(input.left[i - delay])), sum_tolerance,
                 "the bands sum to the input delayed: " + std::to_string(bands + 1) + " bands");
    }
  }
}

// Lookahead lets the gain arrive with the hit: the very first cycle takes
// nearly all of the lift, where without it the detector is still rising.
void lookahead_lifts_the_first_cycle() {
  constexpr double setting_db = 9.0;
  constexpr double nearly_db = 1.0;
  const auto count = frames(render_seconds);
  const auto onset = frames(onset_seconds);
  const auto input = hit(count, onset, 1.0);
  const auto ahead_values =
      with(shaped(setting_db, 0.0), "strike.lookahead", static_cast<double>(lookahead_2ms));
  const auto delay = engine().latency_frames(ahead_values, rate);
  const auto ahead = render(ahead_values, input);
  const auto now = render(shaped(setting_db, 0.0), input);
  Stereo shifted = silence(count);
  for (std::size_t i = delay; i < count; ++i) {
    shifted.left[i - delay] = ahead.left[i];
  }
  const double first_ahead = applied_at_db(shifted.left, input.left, onset);
  const double first_now = applied_at_db(now.left, input.left, onset);
  REQUIRE_GT(first_ahead, setting_db - nearly_db, "with lookahead the first cycle is lifted");
  REQUIRE_LT(first_now, first_ahead - nearly_db, "without it the first cycle is lifted less");
}

// Drum-like hits every quarter second over a little noise.
Stereo drums(std::size_t count) {
  constexpr double body_hz = 180.0;
  constexpr double decay = 0.05;
  constexpr double bleed = 0.003;
  constexpr double hit_seconds = 0.25;
  constexpr double level = 0.5;
  const auto period = frames(hit_seconds);
  Stereo out = noise(bleed, count);
  for (std::size_t i = 0; i < count; ++i) {
    const double t = static_cast<double>(i % period) / rate;
    const double x = level * std::exp(-t / decay) * std::sin(2.0 * std::numbers::pi * body_hz * t);
    out.left[i] += x;
    out.right[i] += x;
  }
  return out;
}

std::vector<std::vector<double>> varied() {
  std::vector<std::vector<double>> result;
  for (const auto& entry : table().presets) {
    result.push_back(preset(table(), entry.name));
  }
  auto soloed = preset(table(), "Master Impact");
  constexpr double partial = 0.4;
  soloed[at(band_id(2, "solo"))] = 1.0;
  soloed[at("strike.link")] = partial;
  soloed[at("strike.mix")] = partial;
  result.push_back(soloed);
  return result;
}

void any_block_split_renders_the_same() {
  constexpr double seconds = 1.0;
  const auto input = drums(frames(seconds));
  for (const auto& v : varied()) {
    const auto a = render(v, input, oscillate::test::block_128);
    const auto b = render(v, input, 0);
    require(a.left == b.left && a.right == b.right, "the split changes nothing");
  }
}

// Export renders in long slices on its own transport generation, stopped;
// live in callback blocks while playing. The audio is the same.
void export_equals_live() {
  constexpr std::size_t export_block = 4'096;
  constexpr double live_bpm = 124.0;
  constexpr double seconds = 1.0;
  const auto input = drums(frames(seconds));
  for (const auto& v : varied()) {
    auto live = make_effect(engine(), v, rate);
    const auto played =
        process(*live, input, {.block = oscillate::test::block_128, .bpm = live_bpm});
    auto offline = make_effect(engine(), v, rate);
    const auto exported =
        process(*offline, input, {.block = export_block, .generation = 2, .bpm = 0.0});
    require(played.left == exported.left && played.right == exported.right, "export equals live");
  }
}

void hostile_input_stays_finite() {
  constexpr std::size_t nan_at = 100;
  constexpr std::size_t infinity_at = 200;
  constexpr std::size_t huge_at = 300;
  constexpr double huge = 1e30;
  constexpr double tiny = 1e-30;
  constexpr std::size_t tiny_from = 4'000;
  Stereo input = drums(frames(render_seconds));
  input.left[nan_at] = std::numeric_limits<double>::quiet_NaN();
  input.right[infinity_at] = std::numeric_limits<double>::infinity();
  input.left[huge_at] = huge;
  for (std::size_t i = tiny_from; i < input.size(); ++i) {
    input.left[i] = input.right[i] = tiny;
  }
  for (const auto& v : varied()) {
    const auto out = render(v, input);
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]), "finite output");
    }
  }
}

// After the input stops, the output falls silent within the tail reported.
void the_tail_is_bounded() {
  constexpr double silent = 1e-6; // -120 dBFS
  constexpr double seconds = 0.5;
  const auto played = frames(seconds);
  for (const auto& v : varied()) {
    const auto tail = engine().tail_frames(v, rate) + engine().latency_frames(v, rate);
    auto input = drums(played + static_cast<std::size_t>(tail) + frames(seconds));
    for (std::size_t i = played; i < input.size(); ++i) {
      input.left[i] = input.right[i] = 0.0;
    }
    const auto out = render(v, input);
    REQUIRE_LT(peak(out.left, played + static_cast<std::size_t>(tail)), silent,
               "silent after the tail");
  }
}

// Onset contrast (as tools/limiter_audit measures it): the energy of a
// hit's first 15 ms over that of 30-150 ms on, in dB.
double contrast_db(const std::vector<double>& x, std::size_t onset) {
  constexpr double attack_seconds = 0.015;
  constexpr double body_from_seconds = 0.03;
  constexpr double body_to_seconds = 0.15;
  const auto energy = [&](std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
      sum += x[i] * x[i];
    }
    return sum / static_cast<double>(to - from);
  };
  const double attack = energy(onset, onset + frames(attack_seconds));
  const double body = energy(onset + frames(body_from_seconds), onset + frames(body_to_seconds));
  return oscillate::native_dsp::dsp::power_to_db(attack / body);
}

// The material each preset is for: hits of a tone and decay every half
// second over a quiet room, peaking at `peak_db`.
struct Source {
  const char* preset;
  double hz;
  double decay;
  double peak_db;
  // +1 when the preset should sharpen the hits, -1 when it should soften
  // them.
  double direction;
  // The least change that counts as doing something: a master preset moves
  // by fractions of a decibel.
  double least_db;
};

void presets_stay_restrained() {
  constexpr double track_least_db = 1.0;
  constexpr double master_least_db = 0.3;
  constexpr double most_change_db = 10.0;
  constexpr double hit_seconds = 0.5;
  constexpr std::size_t periods = 4;
  constexpr double room = 0.002;
  constexpr double kick_hz = 55.0;
  constexpr double snare_hz = 200.0;
  constexpr double mix_hz = 200.0;
  constexpr double soft_hz = 900.0;
  constexpr double short_decay = 0.04;
  constexpr double room_decay = 0.12;
  constexpr double bus_peak_db = -6.0;
  constexpr double master_peak_db = -6.0;
  constexpr double track_peak_db = -6.0;
  // The 4x filters ring a little over the curve's ceiling.
  constexpr double overshoot_db = 0.5;
  const std::array<Source, 6> sources{{
      {"Kick Snap", kick_hz, short_decay, track_peak_db, 1.0, track_least_db},
      {"Snare Crack", snare_hz, short_decay, track_peak_db, 1.0, track_least_db},
      {"Tighten Room", snare_hz, room_decay, track_peak_db, 1.0, track_least_db},
      {"Drum Bus Punch", mix_hz, short_decay, bus_peak_db, 1.0, track_least_db},
      {"Master Impact", mix_hz, short_decay, master_peak_db, 1.0, master_least_db},
      {"Soften Attack", soft_hz, short_decay, track_peak_db, -1.0, track_least_db},
  }};
  const auto period = frames(hit_seconds);
  const auto count = period * periods;
  for (const auto& source : sources) {
    const auto values = preset(table(), source.preset);
    Stereo hits = noise(room, count);
    for (std::size_t i = 0; i < count; ++i) {
      const double t = static_cast<double>(i % period) / rate;
      const double x = db_to_gain(source.peak_db) * std::exp(-t / source.decay) *
                       std::sin(2.0 * std::numbers::pi * source.hz * t);
      hits.left[i] += x;
      hits.right[i] += x;
    }
    const auto latency = engine().latency_frames(values, rate);
    const auto out = render(values, hits);
    std::vector<double> aligned(out.left.begin() + latency, out.left.end());
    aligned.resize(count, 0.0);
    for (std::size_t p = 1; p + 1 < periods; ++p) {
      const auto onset = p * period;
      const double change =
          source.direction * (contrast_db(aligned, onset) - contrast_db(hits.left, onset));
      REQUIRE_GT(change, source.least_db,
                 std::string(source.preset) +
                     " moves the contrast its way: " + std::to_string(change));
      REQUIRE_LT(change, most_change_db,
                 std::string(source.preset) + " by no more than 10 dB: " + std::to_string(change));
    }
    if (values[at("strike.clip")] != 0.0) {
      REQUIRE_LT(gain_to_db(peak(out.left)), values[at("strike.ceiling")] + overshoot_db,
                 std::string(source.preset) + " stays under its ceiling");
    }
  }
}

// A seek (a new transport generation) clears every history: after it
// Strike plays as a fresh one.
void a_seek_starts_clean() {
  constexpr std::uint32_t other_seed = 99;
  const auto before = drums(frames(render_seconds));
  const auto after = noise(0.5, frames(render_seconds), true, other_seed);
  const auto v = preset(table(), "Drum Bus Punch");
  auto seeked = make_effect(engine(), v, rate);
  (void)process(*seeked, before);
  const auto resumed = process(*seeked, after, {.generation = 2});
  const auto fresh = render(v, after);
  require(resumed.left == fresh.left && resumed.right == fresh.right,
          "after a seek Strike plays as a fresh one");
}

void the_callback_never_allocates() {
  constexpr std::uint64_t blocks = 2'000;
  constexpr std::uint64_t start = 4'096;
  constexpr float level = 0.9F;
  constexpr double moved_crossover = 300.0;
  const auto values = preset(table(), "Master Impact");
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, drums(start));
  auto changed = values;
  changed[at("strike.bands")] = static_cast<double>(one_band);
  changed[at("strike.xover_1")] = moved_crossover;
  changed[at(band_id(0, "solo"))] = 1.0;
  require(runtime->offer_values(changed, 1), "new values are offered");
  std::vector<engine::StereoFrame> buffer(oscillate::test::block_128, {level, -level});
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < blocks; ++block) {
    constexpr double beats_per_bar = 4.0;
    const Transport transport{0.0, 0.0, 0.0, beats_per_bar};
    (void)runtime->process(buffer, {}, start + block * buffer.size(), block < blocks / 2 ? 1U : 2U,
                           transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

// The view shows each hit's lift and the cut in its decay, slice by slice,
// over the input's peaks.
void telemetry_feeds_the_panel() {
  namespace t = strike::telemetry;
  constexpr double setting_db = 6.0;
  constexpr float reading_tolerance = 0.5F;
  constexpr float shown_db = 5.0F;
  auto values = shaped(setting_db, -setting_db);
  values[at("strike.bands")] = static_cast<double>(two_bands);
  values[at("strike.band_2.attack")] = setting_db;
  values[at("strike.band_2.sustain")] = -setting_db;
  values[at("strike.lookahead")] = static_cast<double>(lookahead_2ms);
  const auto input = drums(frames(strike::history_seconds));
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, input);
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame");
  REQUIRE_EQ(frame.size(), t::size, "the layout's size");
  REQUIRE_EQ(frame[t::band_count], 2.0F, "two bands");
  REQUIRE_EQ(frame[t::crossovers],
             static_cast<float>(table().parameters[at("strike.xover_1")].default_value),
             "the crossover");
  REQUIRE_EQ(frame[t::crossovers + 1], 0.0F, "no second crossover");
  REQUIRE_LT(std::fabs(frame[t::latency_ms] - 2.0F), tolerance::float_round_trip_f,
             "two milliseconds of latency");
  // The input meter holds its peak, falling 60 dB a second.
  constexpr double meter_fall_db_per_second = 60.0;
  const double fall = db_to_gain(-meter_fall_db_per_second / rate);
  double held = 0.0;
  for (const double x : input.left) {
    held = std::max(held * fall, std::fabs(rounded(x)));
  }
  REQUIRE_LT(std::fabs(frame[t::input_peak] - static_cast<float>(gain_to_db(held))),
             reading_tolerance, "the input peak");
  // The view reads the split bands' sum, whose peaks the crossovers' phase
  // moves a little.
  constexpr double allpass_peak_db = 1.5;
  float most = 0.0F;
  float least = 0.0F;
  float slice_peak = 0.0F;
  for (std::size_t s = 0; s < strike::history_slices; ++s) {
    most = std::max(most, frame[t::history_gain + s]);
    least = std::min(least, frame[t::history_gain + s]);
    slice_peak = std::max(slice_peak, frame[t::history_input + s]);
  }
  REQUIRE_LT(std::fabs(gain_to_db(slice_peak) - gain_to_db(peak(input.left))), allpass_peak_db,
             "the slices read the input's peaks");
  REQUIRE_GT(most, shown_db, "the lifts show");
  REQUIRE_LT(least, -shown_db, "and the cuts");
  for (std::size_t s = 0; s < strike::history_slices; ++s) {
    REQUIRE_EQ(frame[t::history_gain + 2 * strike::history_slices + s], 0.0F,
               "unused bands read 0 dB");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native strike contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"zero_settings_pass_untouched", [] { zero_settings_pass_untouched(); }},
          {"bands_sum_flat", [] { bands_sum_flat(); }},
          {"attack_gain_matches_its_setting_at_any_level",
           [] { attack_gain_matches_its_setting_at_any_level(); }},
          {"sustain_gain_matches_its_setting_at_any_level",
           [] { sustain_gain_matches_its_setting_at_any_level(); }},
          {"a_steady_tone_is_left_alone", [] { a_steady_tone_is_left_alone(); }},
          {"attack_time_sets_the_length_of_the_lift",
           [] { attack_time_sets_the_length_of_the_lift(); }},
          {"sustain_time_sets_how_much_decay_counts",
           [] { sustain_time_sets_how_much_decay_counts(); }},
          {"band_settings_stay_in_their_band", [] { band_settings_stay_in_their_band(); }},
          {"solo_plays_only_its_band", [] { solo_plays_only_its_band(); }},
          {"stereo_link_shares_the_gain", [] { stereo_link_shares_the_gain(); }},
          {"mix_blends_with_the_dry_bands", [] { mix_blends_with_the_dry_bands(); }},
          {"clip_holds_the_ceiling", [] { clip_holds_the_ceiling(); }},
          {"latency_is_reported_and_exact", [] { latency_is_reported_and_exact(); }},
          {"linear_phase_sums_to_the_input_delayed",
           [] { linear_phase_sums_to_the_input_delayed(); }},
          {"lookahead_lifts_the_first_cycle", [] { lookahead_lifts_the_first_cycle(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"export_equals_live", [] { export_equals_live(); }},
          {"hostile_input_stays_finite", [] { hostile_input_stays_finite(); }},
          {"the_tail_is_bounded", [] { the_tail_is_bounded(); }},
          {"presets_stay_restrained", [] { presets_stay_restrained(); }},
          {"a_seek_starts_clean", [] { a_seek_starts_clean(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}

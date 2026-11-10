#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/gild.hpp"
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
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Gild (ADR-0122) against measured oracles: at zero drive one band passes
// the input bit for bit (delayed by the oversampler's round trip) and
// several sum flat; each curve's harmonics, in each band, match the static
// curve applied to the band's sine (an independent oracle written here);
// Even raises the even harmonics; the top factor aliases under -100 dBFS;
// Level Match holds the band's level; Dynamic drives quiet passages
// harder; each band's drive stays in its band; solo, mix and delta; the
// latency is reported and the excited path lines up with the dry; block
// splits, export against live, hostile input, a bounded tail, restrained
// presets, a seek, an allocation-free callback and the panel's telemetry.
namespace {
using namespace effect_test;
namespace gild = oscillate::native_dsp::effects::gild;
using oscillate::native_dsp::dsp::db_to_gain;
using oscillate::native_dsp::dsp::gain_to_db;

const effects::EffectEngine& engine() { return gild::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::size_t parameter_count = 30;
constexpr std::size_t preset_count = 6;
constexpr std::size_t one_band = 0;
constexpr std::size_t two_bands = 1;
constexpr std::size_t three_bands = 2;
constexpr std::size_t four_bands = 3;
constexpr double linear_phase = 1.0;
constexpr double on = 1.0;
constexpr double off = 0.0;
// gild_table::oversampling's choices.
constexpr std::size_t x1 = 0;
constexpr std::size_t x4 = 2;
constexpr std::size_t x8 = 3;
constexpr std::size_t x16 = 4;
constexpr std::array factors{x1, std::size_t{1}, x4, x8, x16};
// gild_table::curves, Forge's order.
enum Curve : std::uint8_t { tape, tube, transistor, diode };
constexpr std::array curve_names{"Tape", "Tube", "Transistor", "Diode"};
constexpr std::size_t curve_count = curve_names.size();

constexpr double render_seconds = 0.5;
constexpr double tone_level = 0.5;
// Measurements read whole cycles of every tone used here (multiples of
// 10 Hz) over this window, after the DC blocker and the bands settle.
constexpr double window_seconds = 0.1;
constexpr double settle_seconds = 0.3;

std::vector<double> defaults() { return preset(table(), "Default"); }

std::size_t frames(double seconds) { return static_cast<std::size_t>(seconds * rate); }

std::vector<double> with(std::vector<double> values, std::string_view id, double value) {
  values[at(id)] = value;
  return values;
}

std::string band_id(std::size_t band, std::string_view part) {
  return "gild.band_" + std::to_string(band + 1) + "." + std::string(part);
}

// One band, no split, at `factor`, Level Match off: the bare curve.
std::vector<double> bare(std::size_t factor = x8) {
  auto v = with(defaults(), "gild.bands", static_cast<double>(one_band));
  v[at("gild.oversampling")] = static_cast<double>(factor);
  v[at("gild.match")] = off;
  return v;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
std::vector<double> driven(std::vector<double> v, std::size_t band, double drive, Curve curve,
                           double even = 0.0) {
  v[at(band_id(band, "drive"))] = drive;
  v[at(band_id(band, "curve"))] = static_cast<double>(curve);
  v[at(band_id(band, "even"))] = even;
  return v;
}

Stereo silence(std::size_t count) {
  return {std::vector<double>(count, 0.0), std::vector<double>(count, 0.0)};
}

Stereo tone(double hz, double amplitude, std::size_t count) { return sine(hz, amplitude, count); }

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

// The complex amplitude of `hz` over [from, from + length).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
std::complex<double> phasor(const std::vector<double>& x, std::size_t from, std::size_t length,
                            double hz) {
  std::complex<double> sum{};
  for (std::size_t i = 0; i < length; ++i) {
    sum += x[from + i] *
           std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(from + i) / rate);
  }
  return 2.0 * sum / static_cast<double>(length);
}

double amplitude_db(const std::vector<double>& x, std::size_t from, std::size_t length, double hz) {
  return gain_to_db(std::abs(phasor(x, from, length, hz)));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
double rms_of(const std::vector<double>& x, std::size_t from, std::size_t length) {
  double sum = 0.0;
  for (std::size_t i = 0; i < length; ++i) {
    sum += x[from + i] * x[from + i];
  }
  return std::sqrt(sum / static_cast<double>(length));
}

// ---- The independent oracle: Forge's curves written out again --------------------------

constexpr double tube_negative = 0.5;
constexpr double max_bias = gild::max_bias;

double curve_at(Curve curve, double u) {
  switch (curve) {
  case tube:
    return u >= 0.0 ? std::tanh(u) : std::tanh(tube_negative * u) / tube_negative;
  case transistor:
    return u / std::sqrt(1.0 + u * u);
  case diode:
    return std::copysign(1.0 - std::exp(-std::fabs(u)), u);
  default:
    return std::tanh(u);
  }
}

// The slope by a central difference.
double slope_at(Curve curve, double u) {
  constexpr double step = 1e-6;
  return (curve_at(curve, u + step) - curve_at(curve, u - step)) / (2.0 * step);
}

// What the static curve adds to the band's sine, normalised as Gild
// normalises it (the curve's output less the sine): harmonic n's amplitude
// (dBFS) by a DFT over one cycle.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
double oracle_db(Curve curve, double drive, double even, double amplitude, std::size_t harmonic) {
  constexpr std::size_t points = 8'192;
  const double d = gild::drive_gain(drive);
  const double b = max_bias * even;
  const double offset = curve_at(curve, b);
  const double scale = 1.0 / (d * slope_at(curve, b));
  std::complex<double> sum{};
  for (std::size_t i = 0; i < points; ++i) {
    const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) / points;
    const double x = amplitude * std::sin(phase);
    const double y = (curve_at(curve, d * x + b) - offset) * scale - x;
    sum += y * std::polar(1.0, -static_cast<double>(harmonic) * phase);
  }
  return gain_to_db(2.0 * std::abs(sum) / points);
}

// ---- Cases ---------------------------------------------------------------------------------

void descriptor_registry_and_presets() {
  REQUIRE_EQ(effects::find_engine("gild"), &engine(), "Gild is registered");
  require(!engine().sidechain && !engine().generator, "Gild reads only its input");
  REQUIRE_EQ(table().parameters.size(), parameter_count, "the table's size");
  for (const auto* id : {"gild.bands", "gild.phase", "gild.oversampling"}) {
    require(!table().parameters[at(id)].automatable, std::string(id) + " is not automatable");
  }
  REQUIRE_EQ(table().presets.size(), preset_count, "Default and five presets");
  for (const auto& entry : table().presets) {
    const auto out = render(preset(table(), entry.name), noise(0.5, frames(render_seconds)));
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]),
              "preset renders finite: " + std::string(entry.name));
    }
  }
}

// At zero drive one band is a wire, the input bit for bit delayed by the
// oversampler's round trip, at every factor.
void zero_drive_passes_untouched() {
  const auto input = noise(0.5, frames(render_seconds));
  for (const std::size_t factor : factors) {
    auto values = with(defaults(), "gild.bands", static_cast<double>(one_band));
    values[at("gild.oversampling")] = static_cast<double>(factor);
    const auto delay = engine().latency_frames(values, rate);
    const auto out = render(values, input);
    for (std::size_t i = 0; i < input.size(); ++i) {
      const double expected_left = i < delay ? 0.0 : rounded(input.left[i - delay]);
      const double expected_right = i < delay ? 0.0 : rounded(input.right[i - delay]);
      require(out.left[i] == expected_left && out.right[i] == expected_right,
              "zero drive passes the input untouched at factor " + std::to_string(factor));
    }
  }
}

// At zero drive the bands sum to one allpass: an impulse's spectrum is flat
// within 0.01 dB from 20 Hz to 20 kHz, for every band count.
void bands_sum_flat() {
  constexpr std::size_t length = 65'536;
  constexpr double impulse_at = 0.5;
  constexpr double lowest_hz = 20.0;
  constexpr double highest_hz = 20'000.0;
  constexpr double flat_db = 0.01;
  for (const std::size_t bands : {two_bands, three_bands, four_bands}) {
    auto input = silence(length);
    input.left[0] = input.right[0] = impulse_at;
    const auto out = render(with(defaults(), "gild.bands", static_cast<double>(bands)), input);
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

// Linear phase: at zero drive the bands sum to the input, delayed by the
// bank's latency and the oversampler's, for every band count.
void linear_phase_sums_to_the_input_delayed() {
  constexpr double sum_tolerance = 1e-6;
  const auto input = noise(0.5, frames(render_seconds));
  for (const std::size_t bands : {one_band, two_bands, three_bands, four_bands}) {
    auto values = with(defaults(), "gild.phase", linear_phase);
    values[at("gild.bands")] = static_cast<double>(bands);
    const auto delay = engine().latency_frames(values, rate);
    const auto out = render(values, input);
    for (std::size_t i = delay; i < input.size(); ++i) {
      REQUIRE_LT(std::fabs(out.left[i] - rounded(input.left[i - delay])), sum_tolerance,
                 "the bands sum to the input delayed: " + std::to_string(bands + 1) + " bands");
    }
  }
}

// What each curve adds (Delta), with a sine in each of four linear-phase
// bands in turn, matches the oracle: the static curve, normalised, on that
// sine, less the sine. H1 (the curve's share of the fundamental) to H5
// under 20 kHz, wherever the oracle reads over -90 dBFS.
void harmonics_match_the_curve(Curve curve) {
  constexpr std::array band_hz{60.0, 500.0, 3'000.0, 9'000.0};
  constexpr std::array drives{0.1, 0.3, 0.7};
  constexpr std::array evens{0.0, 0.6};
  constexpr std::size_t highest_harmonic = 5;
  constexpr double audible_hz = 20'000.0;
  constexpr double oracle_floor_db = -90.0;
  constexpr double harmonic_tolerance_db = 0.1;
  const auto count = frames(settle_seconds + window_seconds);
  const auto from = frames(settle_seconds);
  const auto length = frames(window_seconds);
  auto base = with(bare(x8), "gild.bands", static_cast<double>(four_bands));
  base[at("gild.phase")] = linear_phase;
  base[at("gild.delta")] = on;
  const std::size_t c = curve;
  std::printf("curve band drive even | added H1 H2 H3 H4 H5 dBFS\n");
  for (std::size_t b = 0; b < band_hz.size(); ++b) {
    const auto input = tone(band_hz[b], tone_level, count);
    for (const double drive : drives) {
      for (const double even : evens) {
        const auto out = render(driven(base, b, drive, curve, even), input);
        std::printf("%s %zu %.1f %.1f |", curve_names[c], b + 1, drive, even);
        for (std::size_t n = 1; n <= highest_harmonic; ++n) {
          const double hz = band_hz[b] * static_cast<double>(n);
          if (hz > audible_hz) {
            continue;
          }
          const double expected = oracle_db(curve, drive, even, tone_level, n);
          const double measured = amplitude_db(out.left, from, length, hz);
          std::printf(" %.2f", measured);
          if (expected < oracle_floor_db) {
            continue;
          }
          REQUIRE_LT(std::fabs(measured - expected), harmonic_tolerance_db,
                     std::string(curve_names[c]) + " band " + std::to_string(b + 1) + " H" +
                         std::to_string(n) + ": " + std::to_string(measured) + " against " +
                         std::to_string(expected));
        }
        std::printf("\n");
      }
    }
  }
}

// Even biases the curve: with none the symmetric curves add no even
// harmonics, and the second harmonic against the third rises with it.
void even_raises_the_even_harmonics() {
  constexpr double hz = 1'000.0;
  constexpr double drive = 0.3;
  constexpr double no_even_db = -100.0;
  constexpr std::array evens{0.0, 0.3, 0.6, 1.0};
  const auto count = frames(settle_seconds + window_seconds);
  const auto from = frames(settle_seconds);
  const auto length = frames(window_seconds);
  const auto input = tone(hz, tone_level, count);
  for (const Curve curve : {tape, transistor, diode}) {
    double previous = -std::numeric_limits<double>::infinity();
    for (const double even : evens) {
      const auto out = render(driven(bare(), 0, drive, curve, even), input);
      const double h2 = amplitude_db(out.left, from, length, 2.0 * hz);
      const double h3 = amplitude_db(out.left, from, length, 3.0 * hz);
      if (even == 0.0) {
        REQUIRE_LT(h2, no_even_db, std::string(curve_names[curve]) + " is symmetric at no Even");
      }
      REQUIRE_GT(h2 - h3, previous,
                 std::string(curve_names[curve]) + ": H2 over H3 rises with Even");
      previous = h2 - h3;
    }
  }
}

// The largest spectral line from 20 Hz to 20 kHz away from `hz`, in dBFS,
// through a Kaiser window whose sidelobes sit far under the floor measured.
double worst_spur_db(const std::vector<double>& x, std::size_t from, double hz) {
  constexpr std::size_t size = 65'536;
  constexpr double window_beta = 20.0;
  constexpr double lowest_hz = 20.0;
  constexpr double highest_hz = 20'000.0;
  constexpr double main_lobe_hz = 50.0;
  std::vector<std::complex<double>> spectrum(size);
  double window_sum = 0.0;
  const double half = static_cast<double>(size - 1) / 2.0;
  for (std::size_t i = 0; i < size; ++i) {
    const double w =
        oscillate::native_dsp::dsp::kaiser(static_cast<double>(i) - half, half, window_beta);
    spectrum[i] = x[from + i] * w;
    window_sum += w;
  }
  fft(spectrum);
  double worst = 0.0;
  for (std::size_t bin = 1; bin < size / 2; ++bin) {
    const double f = static_cast<double>(bin) * rate / static_cast<double>(size);
    if (f >= lowest_hz && f <= highest_hz && std::fabs(f - hz) > main_lobe_hz) {
      worst = std::max(worst, 2.0 * std::abs(spectrum[bin]) / window_sum);
    }
  }
  return db(worst);
}

// A 10.5 kHz sine, whose harmonics all lie over 20 kHz, driven hard through
// every curve: at 16x nothing under 20 kHz but the sine reaches -100 dBFS.
// At 1x the harmonics fold straight back, and the measurement sees them.
void aliasing_under_100_dbfs_at_16x() {
  constexpr double hz = 10'500.0;
  constexpr double alias_floor_db = -100.0;
  constexpr std::size_t length = 82'000;
  constexpr std::size_t settled = 12'000;
  constexpr std::array drives{0.5, 1.0};
  constexpr double even = 0.5;
  const auto input = tone(hz, tone_level, length);
  for (std::size_t c = 0; c < curve_count; ++c) {
    const auto curve = static_cast<Curve>(c);
    for (const double drive : drives) {
      const auto out = render(driven(bare(x16), 0, drive, curve, even), input);
      const double worst = worst_spur_db(out.left, settled, hz);
      std::printf("aliases %s drive %.1f: %.1f dBFS\n", curve_names[c], drive, worst);
      REQUIRE_LT(worst, alias_floor_db,
                 std::string(curve_names[c]) +
                     " at 16x aliases under -100 dBFS: " + std::to_string(worst));
    }
  }
  const auto folded = render(driven(bare(x1), 0, 1.0, tape, even), input);
  REQUIRE_GT(worst_spur_db(folded.left, settled, hz), alias_floor_db, "1x aliases");
}

// Level Match holds a driven band's level within 0.1 dB, settled, for every
// curve and drive; without it a hard drive moves the level by over 1 dB.
void level_match_holds_the_level() {
  constexpr double hz = 1'000.0;
  constexpr std::array drives{0.25, 0.5, 1.0};
  constexpr double match_tolerance_db = 0.1;
  constexpr double unmatched_db = 1.0;
  constexpr double settled_seconds = 2.0;
  const auto count = frames(settled_seconds + window_seconds);
  const auto from = frames(settled_seconds);
  const auto length = frames(window_seconds);
  const auto input = tone(hz, tone_level, count);
  const double in_db = gain_to_db(rms_of(input.left, from, length));
  for (std::size_t c = 0; c < curve_count; ++c) {
    const auto curve = static_cast<Curve>(c);
    for (const double drive : drives) {
      const auto matched = with(driven(bare(), 0, drive, curve, 0.5), "gild.match", on);
      const auto out = render(matched, input);
      const double change = gain_to_db(rms_of(out.left, from, length)) - in_db;
      REQUIRE_LT(std::fabs(change), match_tolerance_db,
                 std::string(curve_names[c]) + " matched at drive " + std::to_string(drive) + ": " +
                     std::to_string(change));
    }
  }
  const auto loose = render(driven(bare(), 0, 1.0, tape), input);
  REQUIRE_GT(std::fabs(gain_to_db(rms_of(loose.left, from, length)) - in_db), unmatched_db,
             "unmatched, a hard drive moves the level");
}

// Dynamic drives a quiet passage harder: the third harmonic against the
// fundamental moves 40 dB between a sine 20 dB quieter and the loud one
// without it, and over 20 dB less with it at full; the loud one, over the
// reference, is not lifted.
void dynamic_drives_quiet_passages_harder() {
  constexpr double hz = 1'000.0;
  constexpr double loud = 0.3;
  constexpr double quiet_db = -20.0;
  constexpr double drive = 0.3;
  constexpr double least_narrowing_db = 20.0;
  constexpr double unchanged_db = 0.05;
  const auto count = frames(settle_seconds + window_seconds);
  const auto from = frames(settle_seconds);
  const auto length = frames(window_seconds);
  const auto third_over_first = [&](const std::vector<double>& values, double amplitude) {
    const auto out = render(values, tone(hz, amplitude, count));
    return amplitude_db(out.left, from, length, 3.0 * hz) -
           amplitude_db(out.left, from, length, hz);
  };
  const auto still = driven(bare(), 0, drive, tape);
  const auto dynamic = with(still, "gild.dynamic", 1.0);
  const double spread_still =
      third_over_first(still, loud) - third_over_first(still, loud * db_to_gain(quiet_db));
  const double spread_dynamic =
      third_over_first(dynamic, loud) - third_over_first(dynamic, loud * db_to_gain(quiet_db));
  std::printf("H3/H1 spread over 20 dB: %.2f dB still, %.2f dB dynamic\n", spread_still,
              spread_dynamic);
  REQUIRE_LT(spread_dynamic, spread_still - least_narrowing_db,
             "Dynamic narrows the spread: " + std::to_string(spread_dynamic) + " against " +
                 std::to_string(spread_still));
  REQUIRE_LT(std::fabs(third_over_first(dynamic, loud) - third_over_first(still, loud)),
             unchanged_db, "a loud passage is not lifted");
}

// Tones in bands 1, 2 and 4 of the linear-phase bank with only band 3
// driven hard: what comes out is the undriven render within -100 dBFS, as
// the bank's stopband keeps the tones out of band 3.
void band_drive_stays_in_its_band() {
  constexpr std::array tone_hz{60.0, 500.0, 15'000.0};
  constexpr double quiet = 0.2;
  constexpr double untouched_db = -100.0;
  const auto count = frames(render_seconds);
  Stereo input = silence(count);
  for (const double hz : tone_hz) {
    input = added(input, tone(hz, quiet, count));
  }
  auto four = with(defaults(), "gild.bands", static_cast<double>(four_bands));
  four[at("gild.phase")] = linear_phase;
  const auto plain = render(four, input);
  const auto excited = render(driven(four, 2, 1.0, tape, 0.5), input);
  // The tones' onsets reach every band.
  double worst = 0.0;
  for (std::size_t i = frames(settle_seconds); i < count; ++i) {
    worst = std::max(worst, std::fabs(excited.left[i] - plain.left[i]));
  }
  REQUIRE_LT(gain_to_db(worst), untouched_db,
             "band 3's drive leaves the other bands: " + std::to_string(gain_to_db(worst)));
}

// Solo plays only its band: steady tones in all four, band 2 soloed. Each
// tone sits at least 1.65 octaves from band 2's crossovers (150 Hz and
// 1.5 kHz), where they fall 40 dB.
void solo_plays_only_its_band() {
  constexpr std::array band_hz{40.0, 470.0, 5'040.0, 16'000.0};
  constexpr double quiet = 0.2;
  constexpr double skirt_db = 0.5;
  constexpr double muted_db = -40.0;
  constexpr std::size_t soloed = 1;
  const auto count = frames(render_seconds);
  Stereo input = silence(count);
  for (const double hz : band_hz) {
    input = added(input, tone(hz, quiet, count));
  }
  auto values = with(defaults(), "gild.bands", static_cast<double>(four_bands));
  values[at(band_id(soloed, "solo"))] = on;
  const auto out = render(values, input);
  const auto from = count - frames(window_seconds);
  for (std::size_t b = 0; b < band_hz.size(); ++b) {
    const double level =
        amplitude_db(out.left, from, frames(window_seconds), band_hz[b]) - gain_to_db(quiet);
    if (b == soloed) {
      REQUIRE_LT(std::fabs(level), skirt_db, "the soloed band plays");
    } else {
      REQUIRE_LT(level, muted_db, "band " + std::to_string(b + 1) + " is silent under a solo");
    }
  }
}

// Mix blends with the dry band: no mix is the undriven render bit for bit;
// half mix adds half the harmonics.
void mix_blends_with_the_dry_band() {
  constexpr double hz = 1'000.0;
  constexpr double drive = 0.5;
  constexpr double half = 0.5;
  constexpr double half_db = -6.0206;
  constexpr double mix_tolerance_db = 0.01;
  const auto count = frames(settle_seconds + window_seconds);
  const auto from = frames(settle_seconds);
  const auto length = frames(window_seconds);
  const auto input = tone(hz, tone_level, count);
  const auto full = driven(bare(), 0, drive, tape, 0.5);
  const auto dry = render(with(full, band_id(0, "mix"), 0.0), input);
  const auto plain = render(bare(), input);
  for (std::size_t i = 0; i < count; ++i) {
    REQUIRE_EQ(dry.left[i], plain.left[i], "no mix is the dry band");
  }
  const auto whole = render(full, input);
  const auto halved = render(with(full, band_id(0, "mix"), half), input);
  for (const double n : {2.0, 3.0}) {
    const double change = amplitude_db(halved.left, from, length, n * hz) -
                          amplitude_db(whole.left, from, length, n * hz);
    REQUIRE_LT(std::fabs(change - half_db), mix_tolerance_db, "half mix, half the harmonics");
  }
}

// Delta plays what was added: the excited render less the undriven one.
void delta_plays_what_was_added() {
  constexpr double hz = 700.0;
  const auto count = frames(render_seconds);
  const auto input = added(tone(hz, tone_level, count), noise(0.05, count));
  for (const auto& name : {"Air Lift", "Warm Mids", "Tape Glue"}) {
    const auto values = preset(table(), name);
    const auto whole = render(values, input);
    auto zero = values;
    for (std::size_t b = 0; b < gild::max_bands; ++b) {
      zero[at(band_id(b, "drive"))] = 0.0;
    }
    const auto plain = render(zero, input);
    const auto delta = render(with(values, "gild.delta", on), input);
    for (std::size_t i = 0; i < count; ++i) {
      REQUIRE_LT(std::fabs(delta.left[i] - (whole.left[i] - plain.left[i])),
                 tolerance::float_processing, std::string(name) + ": delta is what was added");
    }
  }
}

// Every combination's latency is the one reported; an impulse at zero drive
// arrives exactly that late, and the excited band's fundamental lines up
// with the dry band's: its phase within 0.3 degrees of the input's at
// 5 kHz, where half a top-rate sample at 16x is 1.2 degrees. (The
// residue's DC blocker turns the curve's share of the fundamental by a
// little under that.)
void latency_is_reported_and_exact() {
  constexpr std::size_t length = 8'192;
  constexpr std::size_t impulse_at = 100;
  constexpr double impulse = 0.5;
  constexpr double hz = 5'000.0;
  constexpr double drive = 0.6;
  constexpr double degrees_per_radian = 180.0 / std::numbers::pi;
  constexpr double phase_tolerance_degrees = 0.3;
  const auto count = frames(settle_seconds + window_seconds);
  const auto from = frames(settle_seconds);
  const auto window = frames(window_seconds);
  for (const std::size_t factor : factors) {
    for (const double phase : {0.0, linear_phase}) {
      auto values = with(bare(factor), "gild.phase", phase);
      const auto latency = engine().latency_frames(values, rate);
      REQUIRE_EQ(latency, gild::latency_frames(phase != 0.0, factor, rate),
                 "the engine reports the header's latency");
      auto runtime = make_effect(engine(), values, rate);
      REQUIRE_EQ(runtime->latency_frames(), latency, "the runtime reports it");
      auto input = silence(length);
      input.left[impulse_at] = input.right[impulse_at] = impulse;
      const auto out = process(*runtime, input);
      for (std::size_t i = 0; i < length; ++i) {
        REQUIRE_LT(std::fabs(out.left[i] - (i == impulse_at + latency ? impulse : 0.0)),
                   tolerance::float_round_trip, "the impulse arrives exactly that late");
      }
      const auto sine_in = tone(hz, tone_level, count);
      const auto excited = render(driven(values, 0, drive, tape), sine_in);
      const auto expected =
          phasor(sine_in.left, from - latency, window, hz) *
          std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(latency) / rate);
      const double error = std::fabs(std::arg(phasor(excited.left, from, window, hz) / expected)) *
                           degrees_per_radian;
      REQUIRE_LT(error, phase_tolerance_degrees,
                 "the excited band lines up at factor " + std::to_string(factor) + ": " +
                     std::to_string(error));
    }
  }
}

// A programme for the presets: a bass line, a chord's partials, hits every
// quarter second, hats (bursts of noise) every eighth and a bed of noise.
Stereo programme(std::size_t count) {
  constexpr std::array partial_hz{55.0, 220.0, 330.0, 440.0, 1'320.0, 2'640.0};
  constexpr std::array partial_level{0.25, 0.08, 0.06, 0.05, 0.02, 0.01};
  constexpr double hit_seconds = 0.25;
  constexpr double hit_hz = 180.0;
  constexpr double hit_decay = 0.05;
  constexpr double hit_level = 0.3;
  constexpr double bed = 0.02;
  constexpr double hat_seconds = 0.125;
  constexpr double hat_decay = 0.015;
  constexpr double hat_level = 0.15;
  constexpr std::uint32_t hat_seed = 77;
  Stereo out = noise(bed, count);
  const auto hats = noise(hat_level, count, false, hat_seed);
  const auto hat_period = frames(hat_seconds);
  for (std::size_t i = 0; i < count; ++i) {
    const double envelope = std::exp(-static_cast<double>(i % hat_period) / rate / hat_decay);
    out.left[i] += envelope * hats.left[i];
    out.right[i] += envelope * hats.right[i];
  }
  for (std::size_t p = 0; p < partial_hz.size(); ++p) {
    out = added(out, tone(partial_hz[p], partial_level[p], count));
  }
  const auto period = frames(hit_seconds);
  for (std::size_t i = 0; i < count; ++i) {
    const double t = static_cast<double>(i % period) / rate;
    const double x =
        hit_level * std::exp(-t / hit_decay) * std::sin(2.0 * std::numbers::pi * hit_hz * t);
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
  auto busy = preset(table(), "Tape Glue");
  constexpr double partial = 0.4;
  busy[at(band_id(2, "solo"))] = on;
  busy[at(band_id(1, "curve"))] = static_cast<double>(diode);
  busy[at(band_id(3, "mix"))] = partial;
  busy[at("gild.dynamic")] = partial;
  busy[at("gild.oversampling")] = static_cast<double>(x16);
  result.push_back(busy);
  auto lean = preset(table(), "Bass Weight");
  lean[at("gild.oversampling")] = static_cast<double>(x1);
  lean[at("gild.delta")] = on;
  result.push_back(lean);
  return result;
}

void any_block_split_renders_the_same() {
  const auto input = programme(frames(render_seconds));
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
  const auto input = programme(frames(render_seconds));
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
  Stereo input = programme(frames(render_seconds));
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
    auto input = programme(played + static_cast<std::size_t>(tail) + frames(seconds));
    for (std::size_t i = played; i < input.size(); ++i) {
      input.left[i] = input.right[i] = 0.0;
    }
    const auto out = render(v, input);
    REQUIRE_LT(peak(out.left, played + static_cast<std::size_t>(tail)), silent,
               "silent after the tail");
  }
}

// On the programme each preset adds harmonics between 18 and 60 dB under
// it (the least where the programme has least in the preset's band), and,
// matched, moves its level by under half a decibel.
void presets_stay_restrained() {
  constexpr double seconds = 3.0;
  constexpr double settled_seconds = 1.0;
  constexpr double most_added_db = -18.0;
  constexpr double least_added_db = -60.0;
  constexpr double most_level_change_db = 0.5;
  const auto count = frames(seconds);
  const auto from = frames(settled_seconds);
  const auto length = count - from;
  const auto input = programme(count);
  const double in_db = gain_to_db(rms_of(input.left, from, length));
  for (const auto& entry : table().presets) {
    if (entry.name == "Default") {
      continue;
    }
    const auto values = preset(table(), entry.name);
    const auto out = render(values, input);
    const auto delta = render(with(values, "gild.delta", on), input);
    const double added_db = gain_to_db(rms_of(delta.left, from, length)) - in_db;
    const double change_db = gain_to_db(rms_of(out.left, from, length)) - in_db;
    std::printf("%s: adds %.1f dB, level %+.2f dB\n", std::string(entry.name).c_str(), added_db,
                change_db);
    REQUIRE_LT(added_db, most_added_db, std::string(entry.name) + " adds no more than that");
    REQUIRE_GT(added_db, least_added_db, std::string(entry.name) + " adds something");
    REQUIRE_LT(std::fabs(change_db), most_level_change_db,
               std::string(entry.name) + " keeps the level");
  }
}

// A seek (a new transport generation) clears every history: after it Gild
// plays as a fresh one.
void a_seek_starts_clean() {
  constexpr std::uint32_t other_seed = 99;
  const auto before = programme(frames(render_seconds));
  const auto after = noise(0.5, frames(render_seconds), true, other_seed);
  const auto v = preset(table(), "Tape Glue");
  auto seeked = make_effect(engine(), v, rate);
  (void)process(*seeked, before);
  const auto resumed = process(*seeked, after, {.generation = 2});
  const auto fresh = render(v, after);
  require(resumed.left == fresh.left && resumed.right == fresh.right,
          "after a seek Gild plays as a fresh one");
}

// A band the band count drops and brings back starts clean. A loud driven
// passage leaves band 4's Level Match far from unity; after a second of
// silence (the crossovers settle, the match's powers fall together and
// keep their ratio), half a second on one band, then four bands again on a
// tone: once the band has faded back in the output is a fresh Gild's.
void a_band_that_rejoins_starts_clean() {
  constexpr double loud_seconds = 0.5;
  constexpr double settle_silence_seconds = 1.0;
  constexpr double one_band_seconds = 0.5;
  constexpr double tone_seconds = 0.5;
  constexpr double hz = 10'000.0;
  constexpr double quiet = 0.1;
  constexpr std::size_t driven_band = 3;
  // The rejoining bands fade in over dsp::switch_fade_seconds.
  constexpr double fade_seconds = 0.02;
  constexpr double clean_tolerance = 1e-5;
  const auto values = driven(defaults(), driven_band, 1.0, tape, 0.5);
  const auto one = with(values, "gild.bands", static_cast<double>(one_band));
  const auto loud = frames(loud_seconds);
  const auto drop = loud + frames(settle_silence_seconds);
  const auto rejoin = drop + frames(one_band_seconds);
  const auto count = rejoin + frames(tone_seconds);
  Stereo input = silence(count);
  Stereo fresh_input = silence(count);
  const auto burst = noise(1.0, loud);
  const auto ending = tone(hz, quiet, count);
  for (std::size_t i = 0; i < count; ++i) {
    if (i < loud) {
      input.left[i] = burst.left[i];
      input.right[i] = burst.right[i];
    } else if (i >= rejoin) {
      input.left[i] = fresh_input.left[i] = ending.left[i];
      input.right[i] = fresh_input.right[i] = ending.right[i];
    }
  }
  auto runtime = make_effect(engine(), values, rate);
  std::uint64_t generation = 1;
  const auto out = process(
      *runtime, input, {.at = {drop, rejoin}, .before = [&](std::uint64_t frame) {
                          require(runtime->offer_values(frame == drop ? one : values, ++generation),
                                  "the band count is offered");
                        }});
  const auto fresh = render(values, fresh_input);
  double worst = 0.0;
  for (std::size_t i = rejoin + frames(fade_seconds); i < count; ++i) {
    worst = std::max(worst, std::fabs(out.left[i] - fresh.left[i]));
  }
  REQUIRE_LT(worst, clean_tolerance, "the band that joins again starts clean");
}

void the_callback_never_allocates() {
  constexpr std::uint64_t blocks = 2'000;
  constexpr std::uint64_t start = 4'096;
  constexpr float level = 0.9F;
  constexpr double moved_crossover = 300.0;
  const auto values = preset(table(), "Tape Glue");
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, programme(start));
  auto changed = values;
  changed[at("gild.bands")] = static_cast<double>(two_bands);
  changed[at("gild.xover_1")] = moved_crossover;
  changed[at(band_id(0, "solo"))] = on;
  changed[at(band_id(1, "curve"))] = static_cast<double>(tube);
  changed[at("gild.dynamic")] = 1.0;
  changed[at("gild.delta")] = on;
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

// The panel's readings: band layout, latency, each band's level, what it
// added and its drive, and both spectra: the sine's bin reads its level,
// the added spectrum its third harmonic.
void telemetry_feeds_the_panel() {
  namespace t = gild::telemetry;
  constexpr double hz = 1'000.0;
  constexpr double drive = 0.5;
  constexpr float reading_tolerance_db = 1.0F;
  constexpr float floor_db = -120.0F;
  constexpr float audible_added_db = -60.0F;
  // The sine is 1.7 octaves over the crossover.
  constexpr float band_apart_db = 40.0F;
  constexpr double milliseconds = 1'000.0;
  constexpr double rms_of_sine_db = -3.0103;
  auto values = with(defaults(), "gild.bands", static_cast<double>(two_bands));
  values[at("gild.xover_1")] = 300.0;
  values = driven(values, 1, drive, tape);
  const auto input = tone(hz, tone_level, frames(1.0));
  auto runtime = make_effect(engine(), values, rate);
  (void)process(*runtime, input);
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame");
  REQUIRE_EQ(frame.size(), t::size, "the layout's size");
  REQUIRE_EQ(frame[t::band_count], 2.0F, "two bands");
  REQUIRE_EQ(frame[t::crossovers], 300.0F, "the crossover");
  REQUIRE_EQ(frame[t::crossovers + 1], 0.0F, "no second crossover");
  const double latency_ms =
      static_cast<double>(engine().latency_frames(values, rate)) / rate * milliseconds;
  REQUIRE_LT(std::fabs(frame[t::latency_ms] - static_cast<float>(latency_ms)),
             tolerance::float_round_trip_f, "the latency");
  REQUIRE_LT(std::fabs(frame[t::input_peak] - static_cast<float>(gain_to_db(tone_level))),
             reading_tolerance_db, "the input peak");
  REQUIRE_LT(std::fabs(frame[t::band_level + 1] -
                       static_cast<float>(gain_to_db(tone_level) + rms_of_sine_db)),
             reading_tolerance_db, "band 2's level");
  REQUIRE_LT(frame[t::band_level], frame[t::band_level + 1] - band_apart_db,
             "band 1 holds only the crossover's skirt");
  REQUIRE_GT(frame[t::band_added + 1], audible_added_db, "band 2 adds");
  REQUIRE_EQ(frame[t::band_added], floor_db, "band 1 adds nothing");
  REQUIRE_LT(
      std::fabs(frame[t::band_drive + 1] - static_cast<float>(gain_to_db(gild::drive_gain(drive)))),
      reading_tolerance_db, "band 2's drive");
  REQUIRE_EQ(frame[t::band_drive], floor_db, "band 1 is not driven");
  // The display bin that holds a frequency.
  const auto bin_of = [](double f) {
    const double share = std::log(f / gild::spectrum_low_hz) /
                         std::log(gild::spectrum_high_hz / gild::spectrum_low_hz);
    return static_cast<std::size_t>(share * static_cast<double>(gild::spectrum_bins));
  };
  REQUIRE_LT(
      std::fabs(frame[t::input_spectrum + bin_of(hz)] - static_cast<float>(gain_to_db(tone_level))),
      reading_tolerance_db, "the input spectrum reads the sine");
  const auto third = bin_of(3.0 * hz);
  const float third_reading = frame[t::added_spectrum + third];
  const auto out = process(*make_effect(engine(), with(values, "gild.delta", on), rate), input);
  const auto from = frames(settle_seconds);
  const double third_db = amplitude_db(out.left, from, frames(window_seconds), 3.0 * hz);
  REQUIRE_LT(std::fabs(third_reading - static_cast<float>(third_db)), reading_tolerance_db,
             "the added spectrum reads the third harmonic: " + std::to_string(third_reading) +
                 " against " + std::to_string(third_db));
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native gild contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"zero_drive_passes_untouched", [] { zero_drive_passes_untouched(); }},
          {"bands_sum_flat", [] { bands_sum_flat(); }},
          {"linear_phase_sums_to_the_input_delayed",
           [] { linear_phase_sums_to_the_input_delayed(); }},
          {"harmonics_match_tape", [] { harmonics_match_the_curve(tape); }},
          {"harmonics_match_tube", [] { harmonics_match_the_curve(tube); }},
          {"harmonics_match_transistor", [] { harmonics_match_the_curve(transistor); }},
          {"harmonics_match_diode", [] { harmonics_match_the_curve(diode); }},
          {"even_raises_the_even_harmonics", [] { even_raises_the_even_harmonics(); }},
          {"aliasing_under_100_dbfs_at_16x", [] { aliasing_under_100_dbfs_at_16x(); }},
          {"level_match_holds_the_level", [] { level_match_holds_the_level(); }},
          {"dynamic_drives_quiet_passages_harder", [] { dynamic_drives_quiet_passages_harder(); }},
          {"band_drive_stays_in_its_band", [] { band_drive_stays_in_its_band(); }},
          {"solo_plays_only_its_band", [] { solo_plays_only_its_band(); }},
          {"mix_blends_with_the_dry_band", [] { mix_blends_with_the_dry_band(); }},
          {"delta_plays_what_was_added", [] { delta_plays_what_was_added(); }},
          {"latency_is_reported_and_exact", [] { latency_is_reported_and_exact(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"export_equals_live", [] { export_equals_live(); }},
          {"hostile_input_stays_finite", [] { hostile_input_stays_finite(); }},
          {"the_tail_is_bounded", [] { the_tail_is_bounded(); }},
          {"presets_stay_restrained", [] { presets_stay_restrained(); }},
          {"a_seek_starts_clean", [] { a_seek_starts_clean(); }},
          {"a_band_that_rejoins_starts_clean", [] { a_band_that_rejoins_starts_clean(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}

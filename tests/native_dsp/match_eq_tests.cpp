#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/shape.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/match_eq.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/tonal_analysis.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Match EQ (ADR-0120): a programme matched to its own copy through known
// Shape curves recovers each curve within recovery_tolerance_db (the band
// meter, the correction and the fit end to end, measured against Shape's own
// response); the fit uses the fewest bands, and what it reports is what
// Shape plays; the correction follows Amount, Smoothing, the limits and the
// boost and cut; a target's median becomes fine levels.
namespace {

using namespace effect_test;
namespace tonal = oscillate::native_dsp::tonal;
namespace match_eq = oscillate::native_dsp::match_eq;
namespace shape = oscillate::native_dsp::effects::shape;
using oscillate::native_dsp::EqBandKind;
using oscillate::test::require;
using oscillate::test::sample_rate_48k_hz;

// The recovered curve against the true one, after the loudness offset the
// normalisation takes out, from compare_low_hz to compare_high_hz.
constexpr double recovery_tolerance_db = 1.0;
constexpr double recovery_rms_tolerance_db = 0.35;
constexpr double compare_low_hz = 40.0;
constexpr double compare_high_hz = 12'000.0;
constexpr double programme_seconds = 30.0;
constexpr std::uint64_t programme_seed = 11;
constexpr double programme_amplitude = 0.25;

// Settings for recovery: the whole correction, finely smoothed, wide limits.
constexpr match_eq::Settings exact{.amount = 1.0,
                                   .smoothing_octaves = 1.0 / 6.0,
                                   .low_hz = 30.0,
                                   .high_hz = 16'000.0,
                                   .max_boost_db = 12.0,
                                   .max_cut_db = 12.0};

// A fitted band against the one that drew the curve.
constexpr double band_hz_tolerance = 0.02;    // Relative.
constexpr double band_shape_tolerance = 0.05; // Relative.
constexpr double band_gain_tolerance_db = 0.1;

// Paul Kellet's economy pink filter: three one-poles and a direct path,
// within 0.5 dB of -3 dB/octave from 10 Hz up.
constexpr std::array pink_poles{0.99765, 0.96300, 0.57000};
constexpr std::array pink_gains{0.0990460, 0.2965164, 1.0526913};
constexpr double pink_direct = 0.1848;

Stereo pink(std::size_t frames) {
  const auto white = oscillate::test::white_noise(frames, programme_seed, programme_amplitude);
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  std::array<double, pink_poles.size()> state{};
  for (std::size_t i = 0; i < frames; ++i) {
    double sum = pink_direct * white[i];
    for (std::size_t k = 0; k < state.size(); ++k) {
      state[k] = pink_poles[k] * state[k] + pink_gains[k] * white[i];
      sum += state[k];
    }
    // Rounded to float as a track would be.
    out.left[i] = static_cast<float>(sum);
    out.right[i] = out.left[i];
  }
  return out;
}

std::vector<double> shape_values(std::span<const match_eq::Band> bands) {
  const auto& table = *shape::engine().descriptor;
  std::vector<double> values;
  for (const auto& p : table.parameters) {
    values.push_back(p.default_value);
  }
  for (const auto& [id, value] : shape::band_values(bands)) {
    values[index(table, id)] = value;
  }
  return values;
}

// What Shape plays for `values`, in dB at every fine band, from its own
// cascade.
match_eq::FineCurve shape_response(const std::vector<double>& values) {
  std::vector<oscillate::native_dsp::EqBandSettings> stages;
  for (std::size_t b = 0; b < shape::bands; ++b) {
    const auto band = shape::band_stages(values, b, match_eq::fit_rate);
    stages.insert(stages.end(), band.stage.begin(),
                  band.stage.begin() + static_cast<std::ptrdiff_t>(band.size));
  }
  const auto prepared =
      oscillate::native_dsp::prepare_eq(stages, static_cast<std::uint32_t>(match_eq::fit_rate));
  REQUIRE_OK(prepared, "Shape's cascade prepares");
  match_eq::FineCurve out{};
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    out[i] = oscillate::native_dsp::eq_magnitude_db(prepared.value(), tonal::fine_hz(i)).value();
  }
  return out;
}

struct Errors {
  double max_db = 0.0;
  double rms_db = 0.0;
};

// `a` against `b` over the comparison range, less their mean difference.
Errors compare(const match_eq::FineCurve& a, const match_eq::FineCurve& b) {
  std::vector<double> difference;
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    const double hz = tonal::fine_hz(i);
    if (hz >= compare_low_hz && hz <= compare_high_hz) {
      difference.push_back(a[i] - b[i]);
    }
  }
  double mean = 0.0;
  for (const double d : difference) {
    mean += d;
  }
  mean /= static_cast<double>(difference.size());
  Errors e;
  for (const double d : difference) {
    e.max_db = std::max(e.max_db, std::abs(d - mean));
    e.rms_db += (d - mean) * (d - mean);
  }
  e.rms_db = std::sqrt(e.rms_db / static_cast<double>(difference.size()));
  return e;
}

struct Known {
  const char* name;
  std::vector<match_eq::Band> bands;
};

// A programme against its own copy through each known curve: the band
// meter reads both, the correction is reference minus mix, the fit draws it.
void own_eq_is_recovered() {
  const auto frames = static_cast<std::size_t>(programme_seconds * sample_rate_48k_hz);
  const auto programme = pink(frames);
  const std::vector<Known> curves{
      {"shelves",
       {{EqBandKind::low_shelf, 120.0, 0.7, 4.0}, {EqBandKind::high_shelf, 8'000.0, 0.7, -3.0}}},
      {"two peaks",
       {{EqBandKind::peaking, 300.0, 1.0, 5.0}, {EqBandKind::peaking, 3'000.0, 2.0, -4.0}}},
      {"tilt and a bump",
       {{EqBandKind::low_shelf, 200.0, 0.5, -3.0},
        {EqBandKind::high_shelf, 3'000.0, 0.6, 3.0},
        {EqBandKind::peaking, 60.0, 1.4, 2.0}}},
      {"a narrow cut and air",
       {{EqBandKind::peaking, 900.0, 4.0, -6.0}, {EqBandKind::high_shelf, 12'000.0, 0.8, 2.5}}},
  };
  for (const auto& known : curves) {
    const auto values = shape_values(known.bands);
    auto runtime = make_effect(shape::engine(), values);
    const auto copy = process(*runtime, programme);
    tonal::BandMeter meter(sample_rate_48k_hz);
    for (std::size_t i = 0; i < frames; ++i) {
      meter.push(0.5 * (programme.left[i] + programme.right[i]),
                 0.5 * (copy.left[i] + copy.right[i]));
    }
    const auto correction = match_eq::correction(tonal::fine_levels(meter.mean_a()),
                                                 tonal::fine_levels(meter.mean_b()), exact);
    const auto fitted = match_eq::fit(correction, exact);
    const auto truth = shape_response(values);
    const auto recovered = compare(fitted.response_db, truth);
    std::cout << known.name << ": " << fitted.size << " bands, fit error " << fitted.rms_db
              << " dB RMS (" << fitted.max_db << " max), recovered within " << recovered.rms_db
              << " dB RMS (" << recovered.max_db << " max)\n";
    require(recovered.max_db <= recovery_tolerance_db,
            std::string(known.name) + " is recovered within the tolerance");
    require(recovered.rms_db <= recovery_rms_tolerance_db,
            std::string(known.name) + " is recovered within the RMS tolerance");
    require(fitted.size <= match_eq::max_bands, "Shape holds the fit");
  }
}

void fit_uses_the_fewest_bands() {
  const match_eq::Band peak{EqBandKind::peaking, 1'000.0, 1.4, 4.0};
  const auto one = match_eq::fit(match_eq::response_db(std::span(&peak, 1)), exact);
  REQUIRE_EQ(one.size, 1, "one peak takes one band");
  require(one.band[0].kind == EqBandKind::peaking &&
              std::abs(one.band[0].hz / peak.hz - 1.0) < band_hz_tolerance &&
              std::abs(one.band[0].shape / peak.shape - 1.0) < band_shape_tolerance &&
              std::abs(one.band[0].gain_db - peak.gain_db) < band_gain_tolerance_db,
          "and finds it");
  const std::array two{match_eq::Band{EqBandKind::low_shelf, 150.0, 0.7, -3.0},
                       match_eq::Band{EqBandKind::peaking, 2'500.0, 2.0, 3.0}};
  const auto pair = match_eq::fit(match_eq::response_db(two), exact);
  require(pair.size <= two.size() && pair.rms_db <= match_eq::fit_tolerance_db,
          "a shelf and a peak take two bands at most");
  const auto flat = match_eq::fit(match_eq::FineCurve{}, exact);
  REQUIRE_EQ(flat.size, 0, "a flat curve takes none");
}

// The fit's response and errors are what Shape plays once written.
void fit_reports_what_shape_plays() {
  const std::array bands{match_eq::Band{EqBandKind::low_shelf, 90.0, 0.6, 3.0},
                         match_eq::Band{EqBandKind::peaking, 400.0, 1.2, -2.5},
                         match_eq::Band{EqBandKind::peaking, 5'000.0, 3.0, 2.0},
                         match_eq::Band{EqBandKind::high_shelf, 11'000.0, 0.9, -1.5}};
  auto curve = match_eq::response_db(bands);
  // A wobble no band count can follow exactly.
  constexpr double wobble_db = 0.3;
  constexpr double wobble_cycles = 0.37;
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    curve[i] += wobble_db * std::sin(wobble_cycles * static_cast<double>(i));
  }
  const auto fitted = match_eq::fit(curve, exact);
  const auto played = shape_response(shape_values(std::span(fitted.band.data(), fitted.size)));
  double sum = 0.0;
  double count = 0.0;
  double largest = 0.0;
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    require(std::abs(played[i] - fitted.response_db[i]) <
                oscillate::test::tolerance::double_accumulation,
            "the fit's response is Shape's");
    const double hz = tonal::fine_hz(i);
    if (hz >= exact.low_hz && hz <= exact.high_hz) {
      const double e = curve[i] - played[i];
      sum += e * e;
      count += 1.0;
      largest = std::max(largest, std::abs(e));
    }
  }
  require(std::abs(std::sqrt(sum / count) - fitted.rms_db) <
                  oscillate::test::tolerance::double_accumulation &&
              std::abs(largest - fitted.max_db) < oscillate::test::tolerance::double_accumulation,
          "the reported error is Shape's");
  require(fitted.rms_db <= wobble_db, "the fit follows the curve under its wobble");
}

void correction_follows_its_settings() {
  match_eq::FineCurve mix{};
  match_eq::FineCurve reference{};
  constexpr double step_db = 6.0;
  constexpr double step_hz = 1'000.0;
  constexpr double ripple_db = 1.0;
  constexpr double gain_db = 9.0;
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    mix[i] = (i % 2 == 0 ? ripple_db : -ripple_db) - static_cast<double>(i) / 10.0;
    reference[i] = -static_cast<double>(i) / 10.0 + (tonal::fine_hz(i) > step_hz ? step_db : 0.0);
  }
  const auto base = match_eq::correction(mix, reference, exact);
  auto louder = mix;
  for (auto& level : louder) {
    level += gain_db;
  }
  const auto same = match_eq::correction(louder, reference, exact);
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    require(std::abs(same[i] - base[i]) < oscillate::test::tolerance::double_accumulation,
            "the mix's gain does not move the correction");
  }
  auto half = exact;
  half.amount = 0.5;
  const auto halved = match_eq::correction(mix, reference, half);
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    require(std::abs(halved[i] - 0.5 * base[i]) < oscillate::test::tolerance::double_accumulation,
            "Amount scales it");
  }
  auto limited = exact;
  limited.max_boost_db = 1.0;
  limited.max_cut_db = 2.0;
  limited.low_hz = 200.0;
  limited.high_hz = 4'000.0;
  const auto held = match_eq::correction(mix, reference, limited);
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    const double hz = tonal::fine_hz(i);
    require(held[i] <= limited.max_boost_db && held[i] >= -limited.max_cut_db,
            "boost and cut hold it");
    if (hz < limited.low_hz * std::exp2(-match_eq::fade_octaves) ||
        hz > limited.high_hz * std::exp2(match_eq::fade_octaves)) {
      require(held[i] == 0.0, "nothing beyond the limits and their fade");
    }
  }
  // Smoothing over a sixth of an octave and more takes the alternating
  // ripple out.
  auto wide = exact;
  wide.smoothing_octaves = 1.0;
  const auto smooth = match_eq::correction(mix, reference, wide);
  double ripple = 0.0;
  for (std::size_t i = 1; i + 1 < tonal::fine_bands; ++i) {
    if (tonal::fine_hz(i) > exact.low_hz && tonal::fine_hz(i) < step_hz / 2.0) {
      ripple = std::max(ripple, std::abs(smooth[i] - 0.5 * (smooth[i - 1] + smooth[i + 1])));
    }
  }
  require(ripple < 0.1 * ripple_db, "smoothing takes out the ripple");
}

// A target whose median rises one dB a band reads, at each fine band, its
// position on the grid in bands: fine band i sits (i + 1/2) / 4 - 1/2 bands
// from the first centre. Beyond the end centres it holds their levels.
void targets_become_fine_levels() {
  tonal::Target target;
  for (std::size_t b = 0; b < tonal::bands; ++b) {
    target.median[b] = static_cast<double>(b);
  }
  const auto levels = match_eq::target_levels(target);
  const auto per = static_cast<double>(tonal::fine_per_band);
  for (std::size_t i = 0; i < tonal::fine_bands; ++i) {
    const double position = std::clamp((static_cast<double>(i) + 0.5) / per - 0.5, 0.0,
                                       static_cast<double>(tonal::bands - 1));
    require(std::abs(levels[i] - position) < oscillate::test::tolerance::double_accumulation,
            "fine band " + std::to_string(i) + " reads its place between the medians");
  }
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "match EQ contract passed",
      {{"own_eq_is_recovered", own_eq_is_recovered},
       {"fit_uses_the_fewest_bands", fit_uses_the_fewest_bands},
       {"fit_reports_what_shape_plays", fit_reports_what_shape_plays},
       {"correction_follows_its_settings", correction_follows_its_settings},
       {"targets_become_fine_levels", targets_become_fine_levels}});
}

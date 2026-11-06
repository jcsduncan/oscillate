#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/strata.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

// Strata (ADR-0082) against measured oracles: crossovers that sum flat (an
// allpass in minimum phase, the input exactly in linear phase), the static
// curve down and up, attack and release time constants, the range, mid/side
// leaving the other part untouched, an external key, delta as input minus
// output, the reported latency, block splits, hostile input, OTT as its
// bands, an allocation-free callback and the panel's telemetry.
namespace {
using namespace effect_test;
namespace strata = oscillate::native_dsp::effects::strata;

const effects::EffectEngine& engine() { return strata::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
std::string band_id(std::size_t b, std::string_view field) {
  return "strata.band_" + std::to_string(b + 1) + "." + std::string(field);
}
void set_band(std::vector<double>& v, std::size_t b, std::string_view field, double value) {
  v[at(band_id(b, field))] = value;
}
void set_all(std::vector<double>& v, std::string_view field, double value) {
  for (std::size_t b = 0; b < strata::max_bands; ++b) {
    set_band(v, b, field, value);
  }
}

constexpr double rate = oscillate::test::sample_rate_48k_hz;

std::vector<double> init() { return preset(table(), "Init"); }

// Every band at 1:1 both ways: the dynamics do nothing.
std::vector<double> unity(double bands_index) {
  auto v = init();
  v[at("strata.bands")] = bands_index;
  set_all(v, "above", 1.0);
  set_all(v, "below", 1.0);
  return v;
}

// One band (no crossover) with a hard knee and quick timing.
std::vector<double> single(double threshold, double above, double below) {
  auto v = init();
  v[at("strata.bands")] = 0.0;
  set_band(v, 0, "threshold", threshold);
  set_band(v, 0, "above", above);
  set_band(v, 0, "below", below);
  set_band(v, 0, "knee", 0.0);
  set_band(v, 0, "range", 36.0);
  set_band(v, 0, "attack", 1.0);
  set_band(v, 0, "release", 5.0);
  return v;
}

Stereo render(const std::vector<double>& values, const Stereo& input, const Stereo* key = nullptr,
              std::size_t block = 128) {
  auto runtime = make_effect(engine(), values, rate);
  return process(*runtime, input, {.block = block, .sidechain = key, .rate = rate});
}

Stereo constant(double level, std::size_t frames) {
  return {std::vector<double>(frames, level), std::vector<double>(frames, level)};
}

Stereo impulse(std::size_t frames, std::size_t at_frame = 0) {
  Stereo out{std::vector<double>(frames), std::vector<double>(frames)};
  out.left[at_frame] = 1.0;
  out.right[at_frame] = 1.0;
  return out;
}

double gain_db(double x) { return 20.0 * std::log10(std::fabs(x) + 1e-30); }

void descriptor_registry_and_presets() {
  REQUIRE_EQ(effects::find_engine("strata"), &engine(), "Strata is registered");
  require(engine().sidechain, "Strata reads a sidechain");
  REQUIRE_EQ(table().parameters.size(), 18 + strata::max_bands * 13, "the table's size");
  for (const auto& entry : table().presets) {
    const auto values = preset(table(), entry.name);
    auto runtime = make_effect(engine(), values, rate);
    const auto out = process(*runtime, noise(0.3, 9'600));
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]),
              "preset renders finite: " + std::string(entry.name));
    }
  }
  REQUIRE_EQ(table().presets.size(), 7, "seven presets");
}

// Minimum phase: an impulse through six unity bands has a flat magnitude.
// Linear phase: the impulse comes back exactly, delayed.
void crossovers_sum_flat() {
  const auto v = unity(5.0);
  const auto out = render(v, impulse(16'384));
  std::vector<std::complex<double>> spectrum(16'384);
  for (std::size_t i = 0; i < spectrum.size(); ++i) {
    spectrum[i] = out.left[i];
  }
  fft(spectrum);
  double worst = 0.0;
  for (std::size_t bin = 1; bin < spectrum.size() / 2; ++bin) {
    const double hz = static_cast<double>(bin) * rate / static_cast<double>(spectrum.size());
    if (hz >= 20.0 && hz <= 20'000.0) {
      worst = std::max(worst, std::fabs(gain_db(std::abs(spectrum[bin]))));
    }
  }
  REQUIRE_LT(worst, 0.1, "minimum phase sums flat within 0.1 dB: " + std::to_string(worst));

  auto linear = v;
  linear[at("strata.phase")] = 1.0;
  const auto delay = engine().latency_frames(linear, rate);
  REQUIRE_EQ(delay, 1'023 + 512, "linear phase latency at 48 kHz: " + std::to_string(delay));
  const auto echo = render(linear, impulse(8'192, 100));
  for (std::size_t i = 0; i < echo.size(); ++i) {
    const double expected = i == 100 + delay ? 1.0 : 0.0;
    REQUIRE_LT(std::fabs(echo.left[i] - expected), tolerance::float_processing,
               "linear phase nulls against the delayed input at " + std::to_string(i));
  }
}

// A steady level lands where the formula says: 20 dB over -30 at 4:1 comes
// out 15 dB down; 20 dB under it at 1:2 below comes up 10 dB.
void the_static_curve_matches_the_formula() {
  struct Case {
    double threshold, above, below, level_db, expected_db;
  };
  for (const Case c : {Case{-30, 4, 1, -10, -15}, Case{-30, 1, 2, -50, 10},
                       Case{-20, 0.5, 1, -10, 10}, Case{-20, 1, 0.5, -30, -10}}) {
    const auto v = single(c.threshold, c.above, c.below);
    const double level = oscillate::native_dsp::dsp::db_to_gain(c.level_db);
    const auto out = render(v, constant(level, 24'000));
    const double change = gain_db(out.left.back()) - c.level_db;
    REQUIRE_LT(std::fabs(change - c.expected_db), 0.05,
               "steady change " + std::to_string(change) + " expected " +
                   std::to_string(c.expected_db));
    strata::Curve curve{c.threshold, c.above, c.below, 0.0, 36.0, 0.0};
    REQUIRE_LT(std::fabs(strata::gain_change_db(c.level_db, curve) - c.expected_db),
               tolerance::double_accumulation, "the formula itself");
  }
  // The knee: halfway in, at the threshold, the change is a quarter of the
  // way to the hard curve's at its top edge: -(1 - 1/4) * 6 / 8.
  strata::Curve knee{-30, 4, 1, 12, 36, 0};
  REQUIRE_LT(std::fabs(strata::gain_change_db(-30.0, knee) + 0.75 * 12.0 / 8.0),
             tolerance::double_accumulation, "the soft knee at the threshold");
}

// From under the threshold to 20 dB over it at 4:1: after the attack time
// the change is 63% of the way to -15 dB; back down, after the release time
// 37% of it is left.
void attack_and_release_are_time_constants() {
  auto v = single(-30, 4, 1);
  set_band(v, 0, "attack", 20.0);
  set_band(v, 0, "release", 200.0);
  const std::size_t step = 4'800;
  Stereo input = constant(std::pow(10.0, -10.0 / 20.0), 48'000);
  for (std::size_t i = 0; i < step; ++i) {
    input.left[i] = input.right[i] = 0.001;
  }
  const std::size_t fall = 24'000;
  for (std::size_t i = fall; i < input.size(); ++i) {
    input.left[i] = input.right[i] = 0.01;
  }
  const auto out = render(v, input);
  const auto attack = static_cast<std::size_t>(0.020 * rate);
  const double after_attack = gain_db(out.left[step + attack]) + 10.0;
  REQUIRE_LT(std::fabs(after_attack + 15.0 * (1.0 - std::exp(-1.0))), 0.2,
             "attack is the time constant: " + std::to_string(after_attack));
  const auto release = static_cast<std::size_t>(0.200 * rate);
  const double after_release = gain_db(out.left[fall + release]) + 40.0;
  REQUIRE_LT(std::fabs(after_release + 15.0 * std::exp(-1.0)), 0.2,
             "release is the time constant: " + std::to_string(after_release));
}

void range_limits_the_change() {
  auto v = single(-40, 20, 1);
  set_band(v, 0, "range", 6.0);
  const auto out = render(v, constant(std::pow(10.0, -10.0 / 20.0), 24'000));
  const double change = gain_db(out.left.back()) + 10.0;
  REQUIRE_LT(std::fabs(change + 6.0), 0.05, "the range caps the cut: " + std::to_string(change));
  auto up = single(-20, 1, 20);
  set_band(up, 0, "range", 6.0);
  const auto lifted = render(up, constant(std::pow(10.0, -50.0 / 20.0), 24'000));
  REQUIRE_LT(std::fabs(gain_db(lifted.left.back()) + 50.0 - 6.0), 0.05, "and the lift");
}

// Mid only: the side passes untouched while the mid is compressed; side
// only, the other way round.
void mid_side_touch_only_their_part() {
  const auto mid = sine(1'000.0, 0.5, 24'000);
  const auto side = sine(3'100.0, 0.05, 24'000);
  Stereo input{std::vector<double>(24'000), std::vector<double>(24'000)};
  for (std::size_t i = 0; i < input.size(); ++i) {
    input.left[i] = mid.left[i] + side.left[i];
    input.right[i] = mid.left[i] - side.left[i];
  }
  for (const auto [process, name] : {std::pair{1.0, "mid"}, std::pair{2.0, "side"}}) {
    auto v = single(-50, 10, 1);
    set_band(v, 0, "process", process);
    const auto out = render(v, input);
    double kept = 0.0;
    double changed_in = 0.0;
    double changed_out = 0.0;
    for (std::size_t i = 12'000; i < out.size(); ++i) {
      const double in_m = 0.5 * (input.left[i] + input.right[i]);
      const double in_s = 0.5 * (input.left[i] - input.right[i]);
      const double out_m = 0.5 * (out.left[i] + out.right[i]);
      const double out_s = 0.5 * (out.left[i] - out.right[i]);
      const bool on_mid = process == 1.0;
      kept = std::max(kept, std::fabs((on_mid ? out_s : out_m) - (on_mid ? in_s : in_m)));
      changed_in += std::pow(on_mid ? in_m : in_s, 2.0);
      changed_out += std::pow(on_mid ? out_m : out_s, 2.0);
    }
    REQUIRE_LT(kept, 1e-6, std::string(name) + ": the other part is untouched");
    REQUIRE_LT(10.0 * std::log10(changed_out / changed_in), -6.0,
               std::string(name) + ": its own part is compressed");
  }
}

// Keyed from the sidechain: a quiet input under the threshold is pushed
// down while the key is loud; with no key connected, the band keys itself.
void an_external_key_drives_the_band() {
  auto v = single(-30, 10, 1);
  set_band(v, 0, "key", 2.0);
  set_band(v, 0, "release", 200.0);
  const auto input = sine(500.0, 0.01, 24'000);
  const auto key = sine(60.0, 0.5, 24'000);
  const auto keyed = render(v, input, &key);
  const auto alone = render(v, input);
  const double drop = db(rms(keyed.left, 12'000, 24'000) / rms(input.left, 12'000, 24'000));
  REQUIRE_LT(drop, -15.0, "the key pushes the band down: " + std::to_string(drop));
  const double kept = db(rms(alone.left, 12'000, 24'000) / rms(input.left, 12'000, 24'000));
  REQUIRE_LT(std::fabs(kept), 0.1, "unkeyed, the quiet band is left alone");
}

// Linear phase, three working bands: delta plus output is the input,
// delayed by the latency.
void delta_is_the_input_less_the_output() {
  auto v = preset(table(), "Glue the drums");
  v[at("strata.phase")] = 1.0;
  v[at("strata.mix")] = 1.0;
  const auto input = noise(0.4, 24'000);
  const auto out = render(v, input);
  auto delta = v;
  delta[at("strata.delta")] = 1.0;
  const auto removed = render(delta, input);
  const auto latency = engine().latency_frames(v, rate);
  double worst = 0.0;
  double moved = 0.0;
  for (std::size_t i = latency; i < input.size(); ++i) {
    worst = std::max(worst, std::fabs(out.left[i] + removed.left[i] - input.left[i - latency]));
    moved = std::max(moved, std::fabs(removed.left[i]));
  }
  REQUIRE_LT(worst, 1e-5, "delta + output = input: " + std::to_string(worst));
  REQUIRE_GT(moved, 1e-3, "and the dynamics did something");
}

void the_latency_is_reported_and_exact() {
  auto v = unity(0.0);
  v[at("strata.lookahead")] = 5.0;
  const auto look = engine().latency_frames(v, rate);
  REQUIRE_EQ(look, 240, "5 ms of lookahead at 48 kHz");
  const auto out = render(v, impulse(2'048, 10));
  REQUIRE_LT(std::fabs(out.left[10 + look] - 1.0), tolerance::float_round_trip,
             "the impulse arrives at the latency");
  v[at("strata.phase")] = 1.0;
  const auto both = engine().latency_frames(v, rate);
  REQUIRE_EQ(both, 240 + 1'023 + 512, "lookahead plus the linear bank");
  const auto later = render(v, impulse(4'096, 10));
  REQUIRE_LT(std::fabs(later.left[10 + both] - 1.0), tolerance::float_processing,
             "the linear impulse too");
  REQUIRE_EQ(engine().latency_frames(v, 96'000.0), 480 + 2'046 + 1'024, "doubled at 96 kHz");
}

void any_block_split_renders_the_same() {
  const auto input = noise(0.4, 20'000);
  for (const auto* name : {"Glue the drums", "Master polish", "OTT 30%", "De-mud the pad"}) {
    const auto v = preset(table(), name);
    const auto a = render(v, input, nullptr, 128);
    const auto b = render(v, input, nullptr, 0);
    for (std::size_t i = 0; i < input.size(); ++i) {
      require(a.left[i] == b.left[i] && a.right[i] == b.right[i],
              std::string(name) + ": the split changed frame " + std::to_string(i));
    }
  }
}

void hostile_input_stays_finite() {
  Stereo input = noise(0.5, 9'600);
  input.left[100] = std::numeric_limits<double>::quiet_NaN();
  input.right[200] = std::numeric_limits<double>::infinity();
  input.left[300] = 1e30;
  for (std::size_t i = 4'000; i < input.size(); ++i) {
    input.left[i] = input.right[i] = 1e-30;
  }
  for (const auto* name : {"Init", "OTT 30%", "Master polish"}) {
    const auto out = render(preset(table(), name), input);
    for (std::size_t i = 0; i < out.size(); ++i) {
      require(std::isfinite(out.left[i]) && std::isfinite(out.right[i]),
              std::string(name) + ": finite output");
      REQUIRE_NE(std::fpclassify(static_cast<float>(out.left[i])), FP_SUBNORMAL,
                 std::string(name) + ": no denormals");
    }
  }
}

// OTT mode plays exactly what its bands, written out, play.
void ott_mode_is_its_bands() {
  auto v = preset(table(), "OTT 30%");
  v[at("strata.ott.depth")] = 0.8;
  v[at("strata.ott.balance")] = -0.3;
  auto written = v;
  for (const auto& named : strata::ott_values(v)) {
    written[at(named.id)] = named.value;
  }
  REQUIRE_EQ(written[at("strata.mode")], 0.0, "the written values are Bands mode");
  const auto input = noise(0.3, 12'000);
  const auto a = render(v, input);
  const auto b = render(written, input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_LT(std::fabs(a.left[i] - b.left[i]), tolerance::float_round_trip,
               "OTT renders as its bands");
  }
}

void the_callback_never_allocates() {
  const auto values = preset(table(), "Master polish");
  auto runtime = make_effect(engine(), values, rate);
  const auto key = noise(0.2, 4'096);
  (void)process(*runtime, noise(0.3, 4'096), {.sidechain = &key});
  auto changed = values;
  changed[at("strata.xover_2")] = 700.0;
  changed[at("strata.learn")] = 1.0;
  set_all(changed, "key", 2.0);
  require(runtime->offer_values(changed, 1), "new crossovers are offered");
  std::vector<engine::StereoFrame> buffer(128, {0.2F, -0.2F});
  std::vector<engine::StereoFrame> side(128, {0.1F, 0.1F});
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < 2'000; ++block) {
    const SynthTransport transport{0.0, 0.0, 0.0, 4.0};
    (void)runtime->process(buffer, side, 4'096 + block * buffer.size(), block < 1'000 ? 1U : 2U,
                           transport, {});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the callback allocates nothing");
}

void telemetry_feeds_the_panel() {
  auto v = preset(table(), "Glue the drums");
  v[at("strata.learn")] = 1.0;
  auto runtime = make_effect(engine(), v, rate);
  (void)process(*runtime, noise(0.3, static_cast<std::size_t>(4.5 * rate)));
  std::vector<float> frame;
  REQUIRE_TRUE(runtime->read_telemetry(frame), "a telemetry frame");
  REQUIRE_EQ(frame.size(), strata::telemetry::size, "a telemetry frame");
  namespace t = strata::telemetry;
  REQUIRE_EQ(frame[t::band_count], 3.0F, "three bands");
  REQUIRE_LT(std::fabs(frame[t::crossovers] - 150.0F), 0.01F, "the crossovers as they play");
  REQUIRE_EQ(frame[t::crossovers + 2], 0.0F, "the crossovers as they play");
  REQUIRE_GE(frame[t::learn_progress], 1.0F, "Learn has finished");
  for (std::size_t b = 0; b < 3; ++b) {
    require(frame[t::learned + b] > -60.0F && frame[t::learned + b] <= 0.0F,
            "a threshold suggestion for band " + std::to_string(b + 1));
    REQUIRE_GT(frame[t::band_level + b], -60.0F, "a band level");
  }
  REQUIRE_EQ(frame[t::learned + 3], strata::silent_db, "no suggestion for a silent band");
  REQUIRE_GT(frame[t::input_peak], -12.0F, "peaks");
  REQUIRE_GT(frame[t::output_peak], -40.0F, "peaks");
  double loudest = -200.0;
  for (std::size_t i = 0; i < strata::display_bands; ++i) {
    loudest = std::max(loudest, static_cast<double>(frame[t::input_spectrum + i]));
  }
  REQUIRE_GT(loudest, -80.0, "a spectrum");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native strata contract passed",
      {
          {"descriptor_registry_and_presets", [] { descriptor_registry_and_presets(); }},
          {"crossovers_sum_flat", [] { crossovers_sum_flat(); }},
          {"the_static_curve_matches_the_formula", [] { the_static_curve_matches_the_formula(); }},
          {"attack_and_release_are_time_constants",
           [] { attack_and_release_are_time_constants(); }},
          {"range_limits_the_change", [] { range_limits_the_change(); }},
          {"mid_side_touch_only_their_part", [] { mid_side_touch_only_their_part(); }},
          {"an_external_key_drives_the_band", [] { an_external_key_drives_the_band(); }},
          {"delta_is_the_input_less_the_output", [] { delta_is_the_input_less_the_output(); }},
          {"the_latency_is_reported_and_exact", [] { the_latency_is_reported_and_exact(); }},
          {"any_block_split_renders_the_same", [] { any_block_split_renders_the_same(); }},
          {"hostile_input_stays_finite", [] { hostile_input_stays_finite(); }},
          {"ott_mode_is_its_bands", [] { ott_mode_is_its_bands(); }},
          {"the_callback_never_allocates", [] { the_callback_never_allocates(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}

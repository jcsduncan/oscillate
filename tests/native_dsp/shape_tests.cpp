#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/effects/shape.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shape's slope (ADR-0047): a pass band is 1 to 8 Butterworth stages, 12 to
// 96 dB an octave; 12 dB is the classic band, and a slope changed live adds
// or removes stages one at a time without a click.
namespace {

using namespace effect_test;
namespace shape = oscillate::native_dsp::effects::shape;
using oscillate::native_dsp::EqBandKind;

constexpr double rate = oscillate::test::sample_rate_48k_hz;

const effects::EffectEngine& engine() {
  const auto* found = effects::find_engine("shape");
  REQUIRE_NE(found, nullptr, "Shape is registered");
  return *found;
}

const EngineDescriptor& table() { return *engine().descriptor; }

std::vector<double> with(std::vector<double> values,
                         std::initializer_list<std::pair<std::string_view, double>> set) {
  for (const auto& [id, value] : set) {
    values[index(table(), id)] = value;
  }
  return values;
}

// Band 1 as a high-pass at `hz` with slope choice `slope`.
std::vector<double> high_pass(double hz, double slope) {
  return with(default_values(table()), {{"shape.on_1", 1},
                                        {"shape.kind_1", 1},
                                        {"shape.freq_1", hz},
                                        {"shape.q_1", oscillate::native_dsp::dsp::butterworth_q},
                                        {"shape.slope_1", slope}});
}

void band_stages_follow_the_slope() {
  for (std::size_t slope = 0; slope < 4; ++slope) {
    const auto stages = shape::band_stages(high_pass(30, static_cast<double>(slope)), 0, rate);
    REQUIRE_EQ(stages.size, (std::size_t{1} << slope), "a slope's stages");
    for (std::size_t s = 0; s < stages.size; ++s) {
      REQUIRE_EQ(stages.stage[s].kind, EqBandKind::high_pass,
                 "every stage is the band's filter at its corner");
      REQUIRE_EQ(stages.stage[s].frequency_hz, 30,
                 "every stage is the band's filter at its corner");
    }
  }
  // Only a pass filter has a slope: a bell is one stage whatever it says.
  const auto bell = with(high_pass(1'000, 3), {{"shape.kind_1", 4}, {"shape.gain_1", 6}});
  REQUIRE_EQ(shape::band_stages(bell, 0, rate).size, 1, "a bell ignores the slope");
  REQUIRE_EQ(shape::band_stages(default_values(table()), 0, rate).size, 0, "a band off has none");
  // 12 dB is the classic band: its Q passes through unchanged.
  const auto classic = shape::band_stages(with(high_pass(80, 0), {{"shape.q_1", 2.5}}), 0, rate);
  REQUIRE_EQ(classic.size, 1, "12 dB is one stage at its Q");
  REQUIRE_EQ(classic.stage[0].shape, 2.5, "12 dB is one stage at its Q");
}

// A sine an octave below the corner falls by the slope; the 96 dB render
// is far below the 12 dB one.
void slopes_cut_at_their_rate() {
  const auto input = sine(500.0, 0.5, 72'000);
  const std::array expected{-12.3, -24.1, -48.2, -96.3};
  for (std::size_t slope = 0; slope < 4; ++slope) {
    auto runtime = make_effect(engine(), high_pass(1'000, static_cast<double>(slope)));
    const auto out = process(*runtime, input);
    const double gain = db(rms(out.left, 48'000, 72'000) / rms(input.left, 48'000, 72'000));
    REQUIRE_LT(std::abs(gain - expected[slope]), 1.0,
               "slope " + std::to_string(slope) + " cuts " + std::to_string(gain) + " dB");
  }
}

// Switching the slope live steps through the stages to the full depth.
void a_live_slope_reaches_its_depth() {
  const auto input = sine(500.0, 0.5, 96'000);
  auto runtime = make_effect(engine(), high_pass(1'000, 0));
  const auto steep = high_pass(1'000, 3);
  const auto out =
      process(*runtime, input, {.block = 256, .at = {12'000}, .before = [&](std::uint64_t) {
                                  require(runtime->offer_live_values(steep), "a live slope");
                                }});
  const double gain = db(rms(out.left, 72'000, 96'000) / rms(input.left, 72'000, 96'000));
  REQUIRE_LT(std::abs(gain + 96.3), 1.0,
             "the live slope is the saved one: " + std::to_string(gain));
}

// A 1 kHz sine above a 30 Hz cut while the slope jumps 12 -> 96 -> 12 and the
// band switches off and on: no step larger than the sine's own curvature.
void slope_changes_never_click() {
  const auto input = sine(1'000.0, 0.5, 48'000);
  auto runtime = make_effect(engine(), high_pass(30, 0));
  const std::vector<std::uint64_t> at{6'000, 18'000, 30'000, 40'000};
  const std::array changes{high_pass(30, 3), high_pass(30, 0),
                           with(high_pass(30, 3), {{"shape.on_1", 0}}), high_pass(30, 3)};
  std::size_t next = 0;
  const auto out = process(*runtime, input, {.block = 128, .at = at, .before = [&](std::uint64_t) {
                                               require(runtime->offer_live_values(changes[next++]),
                                                       "a live change");
                                             }});
  // The sine's largest second difference is A * w^2 (about 0.0086).
  const double w = 2.0 * std::numbers::pi * 1'000.0 / rate;
  double worst = 0.0;
  for (std::size_t i = 2; i < out.size(); ++i) {
    worst = std::max(worst, std::abs(out.left[i] - 2.0 * out.left[i - 1] + out.left[i - 2]));
  }
  REQUIRE_LT(worst, 2.0 * 0.5 * w * w, "no click: " + std::to_string(worst));
}

void any_block_split_renders_identically() {
  const auto input = noise(0.3, 20'000);
  const auto values = with(high_pass(60, 3), {{"shape.on_8", 1}, {"shape.slope_8", 2}});
  auto even = make_effect(engine(), values);
  auto odd = make_effect(engine(), values);
  const auto a = process(*even, input, {.block = 128});
  const auto b = process(*odd, input, {.block = 0});
  REQUIRE_EQ(a.left, b.left, "block splits render alike");
  REQUIRE_EQ(a.right, b.right, "block splits render alike");
}

// The display spectra share one transform: the input's band at a tone reads
// the tone, while the output's band reads what a high-pass far above it
// left.
void display_spectra_read_input_and_output_apart() {
  constexpr double tone_hz = 1'000.0;
  constexpr double amplitude = 0.5;
  constexpr double cut_hz = 8'000.0;
  constexpr double steepest_slope = 3; // 96 dB an octave
  constexpr double tolerance_db = 1.0;
  constexpr double removed_db = -60.0;
  // Band b covers 20 * 1000^(b / bands) Hz up to the next.
  const auto band_of = [](double hz) {
    return static_cast<std::size_t>(std::log(hz / 20.0) / std::log(1'000.0) *
                                    static_cast<double>(shape::spectrum_bands));
  };
  auto runtime = make_effect(engine(), high_pass(cut_hz, steepest_slope));
  (void)process(*runtime, sine(tone_hz, amplitude, static_cast<std::size_t>(rate)));
  std::vector<float> t;
  REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
  const auto band = band_of(tone_hz);
  const double input_db = t[shape::telemetry::input_spectrum + band];
  const double output_db = t[shape::telemetry::output_spectrum + band];
  REQUIRE_LT(std::fabs(input_db - 20.0 * std::log10(amplitude)), tolerance_db,
             "the input spectrum reads the tone: " + std::to_string(input_db));
  REQUIRE_LT(output_db, removed_db,
             "the output spectrum reads the cut: " + std::to_string(output_db));
}

// EQs created together analyse in different blocks, so their transforms do
// not pile into one callback.
void spectra_of_eqs_started_together_land_in_different_blocks() {
  constexpr std::size_t block = 128;
  constexpr float silent_db = -200.0F;
  const auto input = sine(1'000.0, 0.5, static_cast<std::size_t>(rate));
  const auto first_analysis = [&](effects::NativeEffectRuntime& runtime) {
    std::vector<float> t;
    for (std::size_t start = 0; start + block <= input.size(); start += block) {
      Stereo slice{
          std::vector<double>(input.left.begin() + static_cast<std::ptrdiff_t>(start),
                              input.left.begin() + static_cast<std::ptrdiff_t>(start + block)),
          std::vector<double>(input.right.begin() + static_cast<std::ptrdiff_t>(start),
                              input.right.begin() + static_cast<std::ptrdiff_t>(start + block))};
      (void)process(runtime, slice, {.block = block}, start);
      if (runtime.read_telemetry(t) &&
          std::ranges::any_of(
              std::span(t).subspan(shape::telemetry::input_spectrum, shape::spectrum_bands),
              [&](float value) { return value > silent_db; }))
        return start;
    }
    return input.size();
  };
  auto first = make_effect(engine(), default_values(table()));
  auto second = make_effect(engine(), default_values(table()));
  const auto a = first_analysis(*first);
  const auto b = first_analysis(*second);
  require(a < input.size() && b < input.size(), "both analyse");
  REQUIRE_NE(a, b, "two EQs started together analyse in different blocks");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "shape slope contract passed",
      {
          {"band_stages_follow_the_slope", [] { band_stages_follow_the_slope(); }},
          {"slopes_cut_at_their_rate", [] { slopes_cut_at_their_rate(); }},
          {"a_live_slope_reaches_its_depth", [] { a_live_slope_reaches_its_depth(); }},
          {"slope_changes_never_click", [] { slope_changes_never_click(); }},
          {"any_block_split_renders_identically", [] { any_block_split_renders_identically(); }},
          {"display_spectra_read_input_and_output_apart",
           [] { display_spectra_read_input_and_output_apart(); }},
          {"spectra_of_eqs_started_together_land_in_different_blocks",
           [] { spectra_of_eqs_started_together_land_in_different_blocks(); }},
      });
}

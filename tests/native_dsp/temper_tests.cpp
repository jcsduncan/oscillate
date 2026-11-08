#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/temper.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Temper (ADR-0064) against measured oracles: transparency at rest in every
// phase, exact latency, a resonance cut while the noise around it is left,
// Max Cut, the sensitivity curve, Split sparing attacks, Unmask carving
// where the key plays, dynamic bands against their threshold, stereo modes
// leaving the rest untouched, Delta plus output equal to the input, and the
// panel's telemetry.
namespace {
using namespace effect_test;
namespace temper = oscillate::native_dsp::effects::temper;

const effects::EffectEngine& engine() { return temper::engine(); }
const EngineDescriptor& table() { return *engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::array phases{0.0, 1.0, 2.0}; // Low Latency, Natural, Linear

std::vector<double> defaults() { return preset(table(), "Default"); }

// Defaults with the suppressor at rest: nothing is cut until a test asks.
std::vector<double> at_rest() {
  auto v = defaults();
  v[at("temper.depth")] = 0.0;
  return v;
}

std::uint32_t latency(const std::vector<double>& v, double r = rate) {
  return engine().latency_frames(v, r);
}

Stereo render(const std::vector<double>& values, const Stereo& input, const Stereo* key = nullptr,
              double r = rate) {
  auto runtime = make_effect(engine(), values, r);
  return process(*runtime, input, {.block = 0, .sidechain = key, .rate = r});
}

Stereo add(Stereo a, const Stereo& b, double gain = 1.0) {
  for (std::size_t i = 0; i < a.size(); ++i) {
    a.left[i] += gain * b.left[i];
    a.right[i] += gain * b.right[i];
  }
  return a;
}

// The largest difference between the output and the input `delay` earlier.
double delayed_error(const std::vector<double>& out, const std::vector<double>& in,
                     std::size_t delay) {
  double worst = 0.0;
  for (std::size_t i = delay; i < out.size(); ++i) {
    worst = std::max(worst, std::fabs(out[i] - in[i - delay]));
  }
  return worst;
}

// The level of a tone (dB re amplitude 1) over [from, to).
double tone_db(const std::vector<double>& x, double hz, std::size_t from, std::size_t to) {
  std::complex<double> sum = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    sum += x[i] * std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
  }
  return db(2.0 * std::abs(sum) / static_cast<double>(to - from));
}

// The power in [lo, hi] Hz of a Hann-windowed 16384-frame span from `from`
// (dB, relative).
double band_db(const std::vector<double>& x, double lo, double hi, std::size_t from) {
  std::vector<std::complex<double>> window(16'384);
  for (std::size_t i = 0; i < window.size(); ++i) {
    const double hann = oscillate::native_dsp::dsp::hann(static_cast<double>(i),
                                                         static_cast<double>(window.size()));
    window[i] = x[from + i] * hann;
  }
  fft(window);
  double power = 0.0;
  for (std::size_t bin = 1; bin < window.size() / 2; ++bin) {
    const double hz = static_cast<double>(bin) * rate / static_cast<double>(window.size());
    if (hz >= lo && hz <= hi) {
      power += std::norm(window[bin]);
    }
  }
  return 10.0 * std::log10(power + 1e-30);
}

void at_rest_it_is_the_input_delayed() {
  const auto input = add(noise(0.3, 48'000), sine(3'000.0, 0.4, 48'000));
  for (const double phase : phases) {
    auto v = at_rest();
    v[at("temper.phase")] = phase;
    const auto out = render(v, input);
    const auto delay = latency(v);
    require(delayed_error(out.left, input.left, delay) < 1e-5 &&
                delayed_error(out.right, input.right, delay) < 1e-5,
            "Depth 0 passes the input, delayed by the latency: phase " + std::to_string(phase));
  }
}

void the_latency_is_exact() {
  const std::array<std::uint32_t, 3> expected{0, 768, 1'280};
  for (std::size_t i = 0; i < phases.size(); ++i) {
    auto v = at_rest();
    v[at("temper.phase")] = phases[i];
    REQUIRE_EQ(latency(v), expected[i], "latency at 48 kHz: " + std::to_string(i));
    Stereo click{std::vector<double>(8'192), std::vector<double>(8'192)};
    click.left[100] = 1.0;
    click.right[100] = 1.0;
    const auto out = render(v, click);
    const auto loudest = static_cast<std::size_t>(
        std::ranges::max_element(out.left,
                                 [](double a, double b) { return std::fabs(a) < std::fabs(b); }) -
        out.left.begin());
    REQUIRE_EQ(loudest, 100 + latency(v), "the click comes out exactly a latency later");
    // Twice the rate, twice the frames: the same time.
    REQUIRE_EQ(latency(v, 2.0 * rate), 2 * latency(v), "latency scales with the rate");
  }
}

// A loud 3 kHz ring over noise: the ring is cut, the noise around it stays.
Stereo ringing(std::size_t frames) { return add(noise(0.1, frames), sine(3'000.0, 0.5, frames)); }

void a_resonance_is_cut_and_the_rest_is_left() {
  const std::size_t frames = 96'000;
  const auto input = ringing(frames);
  for (const double phase : phases) {
    auto v = defaults();
    v[at("temper.depth")] = 10.0;
    v[at("temper.phase")] = phase;
    const auto out = render(v, input);
    const auto delay = latency(v);
    const std::size_t from = 48'000;
    const double before = tone_db(input.left, 3'000.0, from - delay, frames - delay);
    const double after = tone_db(out.left, 3'000.0, from, frames);
    REQUIRE_GT(before - after, 6.0,
               "the ring is cut by more than 6 dB: phase " + std::to_string(phase) + " " +
                   std::to_string(before - after));
    const double noise_in = band_db(input.left, 300.0, 1'200.0, from - delay);
    const double noise_out = band_db(out.left, 300.0, 1'200.0, from);
    REQUIRE_LT(std::fabs(noise_in - noise_out), 0.5,
               "noise an octave and more away is left: " + std::to_string(noise_in - noise_out));
  }
}

double ring_cut(const std::vector<double>& values) {
  const std::size_t frames = 96'000;
  const auto input = ringing(frames);
  const auto out = render(values, input);
  const auto delay = latency(values);
  return tone_db(input.left, 3'000.0, 48'000 - delay, frames - delay) -
         tone_db(out.left, 3'000.0, 48'000, frames);
}

void max_cut_bounds_the_cut() {
  auto v = defaults();
  v[at("temper.depth")] = 20.0;
  v[at("temper.max_cut")] = 3.0;
  const double cut = ring_cut(v);
  require(cut > 1.5 && cut < 3.3, "Max Cut 3 dB bounds the cut: " + std::to_string(cut));
}

void depth_and_sensitivity_steer_the_cut() {
  auto v = defaults();
  v[at("temper.depth")] = 4.0;
  const double plain = ring_cut(v);
  v[at("temper.depth")] = 12.0;
  REQUIRE_GT(ring_cut(v), plain + 2.0, "more Depth cuts more");
  v[at("temper.depth")] = 4.0;
  // Node 4 sits at 3.2 kHz.
  v[at("temper.sens_gain_4")] = 12.0;
  const double keen = ring_cut(v);
  v[at("temper.sens_gain_4")] = -12.0;
  const double deaf = ring_cut(v);
  require(keen > plain + 2.0 && deaf < plain - 1.0,
          "the sensitivity curve steers the cut: " + std::to_string(keen) + " " +
              std::to_string(plain) + " " + std::to_string(deaf));
}

void hard_cuts_harder_than_soft() {
  auto v = defaults();
  const double soft = ring_cut(v);
  v[at("temper.mode")] = 1.0;
  REQUIRE_GT(ring_cut(v), soft + 1.0, "Hard cuts more than Soft");
}

// A 2 kHz tone switched on and off every 300 ms over quiet noise: Split with
// Attacks 0 leaves each onset louder than with Attacks 100.
void split_spares_the_attacks() {
  const std::size_t frames = 144'000;
  const std::size_t period = 28'800;
  auto input = noise(0.02, frames);
  const auto tone = sine(2'000.0, 0.5, frames);
  for (std::size_t i = 0; i < frames; ++i) {
    if (i % period < period / 2) {
      input.left[i] += tone.left[i];
      input.right[i] += tone.right[i];
    }
  }
  const auto onsets_db = [&](double attacks) {
    auto v = defaults();
    v[at("temper.depth")] = 12.0;
    v[at("temper.attack")] = 0.5;
    v[at("temper.attacks")] = attacks;
    const auto out = render(v, input);
    const auto delay = latency(v);
    double total = 0.0;
    for (std::size_t start = period; start + period <= frames; start += period) {
      total += tone_db(out.left, 2'000.0, start + delay + 480, start + delay + 1'440);
    }
    return total;
  };
  const double spared = onsets_db(0.0);
  const double cut = onsets_db(1.0);
  REQUIRE_GT(spared, cut + 2.0,
             "Attacks 0 leaves the onsets louder: " + std::to_string(spared) + " " +
                 std::to_string(cut));
}

// Bass-like noise keyed by a 60 Hz kick tone: the noise gives way around
// 60 Hz only while the key plays, and is left everywhere else.
void unmask_carves_where_the_key_plays() {
  const std::size_t frames = 96'000;
  const auto input = noise(0.2, frames);
  const auto key = sine(60.0, 0.8, frames);
  auto v = at_rest();
  v[at("temper.unmask")] = 1.0;
  v[at("temper.unmask_amount")] = 1;
  const auto out = render(v, input, &key);
  const auto delay = latency(v);
  const std::size_t from = 48'000;
  const double low_cut =
      band_db(input.left, 45.0, 80.0, from - delay) - band_db(out.left, 45.0, 80.0, from);
  const double high_change = std::fabs(band_db(input.left, 1'500.0, 4'000.0, from - delay) -
                                       band_db(out.left, 1'500.0, 4'000.0, from));
  REQUIRE_GT(low_cut, 4.0, "Unmask carves under the key: " + std::to_string(low_cut));
  REQUIRE_LT(high_change, 0.3, "Unmask leaves the rest: " + std::to_string(high_change));
  const Stereo silent{std::vector<double>(frames), std::vector<double>(frames)};
  const auto unkeyed = render(v, input, &silent);
  REQUIRE_LT(delayed_error(unkeyed.left, input.left, delay), 1e-5, "a silent key carves nothing");
  v[at("temper.unmask")] = 0.0;
  const auto off = render(v, input, &key);
  REQUIRE_LT(delayed_error(off.left, input.left, delay), 1e-5, "Unmask off ignores the key");
}

// A 1 kHz bell of -12 dB range: a loud tone above the threshold takes the
// whole range; with the threshold above it, nothing.
void dynamic_bands_follow_their_threshold() {
  const std::size_t frames = 72'000;
  const auto input = sine(1'000.0, 0.5, frames);
  const auto cut_with = [&](double threshold) {
    auto v = at_rest();
    v[at("temper.band_on_1")] = 1.0;
    v[at("temper.band_freq_1")] = 1'000.0;
    v[at("temper.band_q_1")] = 2.0;
    v[at("temper.band_range_1")] = -12.0;
    v[at("temper.band_threshold_1")] = threshold;
    v[at("temper.band_ratio_1")] = 20.0;
    const auto out = render(v, input);
    const auto delay = latency(v);
    return tone_db(input.left, 1'000.0, 24'000 - delay, frames - delay) -
           tone_db(out.left, 1'000.0, 24'000, frames);
  };
  const double full = cut_with(-60.0);
  REQUIRE_LT(std::fabs(full - 12.0), 0.5,
             "above threshold the band takes its range: " + std::to_string(full));
  REQUIRE_LT(std::fabs(cut_with(0.0)), 0.05, "below threshold the band does nothing");
  // Positive range boosts.
  auto v = at_rest();
  v[at("temper.band_on_1")] = 1.0;
  v[at("temper.band_freq_1")] = 1'000.0;
  v[at("temper.band_range_1")] = 6.0;
  v[at("temper.band_threshold_1")] = -60.0;
  v[at("temper.band_ratio_1")] = 20.0;
  const auto out = render(v, input);
  const auto delay = latency(v);
  const double boost = tone_db(out.left, 1'000.0, 24'000, frames) -
                       tone_db(input.left, 1'000.0, 24'000 - delay, frames - delay);
  REQUIRE_LT(std::fabs(boost - 6.0), 0.5, "a positive range boosts: " + std::to_string(boost));
}

void stereo_modes_leave_the_rest_untouched() {
  const std::size_t frames = 48'000;
  auto input = ringing(frames);
  // Side only: left and right opposite.
  Stereo side = input;
  for (std::size_t i = 0; i < frames; ++i) {
    side.right[i] = -side.left[i];
  }
  auto v = defaults();
  v[at("temper.depth")] = 12.0;
  v[at("temper.stereo")] = 1.0; // Mid
  auto out = render(v, side);
  const auto delay = latency(v);
  REQUIRE_LT(delayed_error(out.left, side.left, delay), 1e-5,
             "Mid leaves a pure side signal untouched");
  REQUIRE_LT(delayed_error(out.right, side.right, delay), 1e-5,
             "Mid leaves a pure side signal untouched");
  v[at("temper.stereo")] = 3.0; // Left
  out = render(v, input);
  REQUIRE_LT(delayed_error(out.right, input.right, delay), 1e-5,
             "Left leaves the right channel untouched");
  REQUIRE_GT(delayed_error(out.left, input.left, delay), 1e-2, "and processes the left");
}

void delta_plus_output_is_the_input() {
  const std::size_t frames = 48'000;
  const auto input = ringing(frames);
  for (const double phase : phases) {
    for (const double stereo : {0.0, 1.0}) {
      auto v = defaults();
      v[at("temper.depth")] = 10.0;
      v[at("temper.phase")] = phase;
      v[at("temper.stereo")] = stereo;
      const auto out = render(v, input);
      v[at("temper.delta")] = 1.0;
      const auto removed = render(v, input);
      const auto sum = add(out, removed);
      const auto delay = latency(v);
      require(delayed_error(sum.left, input.left, delay) < 1e-5 &&
                  delayed_error(sum.right, input.right, delay) < 1e-5,
              "Delta plus the output is the input: phase " + std::to_string(phase));
      REQUIRE_GT(peak(removed.left, 24'000), 1e-2, "Delta hears the cut");
    }
  }
}

void mix_blends_toward_the_input() {
  const std::size_t frames = 48'000;
  const auto input = ringing(frames);
  auto v = defaults();
  v[at("temper.depth")] = 10.0;
  v[at("temper.mix")] = 0;
  const auto out = render(v, input);
  REQUIRE_LT(delayed_error(out.left, input.left, latency(v)), 1e-5, "Mix 0 is the input");
  v[at("temper.mix")] = 0.5;
  const double half = ring_cut(v);
  v[at("temper.mix")] = 1;
  const double full = ring_cut(v);
  REQUIRE_GT(half, 0.5, "half Mix cuts less than full");
  REQUIRE_LT(half, full, "half Mix cuts less than full");
}

void telemetry_feeds_the_panel() {
  auto v = defaults();
  v[at("temper.depth")] = 10.0;
  // Band 2 is on but never moves: its level is still reported.
  v[at("temper.band_on_2")] = 1.0;
  v[at("temper.band_threshold_2")] = 0.0;
  auto runtime = make_effect(engine(), v);
  const auto input = ringing(48'000);
  (void)process(*runtime, input);
  std::vector<float> telemetry;
  REQUIRE_TRUE(runtime->read_telemetry(telemetry), "telemetry has Temper's layout");
  REQUIRE_EQ(telemetry.size(), temper::telemetry::size, "telemetry has Temper's layout");
  REQUIRE_LT(std::fabs(telemetry[temper::telemetry::latency_ms] - 16.0F), 0.01F,
             "latency is reported in ms");
  REQUIRE_GT(telemetry[temper::telemetry::max_cut], 3.0F, "the deepest cut is reported");
  // The display band holding 3 kHz: centred at 20 * 1000^((i + 0.5) / n).
  const auto ring_band = static_cast<std::size_t>(std::log(3'000.0 / 20.0) / std::log(1'000.0) *
                                                  temper::display_bands);
  const auto* gain = &telemetry[temper::telemetry::gain];
  const auto deepest =
      static_cast<std::size_t>(std::min_element(gain, gain + temper::display_bands) - gain);
  require(deepest + 1 >= ring_band && deepest <= ring_band + 1,
          "the gain curve dips at the ring: " + std::to_string(deepest));
  const auto* in = &telemetry[temper::telemetry::input_spectrum];
  REQUIRE_GT(in[ring_band], in[ring_band - 12] + 10.0F,
             "the input spectrum shows the ring: " + std::to_string(in[ring_band]) + " " +
                 std::to_string(in[ring_band - 12]));
  REQUIRE_GT(telemetry[temper::telemetry::band_level + 1], -100.0F,
             "a band that is on reports its level");
  REQUIRE_EQ(telemetry[temper::telemetry::keyed], 0.0F, "no key, not keyed");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native temper contract passed",
      {
          {"at_rest_it_is_the_input_delayed", [] { at_rest_it_is_the_input_delayed(); }},
          {"the_latency_is_exact", [] { the_latency_is_exact(); }},
          {"a_resonance_is_cut_and_the_rest_is_left",
           [] { a_resonance_is_cut_and_the_rest_is_left(); }},
          {"max_cut_bounds_the_cut", [] { max_cut_bounds_the_cut(); }},
          {"depth_and_sensitivity_steer_the_cut", [] { depth_and_sensitivity_steer_the_cut(); }},
          {"hard_cuts_harder_than_soft", [] { hard_cuts_harder_than_soft(); }},
          {"split_spares_the_attacks", [] { split_spares_the_attacks(); }},
          {"unmask_carves_where_the_key_plays", [] { unmask_carves_where_the_key_plays(); }},
          {"dynamic_bands_follow_their_threshold", [] { dynamic_bands_follow_their_threshold(); }},
          {"stereo_modes_leave_the_rest_untouched",
           [] { stereo_modes_leave_the_rest_untouched(); }},
          {"delta_plus_output_is_the_input", [] { delta_plus_output_is_the_input(); }},
          {"mix_blends_toward_the_input", [] { mix_blends_toward_the_input(); }},
          {"telemetry_feeds_the_panel", [] { telemetry_feeds_the_panel(); }},
      });
}

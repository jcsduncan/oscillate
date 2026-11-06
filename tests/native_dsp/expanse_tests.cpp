#include "expanse_acoustics.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/expanse.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Expanse (ADR-0040) measured like a room: octave-band RT60 by Schroeder
// integration against the designed low, mid and high decays in every space;
// decay independent of size; the normalised echo density profile; inter-
// channel decorrelation; colouration against ideal decaying noise; freeze;
// pre-delay; a change of space that spills its tail instead of cutting it;
// shimmer's interval and stability; goldens.
namespace {
using namespace expanse_acoustics;
namespace expanse = oscillate::native_dsp::effects::expanse;

const effects::EffectEngine& expanse_engine() { return expanse::engine(); }
const EngineDescriptor& table() { return *expanse_engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }
constexpr double rate = oscillate::test::sample_rate_48k_hz;
constexpr std::size_t second = 48'000;

const std::array<std::string_view, 6> spaces{"Room",    "Hall",      "Plate",
                                             "Chamber", "Cathedral", "Cloud"};

// The late field alone, fully wet, no modulation of the measurement by EQ.
std::vector<double> late(int mode, double decay) {
  auto values = preset(table(), "Init");
  values[at("expanse.mode")] = mode;
  values[at("expanse.decay")] = decay;
  values[at("expanse.early")] = 0.0;
  values[at("expanse.mix")] = 1.0;
  values[at("expanse.mod_depth")] = 0.35;
  return values;
}

Stereo impulse_response(const std::vector<double>& values, std::size_t frames) {
  auto runtime = make_effect(expanse_engine(), values);
  Stereo in{std::vector<double>(frames), std::vector<double>(frames)};
  in.left[0] = 1.0;
  in.right[0] = 1.0;
  return process(*runtime, in);
}

std::string name(int mode) { return std::string(spaces[static_cast<std::size_t>(mode)]); }

void bands_decay_at_their_rt60() {
  for (int mode = 0; mode < 6; ++mode) {
    auto values = late(mode, 2.0);
    values[at("expanse.low_decay")] = 1.5;
    values[at("expanse.low_freq")] = 250.0;
    values[at("expanse.high_decay")] = 0.5;
    values[at("expanse.high_freq")] = 4'000.0;
    const auto ir = impulse_response(values, 6 * second);
    const std::array<std::array<double, 2>, 3> bands{
        {{63.0, 3.0}, {1'000.0, 2.0}, {12'000.0, 1.0}}};
    for (const auto& [hz, expected] : bands) {
      const double measured = rt60(octave_band(ir.left, hz));
      REQUIRE_LT(std::fabs(measured / expected - 1.0), 0.1,
                 name(mode) + " decays at " + std::to_string(hz) + " Hz in " +
                     std::to_string(measured) + " s, designed " + std::to_string(expected));
    }
  }
}

void decay_follows_the_control_at_any_size() {
  for (const double decay : {0.4, 4.0, 12.0}) {
    for (const double size : {0.1, 0.6, 1.0}) {
      auto values = late(1, decay); // Hall
      values[at("expanse.size")] = size;
      const auto ir = impulse_response(
          values, static_cast<std::size_t>((decay * 1.2 + 0.5) * static_cast<double>(second)));
      const double measured = rt60(octave_band(ir.left, 1'000.0));
      REQUIRE_LT(std::fabs(measured / decay - 1.0), 0.1,
                 "a " + std::to_string(decay) + " s hall at size " + std::to_string(size) +
                     " decays in " + std::to_string(measured) + " s");
    }
  }
}

void echo_density_builds_fast() {
  // Milliseconds to a fully diffuse (normalised echo density 0.9) field.
  const std::array<double, 6> limit_ms{40, 50, 30, 40, 100, 250};
  const std::array<double, 6> decay{1.0, 2.5, 2.5, 1.5, 6.0, 8.0};
  std::array<double, 6> reached{};
  for (int mode = 0; mode < 6; ++mode) {
    const auto m = static_cast<std::size_t>(mode);
    const auto ir = impulse_response(late(mode, decay[m]), second);
    const auto ned = echo_density(ir.left);
    std::size_t window = 0;
    while (window < ned.size() && ned[window] < 0.9) {
      ++window;
    }
    reached[m] = static_cast<double>(window) * 10.0;
    REQUIRE_LE(reached[m], limit_ms[m],
               name(mode) + " is diffuse after " + std::to_string(reached[m]) + " ms");
    // And stays diffuse.
    for (std::size_t w = window; w < std::min<std::size_t>(ned.size(), window + 40); ++w) {
      REQUIRE_GT(ned[w], 0.8, name(mode) + " stays diffuse");
    }
  }
  REQUIRE_GT(reached[5], reached[1], "a cloud blooms more slowly than a hall");
}

void channels_are_decorrelated() {
  for (int mode = 0; mode < 6; ++mode) {
    const auto ir = impulse_response(late(mode, 2.0), 2 * second);
    const double rho = correlation(ir.left, ir.right, second / 5, 2 * second);
    REQUIRE_LT(std::fabs(rho), 0.05, name(mode) + " decorrelates: " + std::to_string(rho));
  }
  auto values = late(1, 2.0);
  values[at("expanse.width")] = 0.0;
  const auto mono = impulse_response(values, second);
  REQUIRE_GT(correlation(mono.left, mono.right, second / 5, second), 0.999, "width 0 is mono");
}

void the_tail_is_uncoloured() {
  // Ideal exponentially decaying white noise scores about 4 dB on this
  // measure; a ringing mode would stand far out.
  const std::array<double, 6> decay{1.0, 2.5, 2.5, 1.5, 6.0, 8.0};
  for (int mode = 0; mode < 6; ++mode) {
    const auto m = static_cast<std::size_t>(mode);
    const auto ir = impulse_response(late(mode, decay[m]), 9 * second);
    const auto spectrum = welch_db(
        ir.left, second / 5,
        second / 5 + static_cast<std::size_t>(decay[m] * 0.8 * static_cast<double>(second)));
    const double colour = colouration_db(spectrum, 500.0, 10'000.0);
    REQUIRE_LT(colour, 7.0, name(mode) + " colours its tail by " + std::to_string(colour) + " dB");
  }
}

void freeze_holds_and_closes_the_input() {
  auto values = late(1, 2.5);
  auto runtime = make_effect(expanse_engine(), values);
  (void)process(*runtime, noise(0.3, second / 2));
  auto frozen = values;
  frozen[at("expanse.freeze")] = 1.0;
  require(runtime->offer_values(frozen, 1), "freeze offered");
  const Stereo quiet{std::vector<double>(second), std::vector<double>(second)};
  const auto first = process(*runtime, quiet, {}, second / 2);
  const double held = rms(first.left, second / 2, second);
  // Ten seconds on, with loud input arriving in the meantime.
  std::uint64_t position = 3 * second / 2;
  for (int i = 0; i < 9; ++i) {
    (void)process(*runtime, i == 4 ? noise(0.5, second) : quiet, {}, position);
    position += second;
  }
  const auto later = process(*runtime, quiet, {}, position);
  const double still = rms(later.left, 0, second);
  REQUIRE_LT(std::fabs(db(still / held)), 1.0,
             "a frozen tail holds and takes no input: " + std::to_string(db(still / held)) + " dB");
  require(runtime->offer_values(values, 2), "thaw offered");
  position += second;
  (void)process(*runtime, quiet, {}, position);
  position += second;
  const auto thawed = process(*runtime, quiet, {}, position);
  REQUIRE_LT(db(rms(thawed.left, 0, second) / still), -20.0, "a thawed tail decays again");
}

void predelay_delays_the_wet() {
  auto values = preset(table(), "Init");
  values[at("expanse.predelay")] = 100.0;
  values[at("expanse.mix")] = 1.0;
  const auto ir = impulse_response(values, second / 2);
  REQUIRE_LT(peak(ir.left, 0, 4'800), 1e-9, "nothing before the pre-delay");
  REQUIRE_LT(peak(ir.right, 0, 4'800), 1e-9, "nothing before the pre-delay");
  REQUIRE_GT(peak(ir.left, 4'800, 4'800 + 2'400), 1e-3, "and the space starts right after it");
}

void a_change_of_space_spills_its_tail() {
  auto input = noise(0.3, 4 * second);
  std::fill(input.left.begin() + 2 * second, input.left.end(), 0.0);
  std::fill(input.right.begin() + 2 * second, input.right.end(), 0.0);
  const auto values = late(4, 6.0); // Cathedral
  auto steady = make_effect(expanse_engine(), values);
  const auto reference = process(*steady, input);
  auto switching = make_effect(expanse_engine(), values);
  auto next = values;
  next[at("expanse.mode")] = 0; // Room
  const auto out =
      process(*switching, input, {.at = {2 * second}, .before = [&](std::uint64_t) {
                                    require(switching->offer_values(next, 1), "switch offered");
                                  }});
  for (const std::size_t from : {2 * second + 2'400, 2 * second + 24'000, 3 * second}) {
    const double kept =
        db(rms(out.left, from, from + 4'800) / rms(reference.left, from, from + 4'800));
    REQUIRE_LT(std::fabs(kept), 1.5,
               "the old space rings on after the switch: " + std::to_string(kept) + " dB");
  }
  double steady_step = 0.0;
  double switch_step = 0.0;
  for (std::size_t i = 2 * second - 4'800; i < 2 * second; ++i) {
    steady_step = std::max(steady_step, std::fabs(out.left[i] - out.left[i - 1]));
  }
  for (std::size_t i = 2 * second; i < 2 * second + 4'800; ++i) {
    switch_step = std::max(switch_step, std::fabs(out.left[i] - out.left[i - 1]));
  }
  REQUIRE_LT(switch_step, steady_step * 1.5, "and the switch does not click");
}

double tone_db(const std::vector<double>& x, std::size_t from, double hz) {
  std::vector<std::complex<double>> window(32'768);
  for (std::size_t i = 0; i < window.size(); ++i) {
    const double hann = oscillate::native_dsp::dsp::hann(static_cast<double>(i),
                                                         static_cast<double>(window.size()));
    window[i] = x[from + i] * hann;
  }
  fft(window);
  const auto bin = static_cast<std::size_t>(std::lround(hz * 32'768.0 / rate));
  double power = 0.0;
  for (std::size_t k = bin - 3; k <= bin + 3; ++k) {
    power += std::norm(window[k]);
  }
  return 10.0 * std::log10(power + 1e-300);
}

void shimmer_adds_its_interval_and_stays_bounded() {
  auto input = sine(440.0, 0.3, 6 * second);
  std::fill(input.left.begin() + second, input.left.end(), 0.0);
  std::fill(input.right.begin() + second, input.right.end(), 0.0);
  for (const double amount : {0.0, 0.6}) {
    auto values = late(1, 5.0);
    values[at("expanse.shimmer")] = amount;
    values[at("expanse.mod_depth")] = 0.0;
    auto runtime = make_effect(expanse_engine(), values);
    const auto out = process(*runtime, input);
    const double fundamental = tone_db(out.left, 3 * second, 440.0);
    const double octave = tone_db(out.left, 3 * second, 880.0);
    if (amount == 0.0) {
      REQUIRE_LT(octave, fundamental - 40.0, "no shimmer, no octave");
    } else {
      REQUIRE_GT(octave, fundamental - 15.0,
                 "shimmer builds the octave: " + std::to_string(octave - fundamental) + " dB");
    }
  }
  auto wild = late(4, 30.0);
  wild[at("expanse.shimmer")] = 1.0;
  auto runtime = make_effect(expanse_engine(), wild);
  const auto out = process(*runtime, noise(0.5, 20 * second));
  require(peak(out.left) < 4.0 && peak(out.right) < 4.0,
          "full shimmer on a 30 s cathedral stays bounded: " + std::to_string(peak(out.left)));
}

struct Golden {
  std::string_view name;
  double level_db;
  double centroid_hz;
};

void presets_match_golden_fingerprints() {
  const std::array<Golden, 13> golden{{
      {"Concert Hall", -48.195, 5031.21},
      {"Small Room", -64.9656, 4894.38},
      {"Drum Room", -62.7759, 6729.87},
      {"Vocal Plate", -51.4704, 7018.61},
      {"Bright Plate", -46.2734, 8919.66},
      {"Stone Chamber", -54.0684, 6105.42},
      {"Cathedral", -38.3944, 5171.08},
      {"Dark Hall", -43.7292, 1774.77},
      {"Shimmer Heaven", -37.6029, 6363.08},
      {"Fifth Shimmer", -36.4226, 5336.71},
      {"Endless Cloud", -32.652, 5946.67},
      {"Ambient Wash", -35.2093, 5439.79},
      {"Init", -44.165, 9280.68},
  }};
  REQUIRE_EQ(golden.size(), table().presets.size(), "every preset has a golden");
  auto input = noise(0.05, 2 * second);
  for (std::size_t i = 0; i < second; ++i) {
    const double env = (i % 24'000) < 4'800 ? 1.0 : 0.0;
    for (const double hz : {196.0, 293.66, 392.0}) {
      const double x =
          0.15 * env * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
      input.left[i] += x;
      input.right[i] += x;
    }
  }
  std::fill(input.left.begin() + second, input.left.end(), 0.0);
  std::fill(input.right.begin() + second, input.right.end(), 0.0);
  bool ok = true;
  for (const auto& entry : golden) {
    auto runtime = make_effect(expanse_engine(), preset(table(), entry.name));
    const auto out = process(*runtime, input);
    const double level = db(rms(out.left, second, 2 * second));
    const double centroid = centroid_hz(out.left, second + 4'800);
    if (std::fabs(level - entry.level_db) > 0.05 ||
        std::fabs(centroid - entry.centroid_hz) > entry.centroid_hz * 0.01 + 1.0) {
      std::cerr << "{\"" << entry.name << "\", " << level << ", " << centroid << "},\n";
      ok = false;
    }
  }
  require(ok, "presets match their golden fingerprints");
}

// A reset network restarts its wander: after a seek the kernel plays
// exactly as a new one, so a render from there equals an export from there.
// Settings changed across the reset are jumped to, not glided from the old.
void a_reset_plays_as_new() {
  constexpr double seconds = 0.5;
  constexpr double mod_depth = 1.0; // The wander at its widest.
  constexpr double changed_mix = 0.4;
  constexpr double changed_width = 0.2;
  auto values = preset(table(), "Concert Hall");
  values[at("expanse.mod_depth")] = mod_depth;
  values[at("expanse.mix")] = 1.0;
  auto changed = values;
  changed[at("expanse.mix")] = changed_mix;
  changed[at("expanse.width")] = changed_width;
  const auto frames = static_cast<std::size_t>(seconds * rate);
  constexpr double level = 0.3;
  const auto input = noise(level, frames);
  const auto run = [&](effects::EffectKernel& kernel, const std::vector<double>& settings) {
    std::vector<double> out;
    std::vector<engine::StereoFrame> tick(oscillate::native_dsp::synth::control_interval_frames);
    for (std::size_t at_frame = 0; at_frame < frames; at_frame += tick.size()) {
      const std::size_t n = std::min(tick.size(), frames - at_frame);
      kernel.control(settings, static_cast<std::uint32_t>(n), {});
      for (std::size_t i = 0; i < n; ++i) {
        tick[i] = {static_cast<float>(input.left[at_frame + i]),
                   static_cast<float>(input.right[at_frame + i])};
      }
      kernel.process(std::span(tick.data(), n), {});
      for (std::size_t i = 0; i < n; ++i) {
        out.push_back(tick[i].left);
      }
    }
    return out;
  };
  auto used = expanse_engine().create_kernel(values, rate);
  (void)run(*used, values);
  used->reset();
  auto fresh = expanse_engine().create_kernel(changed, rate);
  REQUIRE_EQ(run(*used, changed), run(*fresh, changed), "a reset kernel renders as a new one");
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "native expanse contract passed",
      {
          {"bands_decay_at_their_rt60", [] { bands_decay_at_their_rt60(); }},
          {"decay_follows_the_control_at_any_size",
           [] { decay_follows_the_control_at_any_size(); }},
          {"echo_density_builds_fast", [] { echo_density_builds_fast(); }},
          {"channels_are_decorrelated", [] { channels_are_decorrelated(); }},
          {"the_tail_is_uncoloured", [] { the_tail_is_uncoloured(); }},
          {"freeze_holds_and_closes_the_input", [] { freeze_holds_and_closes_the_input(); }},
          {"predelay_delays_the_wet", [] { predelay_delays_the_wet(); }},
          {"a_change_of_space_spills_its_tail", [] { a_change_of_space_spills_its_tail(); }},
          {"shimmer_adds_its_interval_and_stays_bounded",
           [] { shimmer_adds_its_interval_and_stays_bounded(); }},
          {"presets_match_golden_fingerprints", [] { presets_match_golden_fingerprints(); }},
          {"a_reset_plays_as_new", [] { a_reset_plays_as_new(); }},
      });
}

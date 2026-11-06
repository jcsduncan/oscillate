#include "oscillate/native_dsp/synth/dx_envelope.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/signal.hpp"
#include "oscillate/test/tolerances.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
using namespace oscillate::native_dsp::synth;

using oscillate::test::require;

double db(double amplitude) { return oscillate::test::gain_to_db(amplitude); }

DxEnvelopeSettings settings(std::array<double, 4> rates, std::array<double, 4> levels,
                            int scaling = 0) {
  DxEnvelopeSettings s;
  s.rates = rates;
  s.levels = levels;
  s.rate_scaling = scaling;
  return s;
}

// Frames until the envelope first reaches `stage`, or `limit`.
std::size_t frames_to_stage(DxEnvelope& env, int stage, std::size_t limit) {
  for (std::size_t i = 0; i < limit; ++i) {
    (void)env.next();
    if (env.stage() >= stage) {
      return i + 1;
    }
  }
  return limit;
}

// The chip's timing: rate 50 sweeps the full 4032-unit range in 64,512
// reference frames (1.46 s), rate 99 in 288 (6.5 ms); levels are 0.75 dB a
// step, with level 99 at full scale.
void levels_and_rates_follow_the_chip() {
  REQUIRE_EQ(DxEnvelope::level_units(99), 4032.0, "level 99 is full scale");
  REQUIRE_EQ(DxEnvelope::amplitude(DxEnvelope::level_units(99)), 1.0, "full scale is unity");
  const double step = db(DxEnvelope::amplitude(DxEnvelope::level_units(97))) -
                      db(DxEnvelope::amplitude(DxEnvelope::level_units(99)));
  REQUIRE_LT(std::fabs(step + 1.505), 0.01, "two level steps are 1.5 dB");
  REQUIRE_EQ(DxEnvelope::amplitude(DxEnvelope::level_units(0)), 0.0, "level 0 is silent");
  REQUIRE_EQ(DxEnvelope::increment(50, 0, 60), 4096.0 / 65'536.0, "rate 50 increment");
  REQUIRE_EQ(DxEnvelope::increment(99, 0, 60), 7.0 * 131'072.0 / 65'536.0, "rate 99 increment");
  const double full = DxEnvelope::full_units - DxEnvelope::floor_units;
  REQUIRE_LT(std::fabs(DxEnvelope::segment_seconds(4032.0, 208.0, 50, 0, 60) -
                       full / (4096.0 / 65'536.0) / 44'100.0),
             tolerance::double_rounding, "falling time is distance over speed");
}

// A falling segment is a straight line in decibels, and takes the same time
// at every sample rate.
void decays_are_linear_in_decibels_at_any_rate() {
  for (const double rate : {44'100.0, 48'000.0, 96'000.0}) {
    DxEnvelope env;
    env.prepare(rate);
    env.reset();
    env.set(settings({99, 50, 99, 99}, {99, 0, 0, 0}), 60);
    env.gate_on();
    (void)frames_to_stage(env, 1, 100'000);
    const auto frames = frames_to_stage(env, 2, 1'000'000);
    const double expected = DxEnvelope::segment_seconds(4032.0, 208.0, 50, 0, 60) * rate;
    REQUIRE_LE(std::fabs(static_cast<double>(frames) - expected), 2.0,
               "rate 50 decay takes its time at " + std::to_string(rate));

    env.reset();
    env.gate_on();
    (void)frames_to_stage(env, 1, 100'000);
    const std::size_t a = static_cast<std::size_t>(0.1 * rate);
    std::vector<double> levels;
    for (std::size_t i = 0; i < 3 * a; ++i) {
      const double value = env.next();
      if (i % a == 0) {
        levels.push_back(db(value));
      }
    }
    REQUIRE_LT(std::fabs((levels[0] - levels[1]) - (levels[1] - levels[2])),
               tolerance::float_round_trip, "equal dB per equal time");
  }
}

// A rising segment jumps to about -50 dB, then climbs faster the quieter it
// is: each band of 256 units takes less time than the one above it.
void attacks_jump_then_accelerate() {
  DxEnvelope env;
  env.prepare(44'100.0);
  env.reset();
  env.set(settings({30, 99, 99, 99}, {99, 99, 99, 0}), 60);
  env.gate_on();
  const double first = env.next();
  REQUIRE_LT(std::fabs(db(first) + 49.7), 0.5, "the attack starts near -50 dB");
  std::vector<std::size_t> band_frames;
  double band = std::floor((env.units() - 192.0) / 256.0);
  std::size_t count = 0;
  while (env.stage() == 0) {
    (void)env.next();
    ++count;
    const double now = std::floor((env.units() - 192.0) / 256.0);
    if (now != band) {
      band_frames.push_back(count);
      count = 0;
      band = now;
    }
  }
  REQUIRE_GE(band_frames.size(), 6, "the attack crosses several bands");
  // The first band is entered part-way, at the jump.
  for (std::size_t i = 2; i < band_frames.size(); ++i) {
    REQUIRE_GT(band_frames[i], band_frames[i - 1], "louder bands climb more slowly");
  }
  REQUIRE_EQ(env.value(), 1.0, "the attack lands on L1");
}

// L3 holds while the key is down; release falls from wherever the envelope
// is to L4; a silent L4 ends it, a sounding one holds.
void sustain_release_and_end_level() {
  DxEnvelope env;
  env.prepare(48'000.0);
  env.reset();
  env.set(settings({99, 60, 60, 70}, {99, 80, 70, 0}), 60);
  env.gate_on();
  (void)frames_to_stage(env, 3, 1'000'000);
  const double sustain = env.value();
  REQUIRE_LT(std::fabs(sustain - DxEnvelope::amplitude(DxEnvelope::level_units(70))),
             tolerance::double_accumulation, "L3 is reached");
  for (int i = 0; i < 48'000; ++i) {
    REQUIRE_EQ(env.next(), sustain, "L3 holds while the key is down");
  }
  env.gate_off();
  (void)frames_to_stage(env, 4, 10'000'000);
  REQUIRE_TRUE(!env.active(), "a silent L4 ends the envelope");
  REQUIRE_EQ(env.value(), 0.0, "a silent L4 ends the envelope");

  env.reset();
  env.set(settings({99, 99, 99, 50}, {99, 99, 99, 60}), 60);
  env.gate_on();
  REQUIRE_EQ(env.value(), DxEnvelope::amplitude(DxEnvelope::level_units(60)),
             "a new note starts from L4");
  (void)frames_to_stage(env, 3, 100'000);
  env.gate_off();
  (void)frames_to_stage(env, 4, 10'000'000);
  REQUIRE_TRUE(env.active(), "a sounding L4 holds after release");
  REQUIRE_LT(std::fabs(env.value() - DxEnvelope::amplitude(DxEnvelope::level_units(60))),
             tolerance::double_accumulation, "a sounding L4 holds after release");
}

// Rate scaling speeds segments up toward the top of the keyboard, and does
// nothing at sensitivity 0.
void rate_scaling_speeds_up_high_keys() {
  const double low = DxEnvelope::segment_seconds(4032.0, 208.0, 50, 7, 36);
  const double high = DxEnvelope::segment_seconds(4032.0, 208.0, 50, 7, 96);
  REQUIRE_LT(high, 0.5 * low, "high keys decay much faster with full rate scaling");
  REQUIRE_EQ(DxEnvelope::segment_seconds(4032.0, 208.0, 50, 0, 36),
             DxEnvelope::segment_seconds(4032.0, 208.0, 50, 0, 96),
             "no rate scaling, no key dependence");
}

// Whole numbers are the chip's exact values; fractions glide between them,
// speeds geometrically and levels in units.
void fractions_glide_between_chip_values() {
  for (int rate = 0; rate < 99; ++rate) {
    const double low = DxEnvelope::increment(rate, 0, 60);
    const double high = DxEnvelope::increment(rate + 1, 0, 60);
    REQUIRE_EQ(DxEnvelope::increment(static_cast<double>(rate), 0, 60), low, "whole rate is exact");
    const double half = DxEnvelope::increment(rate + 0.5, 0, 60);
    REQUIRE_LT(std::fabs(half - std::sqrt(low * high)), tolerance::double_rounding * high,
               "speeds glide geometrically");
  }
  for (int level = 0; level < 99; ++level) {
    const double low = DxEnvelope::level_units(level);
    const double high = DxEnvelope::level_units(level + 1);
    REQUIRE_EQ(DxEnvelope::level_units(static_cast<double>(level)), low, "whole level is exact");
    REQUIRE_EQ(DxEnvelope::level_units(level + 0.25), low + 0.25 * (high - low),
               "levels glide in units");
  }
}

// A live edit re-aims the running segment from where it is: no step.
void live_edits_are_smooth() {
  DxEnvelope env;
  env.prepare(48'000.0);
  env.reset();
  env.set(settings({99, 40, 40, 40}, {99, 50, 30, 0}), 60);
  env.gate_on();
  (void)frames_to_stage(env, 1, 100'000);
  for (int i = 0; i < 4'800; ++i) {
    (void)env.next();
  }
  const double before = env.value();
  env.set(settings({99, 80, 40, 40}, {99, 20, 30, 0}), 60);
  const double after = env.next();
  REQUIRE_LT(std::fabs(db(after) - db(before)), 0.1, "a rate change does not step the level");
  (void)frames_to_stage(env, 3, 10'000'000);
  const double held = env.value();
  env.set(settings({99, 80, 40, 40}, {99, 20, 60, 0}), 60);
  (void)frames_to_stage(env, 3, 10'000'000);
  REQUIRE_GT(env.value(), held, "a raised L3 is followed while the key is held");
}

} // namespace

int main() {
  try {
    levels_and_rates_follow_the_chip();
    decays_are_linear_in_decibels_at_any_rate();
    attacks_jump_then_accelerate();
    sustain_release_and_end_level();
    rate_scaling_speeds_up_high_keys();
    fractions_glide_between_chip_values();
    live_edits_are_smooth();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "DX envelope contract passed\n";
  return 0;
}

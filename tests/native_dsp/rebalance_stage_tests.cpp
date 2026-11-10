// The master rebalance stage (ADR-0123): exact while neutral, Shape's EQ
// and an exact gain when set, gliding edits without steps or allocation.

#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/rebalance.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace {
namespace dsp = oscillate::native_dsp;
namespace engine = oscillate::engine;
namespace tolerance = oscillate::test::tolerance;
using oscillate::test::take;

constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
constexpr std::size_t block = 128;
constexpr std::size_t blocks = 64;
constexpr double tone_hz = 220.0;
constexpr double second_hz = 3'100.0;
constexpr float level = 0.25F;
constexpr double boost_db = 6.0;
constexpr double low_db = 4.0;
constexpr double mid_db = -3.0;
constexpr double high_db = 2.5;
constexpr double low_hz = 120.0;
constexpr double mid_hz = 900.0;
constexpr double mid_q = 0.9;
constexpr double high_hz = 7'000.0;
constexpr double decibels_per_decade = 20.0;
// One frame's change while gliding stays under this: the tone itself moves
// at most 2 pi f / rate * level (0.0072) per frame, and the settings raise
// it at most fourfold; a 6 dB step in the gain would jump by up to `level`.
constexpr double glide_step_limit = 0.05;

const dsp::RoleTone neutral_tone{0.0, 0.0, low_hz, 0.0, mid_hz, mid_q, 0.0, high_hz};
const dsp::RoleTone tone{boost_db, low_db, low_hz, mid_db, mid_hz, mid_q, high_db, high_hz};

// The low tone on the left, `right_hz` on the right.
std::vector<engine::StereoFrame> signal(std::size_t frames, double right_hz = second_hz) {
  std::vector<engine::StereoFrame> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const auto left = static_cast<float>(level * std::sin(2.0 * std::numbers::pi * tone_hz * t));
    const auto right = static_cast<float>(level * std::sin(2.0 * std::numbers::pi * right_hz * t));
    out[i] = {left, right};
  }
  return out;
}

void run(engine::ProcessorRuntime& stage, std::span<engine::StereoFrame> audio) {
  for (std::size_t first = 0; first < audio.size(); first += block) {
    const auto count = std::min(block, audio.size() - first);
    stage.process(audio.subspan(first, count), engine::BlockContext{first, 1});
  }
}

void neutral_is_exact() {
  auto stage = dsp::RoleStage(take(dsp::role_target(neutral_tone, rate)));
  const auto input = signal(block * blocks);
  auto output = input;
  run(stage, output);
  REQUIRE_TRUE(stage.bypassed(), "a neutral stage does nothing");
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_EQ(output[i].left, input[i].left, "neutral is bit exact");
    REQUIRE_EQ(output[i].right, input[i].right, "neutral is bit exact");
  }
}

// The stage equals Shape's EQ cascade with the same bands, then the gain.
void matches_shape_eq_and_gain() {
  auto stage = dsp::RoleStage(take(dsp::role_target(tone, rate)));
  const auto bands = dsp::rebalance_bands_for(tone, rate);
  dsp::EqRuntime reference(take(dsp::prepare_eq(bands, rate)));
  const auto gain = static_cast<float>(std::pow(10.0, boost_db / decibels_per_decade));
  const auto input = signal(block * blocks);
  auto output = input;
  auto expected = input;
  run(stage, output);
  run(reference, expected);
  for (auto& frame : expected)
    frame = {frame.left * gain, frame.right * gain};
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_EQ(output[i].left, expected[i].left, "the stage is Shape's EQ then the gain");
    REQUIRE_EQ(output[i].right, expected[i].right, "the stage is Shape's EQ then the gain");
  }
}

void gain_only_is_exact() {
  const dsp::RoleTone gain_only{boost_db, 0.0, low_hz, 0.0, mid_hz, mid_q, 0.0, high_hz};
  auto stage = dsp::RoleStage(take(dsp::role_target(gain_only, rate)));
  const auto gain = static_cast<float>(std::pow(10.0, boost_db / decibels_per_decade));
  const auto input = signal(block * blocks);
  auto output = input;
  run(stage, output);
  for (std::size_t i = 0; i < input.size(); ++i)
    REQUIRE_EQ(output[i].left, input[i].left * gain, "a flat EQ does not run");
}

// Edits glide: no step between frames however far the settings move, and
// a return to neutral ends exact again. The callback never allocates.
void edits_glide_without_allocating() {
  auto stage = dsp::RoleStage(take(dsp::role_target(neutral_tone, rate)));
  const auto set = take(dsp::role_target(tone, rate));
  const auto back = take(dsp::role_target(neutral_tone, rate));
  const auto input = signal(block * blocks * 3, tone_hz);
  auto output = input;
  const auto third = output.size() / 3;
  auto audio = std::span(output);
  oscillate::test::counted_allocations = 0;
  oscillate::test::counting_allocations = true;
  run(stage, audio.first(third));
  stage.offer(set);
  run(stage, audio.subspan(third, third));
  stage.offer(back);
  run(stage, audio.subspan(2 * third));
  oscillate::test::counting_allocations = false;
  REQUIRE_EQ(oscillate::test::counted_allocations, 0U, "the stage allocated on the callback");
  for (std::size_t i = 1; i < output.size(); ++i) {
    REQUIRE_LT(std::fabs(output[i].left - output[i - 1].left), glide_step_limit, "no step");
    REQUIRE_LT(std::fabs(output[i].right - output[i - 1].right), glide_step_limit, "no step");
  }
  REQUIRE_TRUE(stage.bypassed(), "back at neutral the stage bypasses");
  for (std::size_t i = output.size() - block; i < output.size(); ++i)
    REQUIRE_EQ(output[i].left, input[i].left, "back at neutral it is exact");
}

// The highest settings at the lowest project rate still prepare.
void bands_stay_in_range() {
  constexpr std::uint32_t low_rate = 44'100;
  constexpr double highest_shelf_hz = 16'000.0;
  dsp::RoleTone high = tone;
  high.high_hz = highest_shelf_hz;
  const auto bands = dsp::rebalance_bands_for(high, low_rate);
  REQUIRE_EQ(bands[2].frequency_hz, highest_shelf_hz, "an in-range shelf is kept");
  REQUIRE_OK(dsp::role_target(high, low_rate));
}

} // namespace

int main(int argc, char** argv) {
  return oscillate::test::run_cases(
      argc, argv, "rebalance stage contract passed",
      {{"neutral_is_exact", neutral_is_exact},
       {"matches_shape_eq_and_gain", matches_shape_eq_and_gain},
       {"gain_only_is_exact", gain_only_is_exact},
       {"edits_glide_without_allocating", edits_glide_without_allocating},
       {"bands_stay_in_range", bands_stay_in_range}});
}

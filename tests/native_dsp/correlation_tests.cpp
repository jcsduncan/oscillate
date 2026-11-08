#include "oscillate/native_dsp/correlation.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
namespace dsp = oscillate::native_dsp;
namespace engine = oscillate::engine;

constexpr double rate = oscillate::test::sample_rate_48k_hz;

using oscillate::test::require;

// One second of a 997 Hz sine on the left; the right is `phase` radians
// later and `gain` times as loud.
std::vector<engine::StereoFrame> sine_pair(double phase, float gain) {
  std::vector<engine::StereoFrame> frames(static_cast<std::size_t>(rate));
  for (std::size_t n = 0; n < frames.size(); ++n) {
    const auto t = 2.0 * std::numbers::pi * 997.0 * static_cast<double>(n) / rate;
    frames[n] = {0.5F * static_cast<float>(std::sin(t)),
                 0.5F * gain * static_cast<float>(std::sin(t + phase))};
  }
  return frames;
}

dsp::StereoCorrelationMeter measured(std::span<const engine::StereoFrame> frames) {
  dsp::StereoCorrelationMeter meter;
  meter.prepare(rate);
  // Block by block, as the master worker feeds it.
  for (std::size_t offset = 0; offset < frames.size(); offset += 128)
    meter.process(frames.subspan(offset, std::min<std::size_t>(128, frames.size() - offset)));
  return meter;
}

bool near(std::optional<double> value, double expected, double tolerance) {
  return value && std::abs(*value - expected) <= tolerance;
}

void phase_oracles() {
  // The closed forms: correlation is cos(phase); the mono sum keeps
  // (1 + cos(phase)) / 2 of the power for equal levels.
  for (const double phase : {0.0, std::numbers::pi / 3.0, std::numbers::pi / 2.0,
                             2.0 * std::numbers::pi / 3.0, std::numbers::pi}) {
    const auto frames = sine_pair(phase, 1.0F);
    const auto meter = measured(frames);
    require(near(meter.correlation(), std::cos(phase), 0.01),
            "correlation is not cos(phase) at " + std::to_string(phase));
    const auto expected_loss = 10.0 * std::log10(std::max((1.0 + std::cos(phase)) / 2.0, 1e-12));
    if (phase < std::numbers::pi)
      require(near(meter.mono_loss_db(), expected_loss, 0.1),
              "mono loss is not (1 + cos) / 2 at " + std::to_string(phase));
    else
      require(meter.mono_loss_db() && *meter.mono_loss_db() < -60.0,
              "an out-of-phase pair did not cancel in mono");
  }
}

void level_does_not_change_phase() {
  // A quieter right channel is still in phase.
  const auto meter = measured(sine_pair(0.0, 0.25F));
  require(near(meter.correlation(), 1.0, 1e-6), "a level difference read as a phase difference");
}

void unrelated_noise_reads_near_zero() {
  std::vector<engine::StereoFrame> frames(static_cast<std::size_t>(rate));
  std::uint32_t left = 1;
  std::uint32_t right = 0x9E37'79B9;
  // Two different generators (Numerical Recipes and Borland), so the
  // channels are not one sequence offset in time.
  const auto unit = [](std::uint32_t state) {
    return static_cast<float>(state >> 8) / static_cast<float>(1U << 24) - 0.5F;
  };
  for (auto& frame : frames) {
    left = left * 1'664'525U + 1'013'904'223U;
    right = right * 22'695'477U + 1U;
    frame = {unit(left), unit(right)};
  }
  const auto meter = measured(frames);
  require(near(meter.correlation(), 0.0, 0.1), "unrelated noise read as correlated");
  require(near(meter.mono_loss_db(), -3.01, 0.5), "unrelated noise did not lose 3 dB in mono");
}

void silence_has_no_reading_and_one_side_is_unrelated() {
  const std::vector<engine::StereoFrame> silence(4'800);
  const auto quiet = measured(silence);
  REQUIRE_TRUE(!quiet.correlation(), "silence reported a phase");
  REQUIRE_TRUE(!quiet.mono_loss_db(), "silence reported a phase");
  auto left_only = sine_pair(0.0, 0.0F);
  const auto one_side = measured(left_only);
  require(near(one_side.correlation(), 0.0, 1e-9), "one silent side was not unrelated");
}

void non_finite_samples_are_skipped_without_allocating() {
  auto frames = sine_pair(0.0, 1.0F);
  frames[100] = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
  frames[200] = {0.0F, std::numeric_limits<float>::infinity()};
  dsp::StereoCorrelationMeter meter;
  meter.prepare(rate);
  counted_allocations = 0;
  counting_allocations = true;
  meter.process(frames);
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "the meter allocated while processing");
  require(near(meter.correlation(), 1.0, 1e-3), "a non-finite sample corrupted the reading");
  meter.reset();
  require(!meter.correlation(), "reset kept an old reading");
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught exception is the test's failure report.
int main() {
  phase_oracles();
  level_does_not_change_phase();
  unrelated_noise_reads_near_zero();
  silence_has_no_reading_and_one_side_is_unrelated();
  non_finite_samples_are_skipped_without_allocating();
}

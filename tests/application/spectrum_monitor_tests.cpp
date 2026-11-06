#include "oscillate/application/spectrum_monitor.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"
#include "oscillate/test/wait.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace tolerance = oscillate::test::tolerance;

namespace {
using oscillate::application::SpectrumMonitor;
using oscillate::engine::StereoFrame;

using oscillate::test::require;

void worker_analyzes_only_complete_current_windows() {
  SpectrumMonitor monitor;
  monitor.set_sample_rate_after_quiesce(48'000);
  std::array<StereoFrame, 64> block{};
  for (std::size_t first = 0; first < 2048; first += block.size()) {
    for (std::size_t offset = 0; offset < block.size(); ++offset) {
      const auto phase =
          2.0 * std::numbers::pi * 37.0 * static_cast<double>(first + offset) / 2048.0;
      block[offset] = {static_cast<float>(0.8 * std::sin(phase)), 0.0F};
    }
    monitor.observe(block, {first, 1});
  }
  std::optional<oscillate::application::SpectrumReading> reading;
  oscillate::test::wait_until(
      [&] {
        reading = monitor.latest();
        return reading.has_value();
      },
      "the spectrum worker published the first window");
  REQUIRE_TRUE(reading, "spectrum worker failed to publish the post-callback stereo window");
  REQUIRE_EQ(reading->end_sample, 2048,
             "spectrum worker failed to publish the post-callback stereo window");
  REQUIRE_EQ(reading->transport_generation, 1,
             "spectrum worker failed to publish the post-callback stereo window");
  REQUIRE_LT(std::abs(reading->spectrum.left[37] - 0.8F), tolerance::float_processing_f,
             "spectrum worker failed to publish the post-callback stereo window");
  REQUIRE_LT(reading->spectrum.right[37], 1e-5F,
             "spectrum worker failed to publish the post-callback stereo window");
  monitor.invalidate_after_quiesce();
  require(!monitor.latest(), "spectrum from stopped output remained visible");
  monitor.set_sample_rate_after_quiesce(48'000);
  for (std::size_t first = 10'000; first < 12'048; first += block.size())
    monitor.observe(block, {first, 2});
  oscillate::test::wait_until(
      [&] {
        reading = monitor.latest();
        return reading && reading->transport_generation == 2;
      },
      "the spectrum worker published the new transport epoch");
  REQUIRE_TRUE(reading, "spectrum worker did not replace a stopped transport epoch");
  REQUIRE_EQ(reading->transport_generation, 2,
             "spectrum worker did not replace a stopped transport epoch");
  REQUIRE_EQ(reading->end_sample, 12'048,
             "spectrum worker did not replace a stopped transport epoch");
}
} // namespace

int main() {
  try {
    worker_analyzes_only_complete_current_windows();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

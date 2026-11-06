#include "oscillate/native_dsp/true_peak.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using oscillate::engine::StereoFrame;
using oscillate::native_dsp::StereoTruePeakMeter;
using oscillate::native_dsp::TruePeakReading;

using oscillate::test::require;

void analytical_intersample_oracle() {
  // At exactly fs/4 with pi/4 phase, every digital sample is +/- A/sqrt(2),
  // yet the continuous sine and a 4x phase grid reach A. This oracle does not
  // reuse the implementation's interpolation coefficients.
  for (const auto rate : {44100U, 48000U, 88200U, 96000U}) {
    StereoTruePeakMeter meter;
    require(meter.prepare(rate), "project rate rejected");
    std::array<StereoFrame, 128> block{};
    for (std::size_t start = 0; start < 4096; start += block.size()) {
      for (std::size_t index = 0; index < block.size(); ++index) {
        const auto sample = start + index;
        const double phase = std::numbers::pi * (static_cast<double>(sample) / 2.0 + 0.25);
        const double envelope = std::min(
            {1.0, static_cast<double>(sample) / 64.0, static_cast<double>(4095 - sample) / 64.0});
        block[index] = {static_cast<float>(0.9 * envelope * std::sin(phase)),
                        static_cast<float>(-0.6 * envelope * std::sin(phase))};
      }
      require(meter.process(block), "valid signal block rejected");
    }
    require(meter.finish(), "valid signal drain rejected");
    const TruePeakReading result = meter.reading();
    REQUIRE_LT(std::abs(result.left_sample_peak - 0.9 / std::sqrt(2.0)), 0.0001,
               "analytic digital-sample oracle failed");
    REQUIRE_LT(std::abs(result.right_sample_peak - 0.6 / std::sqrt(2.0)), 0.0001,
               "analytic digital-sample oracle failed");
    REQUIRE_LT(std::abs(result.left_true_peak - 0.9F), 0.01F,
               "intersample peak missed analytic continuous sine");
    REQUIRE_LT(std::abs(result.right_true_peak - 0.6F), 0.01F,
               "intersample peak missed analytic continuous sine");
    REQUIRE_GT(result.left_true_peak, result.left_sample_peak + 0.2F,
               "intersample peak missed analytic continuous sine");
    REQUIRE_GT(result.right_true_peak, result.right_sample_peak + 0.1F,
               "intersample peak missed analytic continuous sine");
    REQUIRE_EQ(result.frames, 4096, "signal frame accounting failed");
    REQUIRE_EQ(result.invalid_frames, 0, "signal frame accounting failed");
  }
}

void partition_and_drain() {
  std::array<StereoFrame, 129> signal{};
  signal[0] = {0.7F, -0.4F};
  signal[64] = {-0.9F, 0.3F};
  signal.back() = {0.8F, -0.5F};
  StereoTruePeakMeter whole;
  StereoTruePeakMeter partitioned;
  REQUIRE_TRUE(whole.prepare(48000), "prepare failed");
  REQUIRE_TRUE(partitioned.prepare(48000), "prepare failed");
  require(whole.process(signal), "whole signal rejected");
  REQUIRE_TRUE(partitioned.process(std::span(signal).first(1)), "partitioned signal rejected");
  REQUIRE_TRUE(partitioned.process(std::span(signal).subspan(1, 64)),
               "partitioned signal rejected");
  REQUIRE_TRUE(partitioned.process(std::span(signal).subspan(65)), "partitioned signal rejected");
  REQUIRE_TRUE(whole.finish(), "drain failed");
  REQUIRE_TRUE(partitioned.finish(), "drain failed");
  const auto a = whole.reading();
  const auto b = partitioned.reading();
  REQUIRE_EQ(a.left_true_peak, b.left_true_peak, "block partition changed measured peaks");
  REQUIRE_EQ(a.right_true_peak, b.right_true_peak, "block partition changed measured peaks");
  REQUIRE_EQ(a.left_sample_peak, b.left_sample_peak, "block partition changed measured peaks");
  REQUIRE_EQ(a.right_sample_peak, b.right_sample_peak, "block partition changed measured peaks");
  REQUIRE_EQ(a.frames, b.frames, "block partition changed measured peaks");
  REQUIRE_TRUE(whole.finish(), "repeated finish changed the result");
  REQUIRE_EQ(whole.reading().left_true_peak, a.left_true_peak,
             "repeated finish changed the result");
  require(!whole.process(std::span(signal).first(1)), "finished stream accepted more samples");

  whole.reset();
  REQUIRE_EQ(whole.reading().frames, 0, "reset retained old signal");
  REQUIRE_EQ(whole.reading().left_true_peak, 0.0F, "reset retained old signal");
  require(whole.process(std::span(signal).first(1)), "reset did not allow another stream");
}

void bounds_and_invalid_input() {
  StereoTruePeakMeter meter;
  const std::array<StereoFrame, 1> frame{{{0.5F, 0.2F}}};
  REQUIRE_TRUE(!meter.process(frame), "unprepared meter accepted input");
  REQUIRE_TRUE(!meter.finish(), "unprepared meter accepted input");
  REQUIRE_TRUE(!meter.prepare(32000), "unsupported rate accepted");
  REQUIRE_EQ(meter.sample_rate(), 0, "unsupported rate accepted");
  require(meter.prepare(48000), "supported rate rejected");
  REQUIRE_TRUE(!meter.prepare(192000), "invalid prepare damaged the prepared state");
  REQUIRE_EQ(meter.sample_rate(), 48000, "invalid prepare damaged the prepared state");
  std::vector<StereoFrame> oversized(StereoTruePeakMeter::max_block_frames + 1, {0.9F, 0.9F});
  REQUIRE_TRUE(!meter.process(oversized), "oversized block mutated meter state");
  REQUIRE_EQ(meter.reading().frames, 0, "oversized block mutated meter state");
  const std::array<StereoFrame, 3> malformed{{
      {std::numeric_limits<float>::quiet_NaN(), 0.3F},
      {0.2F, std::numeric_limits<float>::infinity()},
      {-0.5F, -0.25F},
  }};
  REQUIRE_TRUE(meter.process(malformed), "malformed stream rejected");
  REQUIRE_TRUE(meter.finish(), "malformed stream rejected");
  const auto result = meter.reading();
  REQUIRE_EQ(result.frames, 3, "invalid channel policy failed");
  REQUIRE_EQ(result.invalid_frames, 2, "invalid channel policy failed");
  REQUIRE_EQ(result.left_sample_peak, 0.5F, "invalid channel policy failed");
  REQUIRE_EQ(result.right_sample_peak, 0.3F, "invalid channel policy failed");
  REQUIRE_TRUE(std::isfinite(result.left_true_peak), "invalid channel policy failed");
  REQUIRE_TRUE(std::isfinite(result.right_true_peak), "invalid channel policy failed");

  meter.reset();
  const std::array<StereoFrame, 2> huge{{
      {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()},
      {-std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
  }};
  REQUIRE_TRUE(meter.process(huge), "large finite signal rejected");
  REQUIRE_TRUE(meter.finish(), "large finite signal rejected");
  REQUIRE_TRUE(std::isfinite(meter.reading().left_true_peak),
               "finite input produced an infinite meter result");
  REQUIRE_TRUE(std::isfinite(meter.reading().right_true_peak),
               "finite input produced an infinite meter result");
}

void callback_does_not_allocate() {
  StereoTruePeakMeter meter;
  require(meter.prepare(48000), "prepare failed");
  std::array<StereoFrame, 64> block{};
  block.fill({0.3F, -0.2F});
  counted_allocations = 0;
  counting_allocations = true;
  for (int index = 0; index < 100; ++index) {
    if (!meter.process(block)) {
      std::abort();
    }
    if (index == 49) {
      meter.reset();
    }
  }
  if (!meter.finish()) {
    std::abort();
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "callback operations allocated");
}

} // namespace

int main() {
  try {
    analytical_intersample_oracle();
    partition_and_drain();
    bounds_and_invalid_input();
    callback_does_not_allocate();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

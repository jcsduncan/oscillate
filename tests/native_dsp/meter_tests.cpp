#include "oscillate/native_dsp/meter.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;

using oscillate::test::require;

void window_and_generation() {
  StereoMeter meter;
  require(!meter.latest(), "unpublished meter exposed a reading");
  std::array<engine::StereoFrame, 128> frames{};
  frames.fill({0.5F, -0.25F});
  for (std::uint64_t position = 0; position < 1024; position += frames.size()) {
    meter.observe(frames, {position, 1});
  }
  const auto first = meter.latest();
  REQUIRE_TRUE(first, "constant window peak/RMS oracle failed");
  REQUIRE_EQ(first->left_peak, 0.5F, "constant window peak/RMS oracle failed");
  REQUIRE_EQ(first->right_peak, 0.25F, "constant window peak/RMS oracle failed");
  REQUIRE_EQ(first->left_rms, 0.5F, "constant window peak/RMS oracle failed");
  REQUIRE_EQ(first->right_rms, 0.25F, "constant window peak/RMS oracle failed");
  REQUIRE_EQ(first->end_sample, 1024, "constant window peak/RMS oracle failed");
  REQUIRE_EQ(first->transport_generation, 1, "constant window peak/RMS oracle failed");
  REQUIRE_TRUE(!first->clipped, "constant window peak/RMS oracle failed");

  frames.fill({0.0F, 0.0F});
  frames[0] = {1.0F, -0.75F};
  for (std::uint64_t position = 1024; position < 2048; position += frames.size()) {
    meter.observe(frames, {position, 1});
    frames[0] = {0.0F, 0.0F};
  }
  const auto transient = meter.latest();
  REQUIRE_TRUE(transient, "impulse RMS or clipping oracle failed");
  REQUIRE_EQ(transient->left_peak, 1.0F, "impulse RMS or clipping oracle failed");
  REQUIRE_EQ(transient->right_peak, 0.75F, "impulse RMS or clipping oracle failed");
  REQUIRE_LT(std::abs(transient->left_rms - 0.03125F), 1e-7F,
             "impulse RMS or clipping oracle failed");
  REQUIRE_TRUE(transient->clipped, "impulse RMS or clipping oracle failed");

  meter.observe(frames, {500, 2});
  const auto reset = meter.latest();
  REQUIRE_TRUE(reset, "new transport generation retained stale meter state");
  REQUIRE_EQ(reset->left_peak, 0.0F, "new transport generation retained stale meter state");
  REQUIRE_EQ(reset->end_sample, 500, "new transport generation retained stale meter state");
  REQUIRE_EQ(reset->transport_generation, 2, "new transport generation retained stale meter state");
  REQUIRE_TRUE(!reset->clipped, "new transport generation retained stale meter state");
  meter.observe(frames, {628, 2});
  meter.observe(frames, {900, 2});
  const auto discontinuity = meter.latest();
  REQUIRE_TRUE(discontinuity, "position discontinuity did not discard partial window");
  REQUIRE_EQ(discontinuity->end_sample, 900,
             "position discontinuity did not discard partial window");
  REQUIRE_EQ(discontinuity->transport_generation, 2,
             "position discontinuity did not discard partial window");

  meter.observe(frames, {1028, 0});
  const auto stopped = meter.latest();
  REQUIRE_TRUE(stopped, "invalid transport left a stale meter value visible");
  REQUIRE_EQ(stopped->transport_generation, 0,
             "invalid transport left a stale meter value visible");
  REQUIRE_EQ(stopped->left_peak, 0.0F, "invalid transport left a stale meter value visible");
  REQUIRE_EQ(stopped->end_sample, 0, "invalid transport left a stale meter value visible");
  meter.observe(frames, {std::numeric_limits<std::uint64_t>::max() - 1, 3});
  REQUIRE_TRUE(meter.latest(), "sample-position overflow was accepted");
  REQUIRE_EQ(meter.latest()->transport_generation, 0, "sample-position overflow was accepted");
  meter.observe(frames, {0, 4});
  meter.invalidate_after_quiesce();
  REQUIRE_TRUE(meter.latest(), "quiescent stop left a stale meter value visible");
  REQUIRE_EQ(meter.latest()->transport_generation, 0,
             "quiescent stop left a stale meter value visible");
}

void planned_loop_wrap_keeps_meter_window() {
  StereoMeter meter;
  std::array<engine::StereoFrame, 128> frames{};
  frames.fill({0.5F, -0.25F});
  for (std::uint64_t position = 0; position < 512; position += frames.size())
    meter.observe(frames, {position, 1});
  meter.observe(frames, {0, 2, 1, true});
  for (std::uint64_t position = 128; position < 512; position += frames.size())
    meter.observe(frames, {position, 2, 1});
  const auto reading = meter.latest();
  REQUIRE_TRUE(reading, "planned loop wrap discarded a partial meter window");
  REQUIRE_EQ(reading->transport_generation, 2,
             "planned loop wrap discarded a partial meter window");
  REQUIRE_EQ(reading->end_sample, 512, "planned loop wrap discarded a partial meter window");
  REQUIRE_EQ(reading->left_peak, 0.5F, "planned loop wrap discarded a partial meter window");
  REQUIRE_EQ(reading->left_rms, 0.5F, "planned loop wrap discarded a partial meter window");
}

void invalid_input_and_partition() {
  StereoMeter meter;
  std::array<engine::StereoFrame, StereoMeter::window_frames> frames{};
  frames.fill({0.5F, -0.25F});
  frames[17].left = std::numeric_limits<float>::quiet_NaN();
  frames[29].right = std::numeric_limits<float>::infinity();
  meter.observe(frames, {0, 1});
  const auto reading = meter.latest();
  if (!reading) {
    throw std::runtime_error("meter failed to publish full window");
  }
  REQUIRE_TRUE(reading, "non-finite audio contaminated a meter window");
  REQUIRE_EQ(reading->invalid_frames, 2, "non-finite audio contaminated a meter window");
  REQUIRE_TRUE(std::isfinite(reading->left_rms), "non-finite audio contaminated a meter window");
  REQUIRE_TRUE(std::isfinite(reading->right_rms), "non-finite audio contaminated a meter window");
  REQUIRE_TRUE(!reading->clipped, "non-finite audio contaminated a meter window");

  StereoMeter partitioned;
  for (std::size_t offset = 0; offset < frames.size(); offset += 64) {
    partitioned.observe(std::span(frames).subspan(offset, 64), {offset, 1});
  }
  const auto split = partitioned.latest();
  if (!split) {
    throw std::runtime_error("partitioned meter failed to publish full window");
  }
  REQUIRE_TRUE(split, "buffer partition altered window metrics");
  REQUIRE_EQ(split->left_peak, reading->left_peak, "buffer partition altered window metrics");
  REQUIRE_EQ(split->right_peak, reading->right_peak, "buffer partition altered window metrics");
  REQUIRE_EQ(split->left_rms, reading->left_rms, "buffer partition altered window metrics");
  REQUIRE_EQ(split->right_rms, reading->right_rms, "buffer partition altered window metrics");
  REQUIRE_EQ(split->invalid_frames, reading->invalid_frames,
             "buffer partition altered window metrics");
}

void concurrent_reading() {
  StereoMeter meter;
  std::array<engine::StereoFrame, 64> frames{};
  std::atomic<bool> complete{false};
  std::atomic<bool> invalid{false};
  std::thread reader([&] {
    while (!complete.load()) {
      const auto value = meter.latest();
      if (value && value->end_sample != 0 &&
          (value->left_peak != value->left_rms || value->right_peak != value->right_rms ||
           value->right_peak != value->left_peak * 0.5F ||
           value->end_sample % StereoMeter::window_frames != 0)) {
        invalid.store(true);
      }
    }
  });
  std::uint64_t position = 0;
  for (int window = 0; window < 256; ++window) {
    const float level = window % 2 == 0 ? 0.25F : 0.5F;
    frames.fill({level, -level * 0.5F});
    for (int block = 0; block < 16; ++block) {
      meter.observe(frames, {position, 1});
      position += frames.size();
    }
  }
  complete.store(true);
  reader.join();
  REQUIRE_TRUE(!invalid.load(), "concurrent UI read observed a torn meter window");
  REQUIRE_TRUE(meter.latest(), "concurrent UI read observed a torn meter window");
  REQUIRE_EQ(meter.latest()->end_sample, position,
             "concurrent UI read observed a torn meter window");
}

void callback_allocation() {
  StereoMeter meter;
  std::array<engine::StereoFrame, 64> frames{};
  frames.fill({0.25F, -0.125F});
  counted_allocations = 0;
  counting_allocations = true;
  for (std::uint64_t block = 0; block < 1000; ++block) {
    meter.observe(frames, {block * frames.size(), 1});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "meter callback allocated memory");
}
} // namespace

int main() {
  try {
    window_and_generation();
    planned_loop_wrap_keeps_meter_window();
    invalid_input_and_partition();
    concurrent_reading();
    callback_allocation();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/limiter.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;

static_assert(std::is_base_of_v<engine::ProcessorRuntime, LimiterRuntime>);
static_assert(noexcept(std::declval<LimiterRuntime&>().process(
    std::declval<std::span<engine::StereoFrame>>(), engine::BlockContext{})));

using oscillate::test::require;

PreparedLimiter prepare(LimiterSettings settings = {},
                        std::uint32_t rate = oscillate::test::sample_rate_48k) {
  auto result = prepare_limiter(settings, rate);
  REQUIRE_OK(result, "valid limiter settings rejected");
  return std::move(result).value();
}

void validation() {
  const LimiterSettings valid{};
  REQUIRE_EQ(prepare(valid).latency_frames(), 240, "prepared limiter latency/rate changed");
  REQUIRE_EQ(prepare(valid, 44'100).sample_rate(), 44'100, "prepared limiter latency/rate changed");
  REQUIRE_TRUE(!prepare_limiter(valid, 32'000), "unsupported sample rate accepted");
  REQUIRE_TRUE(!prepare_limiter(valid, 192'000), "unsupported sample rate accepted");
  auto invalid = [valid](auto mutate) {
    auto candidate = valid;
    mutate(candidate);
    return !prepare_limiter(candidate, 48'000);
  };
  REQUIRE_TRUE(invalid([](auto& s) { s.ceiling_db = -30.01; }),
               "invalid limiter controls accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.ceiling_db = 0.01; }), "invalid limiter controls accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.ceiling_db = NAN; }), "invalid limiter controls accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.lookahead_ms = -0.01; }),
               "invalid limiter controls accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.lookahead_ms = 10.01; }),
               "invalid limiter controls accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.lookahead_ms = INFINITY; }),
               "invalid limiter controls accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.release_ms = 0.99; }), "invalid limiter controls accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.release_ms = 2'000.01; }),
               "invalid limiter controls accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.release_ms = NAN; }), "invalid limiter controls accepted");
}

void impulse_and_linked_stereo() {
  LimiterRuntime limiter(prepare({-6.020599913279624, 1.0, 50.0}));
  REQUIRE_EQ(limiter.latency_frames(), 48, "lookahead latency/tail contract changed");
  REQUIRE_EQ(limiter.tail_frames(), 48, "lookahead latency/tail contract changed");
  std::array<engine::StereoFrame, 128> signal{};
  signal[0] = {1.0F, -0.5F};
  limiter.process(signal, {0, 1});
  for (std::size_t i = 0; i < signal.size(); ++i) {
    if (i == 48) {
      REQUIRE_LT(std::abs(signal[i].left - 0.5F), tolerance::float_round_trip_f,
                 "lookahead impulse amplitude or linked stereo ratio changed");
      REQUIRE_LT(std::abs(signal[i].right + 0.25F), tolerance::float_round_trip_f,
                 "lookahead impulse amplitude or linked stereo ratio changed");
    } else {
      REQUIRE_EQ(signal[i].left, 0.0F, "limiter produced an unexpected impulse frame");
      REQUIRE_EQ(signal[i].right, 0.0F, "limiter produced an unexpected impulse frame");
    }
  }

  LimiterRuntime tail(prepare({0.0, 1.0, 50.0}));
  std::array<engine::StereoFrame, 1> final_source{{{0.75F, -0.25F}}};
  tail.process(final_source, {0, 1});
  REQUIRE_EQ(final_source[0].left, 0.0F, "lookahead emitted before its reported latency");
  std::array<engine::StereoFrame, 48> flush{};
  tail.process(flush, {1, 1});
  REQUIRE_LT(std::abs(flush.back().left - 0.75F), tolerance::float_round_trip_f,
             "reported tail did not include final source sample");
}

void ceiling_and_block_partition() {
  constexpr LimiterSettings settings{-1.0, 1.0, 10.0};
  LimiterRuntime contiguous(prepare(settings));
  LimiterRuntime partitioned(prepare(settings));
  std::array<engine::StereoFrame, 2'048> original{};
  for (std::size_t i = 0; i < original.size(); ++i) {
    const float input = static_cast<float>(1.5 * std::sin(0.017 * static_cast<double>(i)));
    original[i] = {input, -input * 0.4F};
  }
  auto one = original;
  auto many = original;
  contiguous.process(one, {0, 1});
  std::size_t offset = 0;
  while (offset < many.size()) {
    const auto count = std::min<std::size_t>(37, many.size() - offset);
    partitioned.process(std::span(many).subspan(offset, count), {offset, 1});
    offset += count;
  }
  const float ceiling = static_cast<float>(std::pow(10.0, -1.0 / 20.0));
  for (std::size_t i = 0; i < one.size(); ++i) {
    REQUIRE_LE(std::abs(one[i].left), ceiling + 1e-7F, "digital ceiling exceeded");
    REQUIRE_LE(std::abs(one[i].right), ceiling + 1e-7F, "digital ceiling exceeded");
    REQUIRE_EQ(one[i].left, many[i].left, "block partition changed limiter output");
    REQUIRE_EQ(one[i].right, many[i].right, "block partition changed limiter output");
  }
  REQUIRE_GT(std::abs(one[500].left), 0.05F, "limiter muted ordinary signal");
}

// Deliberately use a simple scan rather than the runtime's monotonic queue.
// This checks eviction, equal peaks and ring wrap at each supported lookahead.
void independent_lookahead_oracle() {
  constexpr std::size_t frames = 1'537;
  std::array<engine::StereoFrame, frames> input{};
  for (std::size_t i = 0; i < frames; ++i) {
    const float pulse = i % 37 == 0 ? 1.4F : 0.0F;
    const float wave = static_cast<float>(0.65 * std::sin(0.073 * static_cast<double>(i)));
    input[i] = {pulse + wave, i % 53 == 0 ? -1.2F : -0.7F * wave};
  }
  for (const double lookahead_ms : {0.0, 1.0, 5.0, 10.0}) {
    const LimiterSettings settings{-3.0, lookahead_ms, 80.0};
    LimiterRuntime runtime(prepare(settings));
    auto actual = input;
    std::size_t offset = 0;
    while (offset < actual.size()) {
      const auto count = std::min<std::size_t>(17 + offset % 113, actual.size() - offset);
      runtime.process(std::span(actual).subspan(offset, count), {offset, 1});
      offset += count;
    }
    const auto latency = runtime.latency_frames();
    const double ceiling = oscillate::native_dsp::dsp::db_to_gain(settings.ceiling_db);
    const double release = std::exp(-1'000.0 / (settings.release_ms * 48'000.0));
    double gain = 1.0;
    for (std::size_t i = 0; i < frames; ++i) {
      double peak = 0.0;
      const auto begin = i > latency ? i - latency : 0;
      for (std::size_t j = begin; j <= i; ++j) {
        peak = std::max(
            peak, static_cast<double>(std::max(std::abs(input[j].left), std::abs(input[j].right))));
      }
      const double target = peak > ceiling ? ceiling / peak : 1.0;
      gain = target < gain ? target : release * gain + (1.0 - release) * target;
      const auto delayed = i >= latency ? input[i - latency] : engine::StereoFrame{};
      const auto left = static_cast<float>(
          std::clamp(static_cast<double>(delayed.left) * gain, -ceiling, ceiling));
      const auto right = static_cast<float>(
          std::clamp(static_cast<double>(delayed.right) * gain, -ceiling, ceiling));
      REQUIRE_LT(std::abs(actual[i].left - left), tolerance::float_round_trip_f,
                 "limiter output disagreed with independent lookahead scan");
      REQUIRE_LT(std::abs(actual[i].right - right), tolerance::float_round_trip_f,
                 "limiter output disagreed with independent lookahead scan");
    }
  }
}

void zero_lookahead_release_and_guards() {
  LimiterRuntime limiter(prepare({-6.020599913279624, 0.0, 100.0}));
  std::array<engine::StereoFrame, 64> signal{};
  signal.fill({1.0F, -0.5F});
  limiter.process(signal, {0, 2});
  REQUIRE_LT(std::abs(signal[0].left - 0.5F), tolerance::float_round_trip_f,
             "zero-lookahead digital peak limiting changed");
  REQUIRE_LT(std::abs(signal.back().right + 0.25F), tolerance::float_round_trip_f,
             "zero-lookahead digital peak limiting changed");
  signal.fill({0.1F, 0.1F});
  limiter.process(signal, {64, 2});
  REQUIRE_LT(signal[0].left, signal.back().left, "release did not recover gain gradually");
  REQUIRE_LE(signal.back().left, 0.1F, "release did not recover gain gradually");
  signal.fill({0.1F, 0.1F});
  limiter.process(signal, {128, 1});
  REQUIRE_EQ(signal.front().left, 0.0F, "stale generation was not silenced");
  REQUIRE_EQ(signal.back().right, 0.0F, "stale generation was not silenced");
  signal.fill({0.1F, 0.1F});
  limiter.process(signal, {128, 3});
  REQUIRE_LT(std::abs(signal.front().left - 0.1F), tolerance::float_round_trip_f,
             "new generation did not reset limiter history");
  signal[0] = {std::numeric_limits<float>::quiet_NaN(), 1.0F};
  limiter.process(signal, {192, 3});
  REQUIRE_TRUE(std::isfinite(signal.front().left),
               "non-finite input or stale generation guard failed");
  REQUIRE_TRUE(std::isfinite(signal.front().right),
               "non-finite input or stale generation guard failed");
  REQUIRE_GE(limiter.guarded_frames_after_quiesce(), 65,
             "non-finite input or stale generation guard failed");
  limiter.reset_after_quiesce();
  REQUIRE_EQ(limiter.guarded_frames_after_quiesce(), 0, "quiescent guard reset failed");
  signal.fill({0.2F, -0.2F});
  counted_allocations = 0;
  counting_allocations = true;
  for (int i = 0; i < 1'000; ++i) {
    limiter.process(signal, {static_cast<std::uint64_t>(i) * signal.size(), 1});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "limiter callback allocated memory");
}

void callback_allocation_matrix() {
  for (const LimiterSettings settings :
       {LimiterSettings{-1.0, 0.0, 50.0}, LimiterSettings{-1.0, 5.0, 100.0},
        LimiterSettings{-1.0, 10.0, 500.0}}) {
    for (const std::size_t frames : {64U, 128U}) {
      LimiterRuntime limiter(prepare(settings));
      std::array<engine::StereoFrame, 128> block{};
      block.fill({1.2F, -0.8F});
      counted_allocations = 0;
      counting_allocations = true;
      for (std::uint64_t index = 0; index < 1'000; ++index)
        limiter.process(std::span(block).first(frames), {index * frames, 1});
      counting_allocations = false;
      REQUIRE_EQ(counted_allocations, 0, "representative limiter callback allocated memory");
    }
  }
}

void gain_reduction_reports_audible_delayed_attenuation() {
  constexpr double six_db_ceiling = -6.020599913279624;
  LimiterRuntime zero_lookahead(prepare({six_db_ceiling, 0.0, 100.0}));
  REQUIRE_EQ(zero_lookahead.latest_gain_reduction_db(), 0.0F,
             "new limiter published a reduction before processing");
  std::array<engine::StereoFrame, 64> loud{};
  loud.fill({1.0F, -0.25F});
  zero_lookahead.process(loud, {0, 1});
  REQUIRE_LT(std::abs(zero_lookahead.latest_gain_reduction_db() - 6.0206F), 0.001F,
             "limiter meter did not report the independently known 2:1 attenuation");
  std::array<engine::StereoFrame, 64> silent{};
  zero_lookahead.process(silent, {64, 1});
  REQUIRE_EQ(zero_lookahead.latest_gain_reduction_db(), 0.0F,
             "residual envelope on silent output was reported as audible reduction");
  loud.fill({0.1F, -0.1F});
  zero_lookahead.process(loud, {128, 0});
  REQUIRE_EQ(zero_lookahead.latest_gain_reduction_db(), 0.0F,
             "guarded generation was reported as limiter attenuation");
  zero_lookahead.process(loud, {192, 2});
  REQUIRE_EQ(zero_lookahead.latest_gain_reduction_db(), 0.0F,
             "new generation inherited an old reduction reading");
  zero_lookahead.reset_after_quiesce();
  REQUIRE_EQ(zero_lookahead.latest_gain_reduction_db(), 0.0F,
             "quiescent reset retained limiter reduction");

  LimiterRuntime delayed(prepare({six_db_ceiling, 1.0, 100.0}));
  std::array<engine::StereoFrame, 32> first{};
  first[0] = {1.0F, 0.0F};
  delayed.process(first, {0, 1});
  REQUIRE_EQ(delayed.latest_gain_reduction_db(), 0.0F,
             "lookahead detector reported reduction before delayed audio was audible");
  std::array<engine::StereoFrame, 32> second{};
  delayed.process(second, {32, 1});
  REQUIRE_GT(delayed.latest_gain_reduction_db(), 5.0F,
             "lookahead meter missed attenuation on the delayed impulse");
  REQUIRE_LE(delayed.latest_gain_reduction_db(), 6.0207F,
             "lookahead meter missed attenuation on the delayed impulse");
  std::array<engine::StereoFrame, 64> tail{};
  delayed.process(tail, {64, 1});
  REQUIRE_EQ(delayed.latest_gain_reduction_db(), 0.0F,
             "silent lookahead tail retained a stale reduction reading");
  delayed.process(std::span(tail).first(0), {128, 1});
  REQUIRE_EQ(delayed.latest_gain_reduction_db(), 0.0F,
             "empty limiter block retained a stale reduction reading");
}

void live_mailbox_preserves_delay_and_gain_history() {
  constexpr LimiterSettings initial{-6.0, 1.0, 200.0};
  constexpr LimiterSettings changed{-3.0, 1.0, 20.0};
  LimiterRuntime limiter(prepare(initial));
  std::array<engine::StereoFrame, 128> source{};
  for (std::size_t index = 0; index < source.size(); ++index) {
    const float amplitude = index < 56 ? 1.5F : 0.1F;
    source[index] = {amplitude, -0.5F * amplitude};
  }
  auto actual = source;
  limiter.process(std::span(actual).first(64), {0, 1});
  REQUIRE_EQ(limiter.applied_prepared_revision(), 0,
             "limiter reported a live revision before an offer");
  require(limiter.offer_prepared(prepare(changed), 1), "compatible limiter edit rejected");
  REQUIRE_EQ(limiter.applied_prepared_revision(), 0,
             "limiter reported a live revision before the next block");
  counted_allocations = 0;
  counting_allocations = true;
  limiter.process(std::span(actual).subspan(64), {64, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0,
             "limiter live edit allocated or missed the next block boundary");
  REQUIRE_EQ(limiter.applied_prepared_revision(), 1,
             "limiter live edit allocated or missed the next block boundary");

  // Independent sliding-window peak scan and gain envelope. The reference
  // deliberately keeps its own delay/gain state across the parameter change.
  constexpr std::size_t latency = 48;
  double gain = 1.0;
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto settings = index < 64 ? initial : changed;
    const double ceiling = oscillate::native_dsp::dsp::db_to_gain(settings.ceiling_db);
    const double release = std::exp(-1'000.0 / (settings.release_ms * 48'000.0));
    double window_peak = 0.0;
    for (std::size_t scan = index > latency ? index - latency : 0; scan <= index; ++scan)
      window_peak = std::max(window_peak, static_cast<double>(std::abs(source[scan].left)));
    const double target = window_peak > ceiling ? ceiling / window_peak : 1.0;
    gain = target < gain ? target : release * gain + (1.0 - release) * target;
    const auto delayed = index < latency ? engine::StereoFrame{} : source[index - latency];
    const float expected_left =
        static_cast<float>(std::clamp(static_cast<double>(delayed.left) * gain, -ceiling, ceiling));
    const float expected_right = static_cast<float>(
        std::clamp(static_cast<double>(delayed.right) * gain, -ceiling, ceiling));
    REQUIRE_LT(std::abs(actual[index].left - expected_left), tolerance::float_round_trip_f,
               "live limiter edit reset delayed audio, peak window or gain envelope");
    REQUIRE_LT(std::abs(actual[index].right - expected_right), tolerance::float_round_trip_f,
               "live limiter edit reset delayed audio, peak window or gain envelope");
  }
  REQUIRE_GT(actual[64].left, 0.0F, "boundary frame did not retain delayed and limited audio");
  REQUIRE_LT(actual[64].left, 0.8F, "boundary frame did not retain delayed and limited audio");
}

void live_mailbox_rejects_incompatible_and_stale_offers() {
  LimiterRuntime limiter(prepare({-1.0, 1.0, 100.0}));
  const auto compatible = prepare({-2.0, 1.0, 50.0});
  REQUIRE_TRUE(!limiter.offer_prepared(compatible, 0),
               "limiter mailbox accepted zero revision, latency or sample-rate change");
  REQUIRE_TRUE(!limiter.offer_prepared(prepare({-2.0, 2.0, 50.0}), 1),
               "limiter mailbox accepted zero revision, latency or sample-rate change");
  REQUIRE_TRUE(!limiter.offer_prepared(prepare({-2.0, 1.0, 50.0}, 44'100), 1),
               "limiter mailbox accepted zero revision, latency or sample-rate change");
  for (std::uint64_t revision = 1; revision <= 4; ++revision)
    require(limiter.offer_prepared(compatible, revision), "limiter mailbox rejected a free slot");
  REQUIRE_TRUE(!limiter.offer_prepared(compatible, 5),
               "full limiter mailbox accepted a revision or acknowledged it early");
  REQUIRE_EQ(limiter.applied_prepared_revision(), 0,
             "full limiter mailbox accepted a revision or acknowledged it early");
  std::array<engine::StereoFrame, 64> block{};
  limiter.process(block, {0, 1});
  REQUIRE_EQ(limiter.applied_prepared_revision(), 4,
             "limiter mailbox failed to apply newest revision or reject a stale one");
  REQUIRE_TRUE(!limiter.offer_prepared(compatible, 4),
               "limiter mailbox failed to apply newest revision or reject a stale one");
  REQUIRE_TRUE(limiter.offer_prepared(compatible, 5),
               "limiter mailbox failed to apply newest revision or reject a stale one");
  limiter.reset_after_quiesce();
  REQUIRE_EQ(limiter.applied_prepared_revision(), 0,
             "quiescent limiter reset did not reset mailbox sequence");
  REQUIRE_TRUE(limiter.offer_prepared(compatible, 1),
               "quiescent limiter reset did not reset mailbox sequence");
}

void concurrent_live_revisions_remain_monotonic() {
  constexpr std::uint64_t last_revision = 500;
  LimiterRuntime limiter(prepare({-1.0, 1.0, 100.0}));
  const auto high = prepare({-1.0, 1.0, 20.0});
  const auto low = prepare({-6.0, 1.0, 200.0});
  std::jthread producer([&](const std::stop_token& stop) {
    for (std::uint64_t revision = 1; revision <= last_revision; ++revision) {
      const auto prepared = revision % 2 == 0 ? high : low;
      while (!limiter.offer_prepared(prepared, revision)) {
        if (stop.stop_requested())
          return;
        std::this_thread::yield();
      }
    }
  });
  std::array<engine::StereoFrame, 64> block{};
  std::uint64_t observed = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (observed < last_revision && std::chrono::steady_clock::now() < deadline) {
    block.fill({0.4F, -0.2F});
    limiter.process(block, {observed * block.size(), 1});
    const auto active = limiter.applied_prepared_revision();
    REQUIRE_GE(active, observed,
               "concurrent limiter mailbox revision regressed or exceeded offered range");
    REQUIRE_LE(active, last_revision,
               "concurrent limiter mailbox revision regressed or exceeded offered range");
    observed = active;
    std::this_thread::yield();
  }
  producer.request_stop();
  producer.join();
  REQUIRE_EQ(observed, last_revision, "concurrent limiter mailbox missed final revision");
}
} // namespace

int main() {
  try {
    validation();
    impulse_and_linked_stereo();
    ceiling_and_block_partition();
    independent_lookahead_oracle();
    zero_lookahead_release_and_guards();
    callback_allocation_matrix();
    gain_reduction_reports_audible_delayed_attenuation();
    live_mailbox_preserves_delay_and_gain_history();
    live_mailbox_rejects_incompatible_and_stale_offers();
    concurrent_live_revisions_remain_monotonic();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

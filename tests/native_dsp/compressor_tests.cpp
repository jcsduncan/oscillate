#include "oscillate/native_dsp/compressor.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

using oscillate::test::counted_allocations;
using oscillate::test::counting_allocations;

namespace {
using namespace oscillate;
using namespace oscillate::native_dsp;

static_assert(std::is_base_of_v<engine::ProcessorRuntime, CompressorRuntime>);
static_assert(noexcept(std::declval<CompressorRuntime&>().process(
    std::declval<std::span<engine::StereoFrame>>(), engine::BlockContext{})));

using oscillate::test::require;

PreparedCompressor prepare(CompressorSettings settings = {},
                           std::uint32_t rate = oscillate::test::sample_rate_48k) {
  auto result = prepare_compressor(settings, rate);
  REQUIRE_OK(result, "valid compressor settings rejected");
  return std::move(result).value();
}

void validation() {
  const CompressorSettings valid{};
  REQUIRE_TRUE(!prepare_compressor(valid, 44'000), "invalid sample rate accepted");
  REQUIRE_TRUE(!prepare_compressor(valid, 192'001), "invalid sample rate accepted");
  auto invalid = [valid](auto mutation) {
    auto settings = valid;
    mutation(settings);
    return !prepare_compressor(settings, 48'000);
  };
  REQUIRE_TRUE(invalid([](auto& s) { s.threshold_db = -80.01; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.threshold_db = 0.01; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.threshold_db = NAN; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.ratio = 0.99; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.ratio = 20.01; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.ratio = INFINITY; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.knee_db = -0.01; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.knee_db = 24.01; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.knee_db = NAN; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.attack_ms = 0.09; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.attack_ms = 200.01; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.attack_ms = NAN; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.release_ms = 0.99; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.release_ms = 2'000.01; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.release_ms = NAN; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.makeup_db = -24.01; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.makeup_db = 24.01; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.makeup_db = NAN; }), "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.sidechain_highpass_hz = 19.0; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.sidechain_highpass_hz = 2'001.0; }),
               "invalid compressor control accepted");
  REQUIRE_TRUE(invalid([](auto& s) { s.sidechain_highpass_hz = NAN; }),
               "invalid compressor control accepted");
  REQUIRE_EQ(prepare(valid).sample_rate(), 48'000, "prepared rate missing");
}

void steady_state_transfer() {
  constexpr CompressorSettings hard{-20.0, 4.0, 0.0, 0.1, 1.0, 0.0};
  CompressorRuntime compressor(prepare(hard));
  std::array<engine::StereoFrame, 128> block{};
  for (int iteration = 0; iteration < 2'000; ++iteration) {
    block.fill({0.1F, -0.05F});
    compressor.process(block, {static_cast<std::uint64_t>(iteration) * block.size(), 1});
  }
  REQUIRE_LT(std::abs(block[0].left - 0.1F), 0.0001F, "at-threshold signal compressed");
  for (int iteration = 0; iteration < 2'000; ++iteration) {
    block.fill({1.0F, -0.5F});
    compressor.process(block, {static_cast<std::uint64_t>(iteration) * block.size(), 1});
  }
  // 0 dBFS input, -20 dBFS threshold, 4:1: -15 dB gain => 0.17783.
  const double expected = std::pow(10.0, -15.0 / 20.0);
  REQUIRE_LT(std::abs(block.back().left - expected), 0.0002,
             "hard-knee gain reduction differs from dB oracle");
  REQUIRE_LT(std::abs(block.back().right + expected * 0.5), 0.0002,
             "stereo linked gain did not preserve input ratio");

  CompressorRuntime ratio_one(prepare({-30.0, 1.0, 12.0, 1.0, 50.0, 6.0}));
  block.fill({0.25F, -0.125F});
  ratio_one.process(block, {0, 1});
  const float makeup = static_cast<float>(std::pow(10.0, 6.0 / 20.0));
  REQUIRE_LT(std::abs(block.back().left - 0.25F * makeup), tolerance::float_round_trip_f,
             "ratio-one signal changed beyond makeup");
}

void knee_and_step_oracle() {
  // At knee midpoint, 4:1 reduction is 0.75*knee/8 = 0.75 dB for 8 dB knee.
  const CompressorSettings soft{-12.0, 4.0, 8.0, 0.1, 1.0, 0.0};
  CompressorRuntime compressor(prepare(soft));
  const float amplitude = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
  std::array<engine::StereoFrame, 128> block{};
  for (int iteration = 0; iteration < 1'000; ++iteration) {
    block.fill({amplitude, amplitude});
    compressor.process(block, {static_cast<std::uint64_t>(iteration) * block.size(), 1});
  }
  const double expected = static_cast<double>(amplitude) * std::pow(10.0, -0.75 / 20.0);
  REQUIRE_LT(std::abs(block.back().left - expected), 0.0001,
             "soft-knee midpoint differs from independent oracle");

  // Closed-form first-order step response. With attack 10 ms at 48 kHz,
  // the peak envelope after 480 samples is 1 - exp(-1).
  CompressorRuntime transient(prepare({-20.0, 4.0, 0.0, 10.0, 100.0, 0.0}));
  std::array<engine::StereoFrame, 480> step{};
  step.fill({1.0F, 1.0F});
  transient.process(step, {0, 1});
  const double envelope_480 = 1.0 - std::exp(-1.0);
  const double reduction = (20.0 * std::log10(envelope_480) + 20.0) * 0.75;
  REQUIRE_LT(std::abs(step.back().left - std::pow(10.0, -reduction / 20.0)), 0.0001,
             "attack step response differs from closed-form envelope");
  REQUIRE_GT(step.front().left, step.back().left, "attack did not increase reduction");
  std::array<engine::StereoFrame, 4'800> quiet{};
  transient.process(quiet, {480, 1});
  std::array<engine::StereoFrame, 1> returned{{{1.0F, 1.0F}}};
  transient.process(returned, {5'280, 1});
  REQUIRE_GT(returned[0].left, step.back().left, "release did not recover gain after silence");
}

void sidechain_and_transport() {
  CompressorRuntime compressor(prepare({-20.0, 4.0, 0.0, 0.1, 10.0, 0.0}));
  std::array<engine::StereoFrame, 128> signal{};
  std::array<engine::StereoFrame, 128> sidechain{};
  for (int iteration = 0; iteration < 500; ++iteration) {
    signal.fill({0.1F, -0.1F});
    sidechain.fill({1.0F, 0.0F});
    compressor.process_with_sidechain(signal, sidechain, {0, 3});
  }
  REQUIRE_LT(signal.back().left, 0.019F, "external sidechain did not reduce signal");
  signal.fill({0.1F, 0.1F});
  compressor.process(signal, {0, 2});
  REQUIRE_EQ(signal[0].left, 0.0F, "stale transport generation not silenced");
  REQUIRE_EQ(signal.back().right, 0.0F, "stale transport generation not silenced");
  signal.fill({0.1F, 0.1F});
  compressor.process(signal, {0, 4});
  REQUIRE_GT(signal[0].left, 0.09F, "new generation did not reset detector");
  signal.fill({0.1F, 0.1F});
  compressor.process_with_sidechain(signal, std::span(sidechain).first(64), {0, 4});
  REQUIRE_EQ(signal[0].left, 0.0F, "mismatched sidechain length not silenced");
  REQUIRE_EQ(signal.back().left, 0.0F, "mismatched sidechain length not silenced");
  REQUIRE_GE(compressor.guarded_frames_after_quiesce(), 256, "guard did not record failures");
  compressor.reset_after_quiesce();
  REQUIRE_EQ(compressor.guarded_frames_after_quiesce(), 0, "quiescent reset did not clear guard");
}

void filtered_sidechain_rejects_dc_but_keeps_fast_transients() {
  const auto settings = CompressorSettings{-30.0, 8.0, 0.0, 0.1, 10.0, 0.0, 200.0};
  CompressorRuntime filtered(prepare(settings));
  CompressorRuntime plain(prepare({-30.0, 8.0, 0.0, 0.1, 10.0, 0.0}));
  std::array<engine::StereoFrame, 128> signal{}, direct{}, detector{};
  for (std::size_t block = 0; block < 400; ++block) {
    signal.fill({0.1F, -0.1F});
    direct = signal;
    detector.fill({1.0F, 1.0F});
    filtered.process_with_sidechain(signal, detector, {block * signal.size(), 1});
    plain.process_with_sidechain(direct, detector, {block * direct.size(), 1});
  }
  REQUIRE_GT(signal.back().left, 0.095F, "sidechain high-pass failed to reject a DC trigger");
  REQUIRE_LT(direct.back().left, 0.01F, "sidechain high-pass failed to reject a DC trigger");

  filtered.reset_after_quiesce();
  for (std::size_t block = 0; block < 100; ++block) {
    signal.fill({0.1F, -0.1F});
    for (std::size_t frame = 0; frame < detector.size(); ++frame)
      detector[frame] = {(frame % 2 == 0) ? 1.0F : -1.0F, 0.0F};
    filtered.process_with_sidechain(signal, detector, {block * signal.size(), 2});
  }
  REQUIRE_LT(signal.back().left, 0.01F, "sidechain high-pass removed a fast trigger as well as DC");

  counted_allocations = 0;
  counting_allocations = true;
  for (std::size_t block = 0; block < 100; ++block)
    filtered.process_with_sidechain(signal, detector, {block * signal.size(), 2});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "filtered sidechain allocated in the callback");
}

void concurrent_parameter_mailbox_applies_latest_revision() {
  constexpr std::uint64_t last_revision = 1'000;
  const auto first = prepare({-24.0, 4.0, 0.0, 10.0, 100.0, 0.0});
  const auto second = prepare({-24.0, 4.0, 0.0, 10.0, 100.0, 6.0});
  CompressorRuntime runtime(first);
  std::jthread producer([&](const std::stop_token& stop) {
    for (std::uint64_t revision = 1; revision <= last_revision; ++revision) {
      const auto prepared = revision % 2 == 0 ? first : second;
      while (!runtime.offer_prepared(prepared, revision)) {
        if (stop.stop_requested())
          return;
        std::this_thread::yield();
      }
    }
  });
  std::array<engine::StereoFrame, 64> frames{};
  std::uint64_t observed = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (observed < last_revision && std::chrono::steady_clock::now() < deadline) {
    frames.fill({0.25F, -0.25F});
    runtime.process(frames, {0, 1});
    const auto active = runtime.applied_prepared_revision();
    REQUIRE_GE(active, observed,
               "concurrent compressor acknowledgement regressed or escaped its offered range");
    REQUIRE_LE(active, last_revision,
               "concurrent compressor acknowledgement regressed or escaped its offered range");
    observed = active;
    std::this_thread::yield();
  }
  producer.request_stop();
  producer.join();
  REQUIRE_EQ(observed, last_revision,
             "concurrent compressor mailbox failed to apply the final revision");
  runtime.reset_after_quiesce();
  REQUIRE_EQ(runtime.applied_prepared_revision(), 0,
             "quiescent compressor reset retained an applied revision");
}

void live_gain_transition_is_bounded_and_continuous() {
  const auto neutral = prepare({-18.0, 1.0, 0.0, 10.0, 100.0, 0.0});
  const auto quieter = prepare({-18.0, 1.0, 0.0, 10.0, 100.0, -12.0});
  const auto louder = prepare({-18.0, 1.0, 0.0, 10.0, 100.0, 6.0});
  CompressorRuntime runtime(neutral);
  std::array<engine::StereoFrame, 32> block{};
  const double quiet_gain = std::pow(10.0, -12.0 / 20.0);
  const double loud_gain = std::pow(10.0, 6.0 / 20.0);
  require(runtime.offer_prepared(quieter, 1), "first live setting rejected");
  counted_allocations = 0;
  counting_allocations = true;
  block.fill({0.25F, -0.25F});
  runtime.process(block, {0, 1});
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "live compressor transition allocated in callback");
  REQUIRE_LT(std::abs(block[0].left - 0.25 * (1.0 + (quiet_gain - 1.0) / 64.0)),
             tolerance::float_round_trip,
             "first live block did not follow the independent linear gain oracle");
  REQUIRE_LT(std::abs(block.back().left - 0.25 * (1.0 + (quiet_gain - 1.0) / 2.0)),
             tolerance::float_round_trip,
             "first live block did not follow the independent linear gain oracle");
  const float first_end = block.back().left;
  require(runtime.offer_prepared(louder, 2), "queued live setting rejected");
  block.fill({0.25F, -0.25F});
  runtime.process(block, {32, 1});
  REQUIRE_EQ(runtime.applied_prepared_revision(), 1,
             "second setting interrupted the prior gain transition");
  REQUIRE_LT(block[0].left, first_end, "second setting interrupted the prior gain transition");
  REQUIRE_LT(std::abs(block.back().left - 0.25 * quiet_gain), tolerance::float_round_trip,
             "second setting interrupted the prior gain transition");
  const float quiet_end = block.back().left;
  block.fill({0.25F, -0.25F});
  runtime.process(block, {64, 1});
  REQUIRE_EQ(runtime.applied_prepared_revision(), 2,
             "queued setting did not begin from the completed prior gain");
  REQUIRE_GT(block[0].left, quiet_end,
             "queued setting did not begin from the completed prior gain");
  REQUIRE_LT(std::abs(block.back().left - 0.25 * (quiet_gain + (loud_gain - quiet_gain) / 2.0)),
             tolerance::float_round_trip,
             "queued setting did not begin from the completed prior gain");
  block.fill({0.25F, -0.25F});
  runtime.process(block, {96, 1});
  REQUIRE_LT(std::abs(block.back().left - 0.25 * loud_gain), tolerance::float_round_trip,
             "live compressor gain failed to reach the requested value");
}

void silence_nonfinite_and_allocation() {
  CompressorRuntime compressor(prepare());
  std::array<engine::StereoFrame, 128> block{};
  block.fill({0.0F, 0.0F});
  compressor.process(block, {0, 1});
  REQUIRE_EQ(block.back().left, 0.0F, "zero input produced signal");
  block[0] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  block[1] = {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
  compressor.process(block, {128, 1});
  REQUIRE_EQ(block[0].left, 0.0F, "non-finite or extreme input escaped guard");
  REQUIRE_EQ(block[0].right, 0.0F, "non-finite or extreme input escaped guard");
  REQUIRE_TRUE(std::isfinite(block[1].left), "non-finite or extreme input escaped guard");
  REQUIRE_TRUE(std::isfinite(block[1].right), "non-finite or extreme input escaped guard");
  REQUIRE_GT(compressor.guarded_frames_after_quiesce(), 0, "invalid frame not counted");
  block.fill({0.25F, -0.25F});
  counted_allocations = 0;
  counting_allocations = true;
  for (int iteration = 0; iteration < 1'000; ++iteration) {
    compressor.process(block, {static_cast<std::uint64_t>(iteration) * block.size(), 1});
    compressor.process_with_sidechain(block, block,
                                      {static_cast<std::uint64_t>(iteration) * block.size(), 1});
  }
  counting_allocations = false;
  REQUIRE_EQ(counted_allocations, 0, "callback processing allocated memory");
}

void block_size_invariance() {
  const auto prepared = prepare({-40.0, 8.0, 0.0, 0.1, 100.0, 0.0});
  CompressorRuntime small(prepared);
  CompressorRuntime large(prepared);
  std::array<engine::StereoFrame, 256> small_frames{};
  for (std::size_t index = 0; index < small_frames.size(); ++index)
    small_frames[index] = {index >= 63 && index < 129 ? 0.6F : 0.1F, index >= 127 ? -0.2F : -0.05F};
  auto large_frames = small_frames;
  for (std::size_t first = 0; first < small_frames.size(); first += 64)
    small.process(std::span(small_frames).subspan(first, 64), {first, 1});
  for (std::size_t first = 0; first < large_frames.size(); first += 128)
    large.process(std::span(large_frames).subspan(first, 128), {first, 1});
  for (std::size_t index = 0; index < small_frames.size(); ++index)
    require(small_frames[index].left == large_frames[index].left &&
                small_frames[index].right == large_frames[index].right,
            "compressor changed output when live block size changed from 64 to 128");
}

void optimized_gain_matches_db_oracle() {
  // This reference deliberately evaluates the original dB-domain transfer
  // function, including both knee boundaries, independently of the callback.
  for (const CompressorSettings settings : {
           CompressorSettings{-18.0, 4.0, 6.0, 10.0, 100.0, 1.5},
           CompressorSettings{-30.0, 8.0, 0.0, 0.1, 40.0, -2.0},
           CompressorSettings{-12.0, 1.0, 12.0, 20.0, 200.0, 6.0},
       }) {
    CompressorRuntime compressor(prepare(settings));
    double envelope = 0.0;
    const double attack = std::exp(-1'000.0 / (settings.attack_ms * 48'000.0));
    const double release = std::exp(-1'000.0 / (settings.release_ms * 48'000.0));
    const double makeup = oscillate::native_dsp::dsp::db_to_gain(settings.makeup_db);
    for (std::size_t first = 0; first < 1'024; first += 64) {
      std::array<engine::StereoFrame, 64> signal{};
      std::array<engine::StereoFrame, 64> sidechain{};
      std::array<double, 64> expected_left{};
      for (std::size_t index = 0; index < signal.size(); ++index) {
        const std::size_t sample = first + index;
        const float amplitude = sample < 256   ? 0.005F
                                : sample < 512 ? 0.16F
                                : sample < 768 ? 0.85F
                                               : 0.02F;
        sidechain[index] = {amplitude, -0.5F * amplitude};
        signal[index] = {0.25F, -0.125F};
      }
      for (std::size_t index = 0; index < signal.size(); ++index) {
        const double detector = std::abs(static_cast<double>(sidechain[index].left));
        envelope = detector + (detector > envelope ? attack : release) * (envelope - detector);
        const double db = envelope > 0.0 ? 20.0 * std::log10(envelope) : -1'000.0;
        const double over = db - settings.threshold_db;
        const double half_knee = settings.knee_db * 0.5;
        const double slope = 1.0 - 1.0 / settings.ratio;
        double reduction = 0.0;
        if (over >= half_knee || settings.knee_db == 0.0) {
          reduction = slope * std::max(over, 0.0);
        } else if (over > -half_knee) {
          reduction = slope * (over + half_knee) * (over + half_knee) / (2.0 * settings.knee_db);
        }
        const double expected = 0.25 * makeup * std::pow(10.0, -reduction / 20.0);
        expected_left[index] = expected;
      }
      compressor.process_with_sidechain(signal, sidechain, {first, 1});
      for (std::size_t index = 0; index < signal.size(); ++index) {
        REQUIRE_LT(std::abs(static_cast<double>(signal[index].left) - expected_left[index]), 2e-6,
                   "optimized compressor gain diverged from dB-domain oracle");
        REQUIRE_LT(std::abs(static_cast<double>(signal[index].right) + expected_left[index] * 0.5),
                   2e-6, "optimized compressor changed stereo-link ratio");
      }
    }
  }
}

// Independent dB-domain oracle for the published meter: a linked peak
// envelope and the documented transfer curve, never the callback's
// linear-domain shortcut. Returns the reduction magnitude in dB.
struct ReductionOracle {
  CompressorSettings settings;
  double attack = std::exp(-1'000.0 / (settings.attack_ms * 48'000.0));
  double release = std::exp(-1'000.0 / (settings.release_ms * 48'000.0));
  double envelope = 0.0;

  [[nodiscard]] double curve_db(double level) const {
    if (level <= 0.0)
      return 0.0;
    const double over = 20.0 * std::log10(level) - settings.threshold_db;
    const double half_knee = settings.knee_db * 0.5;
    const double slope = 1.0 - 1.0 / settings.ratio;
    if (over >= half_knee || settings.knee_db == 0.0)
      return slope * std::max(over, 0.0);
    if (over > -half_knee)
      return slope * (over + half_knee) * (over + half_knee) / (2.0 * settings.knee_db);
    return 0.0;
  }
  double step(const engine::StereoFrame& detector_frame) {
    const double detector = std::max(std::abs(static_cast<double>(detector_frame.left)),
                                     std::abs(static_cast<double>(detector_frame.right)));
    envelope = detector + (detector > envelope ? attack : release) * (envelope - detector);
    return curve_db(envelope);
  }
};

void gain_reduction_reading_matches_oracle_for_64_and_128_frame_blocks() {
  // Non-zero makeup proves the reading excludes it; the soft knee and the
  // rise/fall programme exercise attack, knee and release.
  const CompressorSettings settings{-18.0, 4.0, 6.0, 5.0, 60.0, 9.0};
  for (const std::size_t block_frames : {std::size_t{64}, std::size_t{128}}) {
    CompressorRuntime compressor(prepare(settings));
    ReductionOracle oracle{settings};
    std::vector<engine::StereoFrame> block(block_frames);
    bool saw_reduction = false;
    for (std::size_t first = 0; first < 16'384; first += block_frames) {
      double expected = 0.0;
      for (std::size_t index = 0; index < block_frames; ++index) {
        const std::size_t sample = first + index;
        const float amplitude = sample < 1'024   ? 0.02F
                                : sample < 3'072 ? 0.9F
                                : sample < 5'120 ? 0.2F
                                                 : 0.01F;
        block[index] = {amplitude, -0.5F * amplitude};
        expected = std::max(expected, oracle.step(block[index]));
      }
      compressor.process(block, {first, 1});
      const double reading = compressor.latest_gain_reduction_db();
      REQUIRE_LT(std::abs(reading - expected), tolerance::near_silence,
                 "published gain reduction differs from the dB-domain oracle");
      saw_reduction = saw_reduction || expected > 10.0;
    }
    require(saw_reduction, "meter oracle programme never compressed");
    REQUIRE_LT(compressor.latest_gain_reduction_db(), 1e-3,
               "meter retained reduction after a quiet release");
  }
}

void gain_reduction_ratio_one_and_makeup_only_edits_read_zero() {
  CompressorRuntime ratio_one(prepare({-40.0, 1.0, 0.0, 0.1, 1.0, 12.0}));
  std::array<engine::StereoFrame, 64> block{};
  block.fill({1.0F, -1.0F});
  ratio_one.process(block, {0, 1});
  REQUIRE_EQ(ratio_one.latest_gain_reduction_db(), 0.0F,
             "ratio-one compressor reported gain reduction");

  // Settle 0 dBFS into -20 dBFS, 4:1 hard knee: 15 dB. A makeup-only live
  // edit changes output level across 64 frames but not the reduction.
  const CompressorSettings base{-20.0, 4.0, 0.0, 0.1, 1.0, 0.0};
  auto boosted = base;
  boosted.makeup_db = 12.0;
  CompressorRuntime compressor(prepare(base));
  for (std::uint64_t first = 0; first < 4'800; first += block.size()) {
    block.fill({1.0F, 1.0F});
    compressor.process(block, {first, 1});
  }
  REQUIRE_LT(std::abs(compressor.latest_gain_reduction_db() - 15.0F), tolerance::near_silence_f,
             "steady reduction differs from 15 dB hard-knee oracle");
  const float before = block.back().left;
  require(compressor.offer_prepared(prepare(boosted), 1), "makeup-only edit rejected");
  for (std::uint64_t first = 4'800; first < 4'800 + 3 * block.size(); first += block.size()) {
    block.fill({1.0F, 1.0F});
    compressor.process(block, {first, 1});
    REQUIRE_LT(std::abs(compressor.latest_gain_reduction_db() - 15.0F), tolerance::near_silence_f,
               "makeup-only transition changed the reduction reading");
  }
  REQUIRE_EQ(compressor.applied_prepared_revision(), 1,
             "makeup-only edit did not reach the output");
  REQUIRE_GT(block.back().left, 3.9F * before, "makeup-only edit did not reach the output");
}

void gain_reduction_follows_external_sidechain() {
  const CompressorSettings settings{-20.0, 4.0, 0.0, 0.1, 1.0, 0.0};
  CompressorRuntime compressor(prepare(settings));
  std::array<engine::StereoFrame, 128> signal{};
  std::array<engine::StereoFrame, 128> sidechain{};
  for (std::uint64_t first = 0; first < 4'800; first += signal.size()) {
    signal.fill({0.01F, 0.01F});
    sidechain.fill({0.0F, -1.0F});
    compressor.process_with_sidechain(signal, sidechain, {first, 1});
  }
  REQUIRE_LT(std::abs(compressor.latest_gain_reduction_db() - 15.0F), tolerance::near_silence_f,
             "quiet programme did not read reduction from a loud sidechain");
  for (std::uint64_t first = 4'800; first < 9'600; first += signal.size()) {
    signal.fill({1.0F, 1.0F});
    sidechain.fill({0.0F, 0.0F});
    compressor.process_with_sidechain(signal, sidechain, {first, 1});
  }
  REQUIRE_EQ(compressor.latest_gain_reduction_db(), 0.0F,
             "loud programme with a silent sidechain reported reduction");
}

void gain_reduction_tracks_live_setting_transition() {
  // Envelope settled at 1.0. Raising the threshold from -30 to -20 dBFS
  // (4:1, hard knee) crossfades the makeup-free gain over 64 frames, so the
  // greatest reduction is at the first frame: 1/64 of the way to 15 dB.
  const CompressorSettings deep{-30.0, 4.0, 0.0, 0.1, 1.0, 3.0};
  const CompressorSettings shallow{-20.0, 4.0, 0.0, 0.1, 1.0, -3.0};
  const double deep_gain = std::pow(10.0, -22.5 / 20.0);
  const double shallow_gain = std::pow(10.0, -15.0 / 20.0);
  const double first_frame_db = -20.0 * std::log10(deep_gain + (shallow_gain - deep_gain) / 64.0);
  for (const std::size_t block_frames : {std::size_t{64}, std::size_t{128}}) {
    CompressorRuntime compressor(prepare(deep));
    std::vector<engine::StereoFrame> block(block_frames);
    std::uint64_t first = 0;
    for (; first < 4'800; first += block_frames) {
      std::ranges::fill(block, engine::StereoFrame{1.0F, 1.0F});
      compressor.process(block, {first, 1});
    }
    REQUIRE_LT(std::abs(compressor.latest_gain_reduction_db() - 22.5F), tolerance::near_silence_f,
               "deep setting reduction differs from oracle");
    require(compressor.offer_prepared(prepare(shallow), 1), "live threshold edit rejected");
    counted_allocations = 0;
    counting_allocations = true;
    std::ranges::fill(block, engine::StereoFrame{1.0F, 1.0F});
    compressor.process(block, {first, 1});
    const float transition_reading = compressor.latest_gain_reduction_db();
    std::ranges::fill(block, engine::StereoFrame{1.0F, 1.0F});
    compressor.process(block, {first + block_frames, 1});
    const float settled_reading = compressor.latest_gain_reduction_db();
    counting_allocations = false;
    REQUIRE_EQ(counted_allocations, 0, "metered live transition allocated in the callback");
    REQUIRE_LT(std::abs(transition_reading - first_frame_db), tolerance::near_silence,
               "transition block reading differs from the crossfade oracle");
    REQUIRE_LT(std::abs(settled_reading - 15.0F), tolerance::near_silence_f,
               "post-transition reading differs from the new setting");
  }
}

void gain_reduction_guarded_empty_and_reset_blocks_read_zero() {
  CompressorRuntime compressor(prepare({-20.0, 4.0, 0.0, 0.1, 1.0, 0.0}));
  std::array<engine::StereoFrame, 64> block{};
  const auto settle = [&](std::uint64_t generation) {
    for (std::uint64_t first = 0; first < 4'800; first += block.size()) {
      block.fill({1.0F, 1.0F});
      compressor.process(block, {first, generation});
    }
    REQUIRE_LT(std::abs(compressor.latest_gain_reduction_db() - 15.0F), tolerance::near_silence_f,
               "guard fixture did not settle to 15 dB");
  };
  settle(2);
  compressor.process(std::span<engine::StereoFrame>{}, {4'800, 2});
  REQUIRE_EQ(compressor.latest_gain_reduction_db(), 0.0F, "empty block kept a stale reading");

  settle(2);
  block.fill({1.0F, 1.0F});
  compressor.process(block, {0, 1});
  REQUIRE_EQ(compressor.latest_gain_reduction_db(), 0.0F,
             "stale-generation guard published reduction");

  settle(2);
  block.fill({1.0F, 1.0F});
  compressor.process_with_sidechain(block, std::span(block).first(32), {0, 2});
  REQUIRE_EQ(compressor.latest_gain_reduction_db(), 0.0F,
             "mismatched-sidechain guard published reduction");

  // A non-finite final frame is zeroed by the output guard, but earlier
  // frames carried the full curve gain, which remains the block maximum.
  settle(2);
  block.fill({1.0F, 1.0F});
  block.back() = {std::numeric_limits<float>::quiet_NaN(), 1.0F};
  compressor.process(block, {4'800, 2});
  REQUIRE_LT(std::abs(compressor.latest_gain_reduction_db() - 15.0F), tolerance::near_silence_f,
             "non-finite frame disturbed the applied reduction reading");
  block.fill({std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()});
  compressor.process(block, {4'864, 2});
  const float invalid_reading = compressor.latest_gain_reduction_db();
  REQUIRE_TRUE(std::isfinite(invalid_reading),
               "all-invalid block published a non-finite or rising reduction");
  REQUIRE_GE(invalid_reading, 0.0F, "all-invalid block published a non-finite or rising reduction");
  REQUIRE_LT(invalid_reading, 15.0F,
             "all-invalid block published a non-finite or rising reduction");

  settle(2);
  compressor.reset_after_quiesce();
  REQUIRE_EQ(compressor.latest_gain_reduction_db(), 0.0F, "quiescent reset kept a reading");
}

void gain_reduction_reads_safely_across_threads() {
  // A reader polls while the callback alternates loud and silent sections.
  // Each value must be a complete published float within the curve's range.
  CompressorRuntime compressor(prepare({-20.0, 4.0, 0.0, 0.1, 5.0, 6.0}));
  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> nonzero_reads{0};
  std::atomic<bool> invalid{false};
  std::jthread reader([&] {
    while (running.load(std::memory_order_acquire)) {
      const float reading = compressor.latest_gain_reduction_db();
      if (!std::isfinite(reading) || reading < 0.0F || reading > 15.0001F)
        invalid.store(true, std::memory_order_relaxed);
      if (reading > 0.0F)
        nonzero_reads.fetch_add(1, std::memory_order_relaxed);
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });
  std::array<engine::StereoFrame, 64> block{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::uint64_t first = 0;
  while ((reads.load(std::memory_order_relaxed) < 10'000 ||
          nonzero_reads.load(std::memory_order_relaxed) == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    block.fill((first / 4'096) % 2 == 0 ? engine::StereoFrame{1.0F, -1.0F} : engine::StereoFrame{});
    compressor.process(block, {first, 1});
    first += block.size();
  }
  running.store(false, std::memory_order_release);
  reader.join();
  require(!invalid.load(), "cross-thread reader observed an invalid reduction");
  REQUIRE_GE(reads.load(), 10'000, "cross-thread reader did not observe callback publications");
  REQUIRE_GT(nonzero_reads.load(), 0, "cross-thread reader did not observe callback publications");
}

} // namespace

int main() {
  try {
    validation();
    steady_state_transfer();
    knee_and_step_oracle();
    sidechain_and_transport();
    filtered_sidechain_rejects_dc_but_keeps_fast_transients();
    concurrent_parameter_mailbox_applies_latest_revision();
    live_gain_transition_is_bounded_and_continuous();
    silence_nonfinite_and_allocation();
    block_size_invariance();
    optimized_gain_matches_db_oracle();
    gain_reduction_reading_matches_oracle_for_64_and_128_frame_blocks();
    gain_reduction_ratio_one_and_makeup_only_edits_read_zero();
    gain_reduction_follows_external_sidechain();
    gain_reduction_tracks_live_setting_transition();
    gain_reduction_guarded_empty_and_reset_blocks_read_zero();
    gain_reduction_reads_safely_across_threads();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

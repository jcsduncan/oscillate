#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
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

static_assert(noexcept(std::declval<EqRuntime&>().process(
    std::declval<std::span<engine::StereoFrame>>(), engine::BlockContext{})));
static_assert(std::is_base_of_v<engine::ProcessorRuntime, EqRuntime>);

using oscillate::test::require;

PreparedEq take(foundation::Result<PreparedEq> result) {
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result).value();
}

EqRuntime runtime(EqBandSettings settings) {
  return EqRuntime(take(prepare_eq(std::array{settings}, 48'000)));
}

double output_gain_db(EqRuntime& filter, double frequency_hz) {
  // A long driven sine is an independent transfer-function oracle. Discard the
  // initial settling period, then compare RMS gain at the requested frequency.
  constexpr std::size_t total_frames = 48'000;
  constexpr std::size_t warmup_frames = 24'000;
  std::array<engine::StereoFrame, 128> block{};
  double input_energy = 0.0;
  double output_energy = 0.0;
  for (std::size_t start = 0; start < total_frames; start += block.size()) {
    const auto count = std::min(block.size(), total_frames - start);
    for (std::size_t i = 0; i < count; ++i) {
      const double phase =
          2.0 * std::numbers::pi * frequency_hz * static_cast<double>(start + i) / 48'000.0;
      const float sample = static_cast<float>(0.1 * std::sin(phase));
      block[i] = {sample, sample};
      if (start + i >= warmup_frames) {
        input_energy += static_cast<double>(sample) * sample;
      }
    }
    filter.process(std::span(block).first(count), {start, 1});
    for (std::size_t i = 0; i < count; ++i) {
      if (start + i >= warmup_frames) {
        output_energy += static_cast<double>(block[i].left) * block[i].left;
        REQUIRE_NEAR(block[i].right, block[i].left, tolerance::float_round_trip_f,
                     "stereo channels differed for matching input");
      }
    }
  }
  return 10.0 * std::log10(output_energy / input_energy);
}

void preparation_rejects_bad_controls() {
  REQUIRE_TRUE(!prepare_eq({}, 0), "unsupported sample rate accepted");
  REQUIRE_TRUE(!prepare_eq({}, 44'000), "unsupported sample rate accepted");
  REQUIRE_TRUE(!prepare_eq({}, 192'001), "unsupported sample rate accepted");
  const std::array<EqBandSettings, eq_section_limit + 1> too_many{};
  require(!prepare_eq(too_many, 48'000), "overlarge band set accepted");
  const EqBandSettings normal{EqBandKind::peaking, 1'000.0, 1.0, 6.0};
  std::array<EqBandSettings, 1> setting{normal};
  auto invalid = [&] { return !prepare_eq(setting, 48'000); };
  setting[0].kind = std::bit_cast<EqBandKind>(std::uint8_t{255});
  require(invalid(), "unknown filter kind accepted");
  setting[0] = normal;
  for (const double bad : {-1.0, 0.0, 9.0, 22'000.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
    setting[0].frequency_hz = bad;
    require(invalid(), "invalid frequency accepted");
  }
  setting[0] = normal;
  for (const double bad : {-1.0, 0.0, 0.099, 10.01, std::numeric_limits<double>::quiet_NaN()}) {
    setting[0].shape = bad;
    require(invalid(), "invalid Q accepted");
  }
  setting[0] = normal;
  for (const double bad : {-24.01, 24.01, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
    setting[0].gain_db = bad;
    require(invalid(), "invalid gain accepted");
  }
  setting[0] = {EqBandKind::low_pass, 1'000.0, 0.707, 1.0};
  require(invalid(), "gain on passive filter accepted");
  setting[0] = {EqBandKind::high_shelf, 1'000.0, 1.01, 6.0};
  require(invalid(), "unstable shelf slope accepted");
  setting[0] = {EqBandKind::low_shelf, 1'000.0, 0.5, 6.0};
  require(!invalid(), "valid shelf rejected");
  REQUIRE_EQ(take(prepare_eq({}, 48'000)).band_count(), 0, "empty bypass not prepared");
}

void bounded_tail_contract() {
  const auto bypass = take(prepare_eq({}, 48'000));
  const auto zero = estimate_eq_tail_frames(bypass);
  REQUIRE_TRUE(zero, "bypassed EQ acquired an export tail");
  REQUIRE_EQ(zero.value(), 0, "bypassed EQ acquired an export tail");
  const auto low_pass =
      take(prepare_eq(std::array{EqBandSettings{EqBandKind::low_pass, 12'000.0,
                                                oscillate::native_dsp::dsp::butterworth_q, 0.0}},
                      48'000));
  const auto finite = estimate_eq_tail_frames(low_pass);
  // The analytic pole radius is sqrt(3 - 2*sqrt(2)) = sqrt(2)-1;
  // ceil(-21/log(radius))*2 + 256 = 304 frames.
  require(finite && finite.value() == 304, "EQ pole tail bound changed");
  const auto near_limit =
      take(prepare_eq(std::array{EqBandSettings{EqBandKind::low_pass, 10.0, 10.0, 0.0}}, 48'000));
  const auto over_budget = estimate_eq_tail_frames(near_limit);
  REQUIRE_TRUE(!over_budget, "EQ tail beyond ten seconds was not rejected");
  REQUIRE_EQ(over_budget.error().code, foundation::ErrorCode::resource_exhausted,
             "EQ tail beyond ten seconds was not rejected");
}

void block_size_invariance() {
  const auto prepared = take(
      prepare_eq(std::array{EqBandSettings{EqBandKind::low_pass, 1'000.0, 0.707, 0.0}}, 48'000));
  EqRuntime small(prepared);
  EqRuntime large(prepared);
  std::array<engine::StereoFrame, 256> small_frames{};
  for (std::size_t index = 0; index < small_frames.size(); ++index)
    small_frames[index] = {index == 63 || index == 127 ? 0.75F : 0.0F, index == 127 ? -0.5F : 0.0F};
  auto large_frames = small_frames;
  for (std::size_t first = 0; first < small_frames.size(); first += 64)
    small.process(std::span(small_frames).subspan(first, 64), {first, 1});
  for (std::size_t first = 0; first < large_frames.size(); first += 128)
    large.process(std::span(large_frames).subspan(first, 128), {first, 1});
  require(std::ranges::equal(
              small_frames, large_frames,
              [](const auto& a, const auto& b) { return a.left == b.left && a.right == b.right; }),
          "EQ changed output when live block size changed from 64 to 128");
}

void impulse_and_state_continuity() {
  // Butterworth low pass at Fs/4: b = [0.2928932, 0.5857864, 0.2928932],
  // a = [1, 0, 0.1715729]. These numbers are analytic, not read from the DUT.
  auto filter =
      runtime({EqBandKind::low_pass, 12'000.0, oscillate::native_dsp::dsp::butterworth_q, 0.0});
  std::array<engine::StereoFrame, 2> first{{{1.0F, -1.0F}, {0.0F, 0.0F}}};
  filter.process(first, {0, 1});
  std::array<engine::StereoFrame, 2> second{};
  filter.process(second, {2, 1});
  const double b0 = 1.0 - std::numbers::sqrt2 / 2.0;
  const double a2 = 3.0 - 2.0 * std::numbers::sqrt2;
  REQUIRE_LT(std::abs(first[0].left - b0), tolerance::float_round_trip,
             "analytic impulse or cross-block state oracle failed");
  REQUIRE_LT(std::abs(first[1].left - 2.0 * b0), tolerance::float_round_trip,
             "analytic impulse or cross-block state oracle failed");
  REQUIRE_LT(std::abs(second[0].left - b0 * (1.0 - a2)), tolerance::float_round_trip,
             "analytic impulse or cross-block state oracle failed");
  REQUIRE_LT(std::abs(second[1].left + 2.0 * b0 * a2), tolerance::float_round_trip,
             "analytic impulse or cross-block state oracle failed");
  REQUIRE_LT(std::abs(first[0].right + b0), tolerance::float_round_trip,
             "analytic impulse or cross-block state oracle failed");
  std::array<engine::StereoFrame, 1> after_seek{};
  filter.process(after_seek, {10, 2});
  REQUIRE_EQ(after_seek[0].left, 0.0F, "seek generation retained old EQ tail");
  REQUIRE_EQ(after_seek[0].right, 0.0F, "seek generation retained old EQ tail");
  std::array<engine::StereoFrame, 1> stale{{{1.0F, 1.0F}}};
  filter.process(stale, {11, 1});
  REQUIRE_EQ(stale[0].left, 0.0F, "stale generation was rendered");
  REQUIRE_EQ(stale[0].right, 0.0F, "stale generation was rendered");
  filter.reset_after_quiesce();
  std::array<engine::StereoFrame, 1> reset{{{1.0F, 1.0F}}};
  filter.process(reset, {0, 2});
  REQUIRE_LT(std::abs(reset[0].left - b0), tolerance::float_round_trip,
             "quiescent reset retained old filter history");
}

void frequency_gain_oracles() {
  auto peak = runtime({EqBandKind::peaking, 1'000.0, 1.0, 12.0});
  REQUIRE_LT(std::abs(output_gain_db(peak, 1'000.0) - 12.0), 0.08,
             "peaking centre gain is not +12 dB");
  auto cut = runtime({EqBandKind::peaking, 1'000.0, 1.0, -12.0});
  REQUIRE_LT(std::abs(output_gain_db(cut, 1'000.0) + 12.0), 0.08,
             "peaking centre cut is not -12 dB");
  auto low_pass =
      runtime({EqBandKind::low_pass, 2'000.0, oscillate::native_dsp::dsp::butterworth_q, 0.0});
  REQUIRE_LT(std::abs(output_gain_db(low_pass, 2'000.0) + 3.0103), 0.08,
             "Butterworth low-pass cutoff is not -3 dB");
  auto high_pass =
      runtime({EqBandKind::high_pass, 2'000.0, oscillate::native_dsp::dsp::butterworth_q, 0.0});
  REQUIRE_LT(std::abs(output_gain_db(high_pass, 2'000.0) + 3.0103), 0.08,
             "Butterworth high-pass cutoff is not -3 dB");
  auto low_shelf = runtime({EqBandKind::low_shelf, 1'000.0, 1.0, 6.0});
  REQUIRE_LT(std::abs(output_gain_db(low_shelf, 40.0) - 6.0), 0.1,
             "low shelf asymptotes incorrect");
  REQUIRE_LT(std::abs(output_gain_db(low_shelf, 12'000.0)), 0.1, "low shelf asymptotes incorrect");
  auto high_shelf = runtime({EqBandKind::high_shelf, 1'000.0, 1.0, 6.0});
  REQUIRE_LT(std::abs(output_gain_db(high_shelf, 40.0)), 0.1, "high shelf asymptotes incorrect");
  REQUIRE_LT(std::abs(output_gain_db(high_shelf, 12'000.0) - 6.0), 0.1,
             "high shelf asymptotes incorrect");
}

// A steep pass filter is Butterworth stages: -3 dB at the corner, flat
// above it, 12 dB an octave per stage beyond it; q shapes the last stage.
void pass_stages_are_butterworth() {
  std::array<EqBandSettings, eq_max_pass_stages> stages{};
  REQUIRE_EQ(eq_pass_stages(EqBandKind::high_pass, 1'000.0, 3.0, 1, stages), 1,
             "one stage is the band itself");
  REQUIRE_EQ(stages[0].shape, 3.0, "one stage is the band itself");
  REQUIRE_EQ(stages[0].kind, EqBandKind::high_pass, "one stage is the band itself");
  for (const std::size_t count : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    for (const auto kind : {EqBandKind::high_pass, EqBandKind::low_pass}) {
      const auto made =
          eq_pass_stages(kind, 1'000.0, oscillate::native_dsp::dsp::butterworth_q, count, stages);
      REQUIRE_EQ(made, count, "a slope's stage count");
      const auto prepared =
          take(prepare_eq(std::span<const EqBandSettings>(stages).first(made), 48'000));
      const auto label = std::to_string(12 * count) + " dB/oct";
      REQUIRE_LT(std::abs(*eq_magnitude_db(prepared, 1'000.0) + 3.0103),
                 tolerance::float_round_trip, "the corner is -3 dB at " + label);
      const double order = 2.0 * static_cast<double>(count);
      const double beyond = -10.0 * std::log10(1.0 + std::pow(2.0, 2.0 * order));
      const double octave = kind == EqBandKind::high_pass ? 500.0 : 2'000.0;
      // The analog figure; the bilinear transform steepens a stage slightly.
      REQUIRE_LT(std::abs(*eq_magnitude_db(prepared, octave) - beyond),
                 0.1 * static_cast<double>(count),
                 "an octave past the corner falls by the slope at " + label + ": " +
                     std::to_string(*eq_magnitude_db(prepared, octave)));
      // The sweep grid is the test input.
      // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
      for (double hz = 20.0; hz < 20'000.0; hz *= 1.01) {
        REQUIRE_LT(*eq_magnitude_db(prepared, hz), 1e-6, "maximally flat: no bump at " + label);
      }
    }
  }
  // The slope renders as designed: 48 dB an octave is about -48 dB there.
  const auto made = eq_pass_stages(EqBandKind::high_pass, 1'000.0,
                                   oscillate::native_dsp::dsp::butterworth_q, 4, stages);
  EqRuntime steep(take(prepare_eq(std::span<const EqBandSettings>(stages).first(made), 48'000)));
  REQUIRE_LT(std::abs(output_gain_db(steep, 500.0) + 48.16), 0.5, "a rendered 48 dB slope");
  // q scales the most resonant stage: a peak at the corner, held in range.
  (void)eq_pass_stages(EqBandKind::high_pass, 1'000.0, 2.0, 2, stages);
  REQUIRE_LT(std::abs(stages[1].shape -
                      1.3065629648763766 * 2.0 / oscillate::native_dsp::dsp::butterworth_q),
             tolerance::double_accumulation, "Butterworth stage Qs with the last scaled by q");
  REQUIRE_LT(std::abs(stages[0].shape - 0.5411961001461970), tolerance::double_accumulation,
             "Butterworth stage Qs with the last scaled by q");
  (void)eq_pass_stages(EqBandKind::high_pass, 1'000.0, 10.0, 8, stages);
  REQUIRE_EQ(stages[7].shape, eq_pass_section_q_limit, "a resonant steep corner is held in range");
  REQUIRE_OK(prepare_eq(std::span<const EqBandSettings>(stages).first(8), 48'000), "and prepares");
}

void display_response_matches_rendered_filter() {
  const std::array bands{EqBandSettings{EqBandKind::peaking, 1'000.0, 1.0, 6.0},
                         EqBandSettings{EqBandKind::low_shelf, 180.0, 0.7, 3.0}};
  const auto prepared = take(prepare_eq(bands, 48'000));
  for (const double frequency : {40.0, 1'000.0, 12'000.0}) {
    const auto displayed = eq_magnitude_db(prepared, frequency);
    auto reference = EqRuntime(prepared);
    REQUIRE_TRUE(displayed, "EQ display response disagreed with its audible cascade");
    REQUIRE_LT(std::abs(*displayed - output_gain_db(reference, frequency)), 0.08,
               "EQ display response disagreed with its audible cascade");
  }
  const auto bypass = take(prepare_eq({}, 48'000));
  REQUIRE_EQ(eq_magnitude_db(bypass, 20.0), 0.0,
             "EQ display response accepted an invalid frequency or changed bypass gain");
  REQUIRE_TRUE(!eq_magnitude_db(prepared, -1.0),
               "EQ display response accepted an invalid frequency or changed bypass gain");
  REQUIRE_TRUE(!eq_magnitude_db(prepared, 24'001.0),
               "EQ display response accepted an invalid frequency or changed bypass gain");
  REQUIRE_TRUE(!eq_magnitude_db(prepared, std::numeric_limits<double>::quiet_NaN()),
               "EQ display response accepted an invalid frequency or changed bypass gain");
}

void bypass_multiband_and_guards() {
  EqRuntime bypass(take(prepare_eq({}, 48'000)));
  std::array<engine::StereoFrame, 2> dry{{{0.25F, -0.25F}, {1e-35F, -1e-35F}}};
  bypass.process(dry, {0, 1});
  REQUIRE_EQ(dry[0].left, 0.25F, "empty EQ changed finite bypass samples");
  REQUIRE_EQ(dry[0].right, -0.25F, "empty EQ changed finite bypass samples");
  REQUIRE_EQ(dry[1].left, 1e-35F, "empty EQ changed finite bypass samples");
  REQUIRE_EQ(dry[1].right, -1e-35F, "empty EQ changed finite bypass samples");
  const std::array settings{EqBandSettings{EqBandKind::peaking, 1'000.0, 1.0, 6.0},
                            EqBandSettings{EqBandKind::peaking, 1'000.0, 1.0, 6.0}};
  auto stacked = EqRuntime(take(prepare_eq(settings, 48'000)));
  REQUIRE_LT(std::abs(output_gain_db(stacked, 1'000.0) - 12.0), 0.08,
             "two independent peaks did not cascade");
  auto filter = runtime({EqBandKind::low_pass, 1'000.0, 0.707, 0.0});
  std::array<engine::StereoFrame, 128> block{};
  block[0] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  counted_allocations = 0;
  counting_allocations = true;
  filter.process(block, {0, 1});
  const auto allocations = counted_allocations;
  counting_allocations = false;
  REQUIRE_EQ(allocations, 0, "callback allocated or failed to count corrupt input");
  REQUIRE_EQ(filter.guarded_frames_after_quiesce(), 1,
             "callback allocated or failed to count corrupt input");
  for (const auto& frame : block) {
    REQUIRE_TRUE(std::isfinite(frame.left), "nonfinite input poisoned subsequent filter state");
    REQUIRE_TRUE(std::isfinite(frame.right), "nonfinite input poisoned subsequent filter state");
  }
  filter.reset_after_quiesce();
  REQUIRE_EQ(filter.guarded_frames_after_quiesce(), 0, "quiescent reset retained diagnostics");

  std::array<EqBandSettings, eq_band_limit> maximum_bands{};
  maximum_bands.fill({EqBandKind::peaking, 1'000.0, 1.0, 24.0});
  auto extreme = EqRuntime(take(prepare_eq(maximum_bands, 96'000)));
  std::array<engine::StereoFrame, 1> peak{
      {{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}}};
  extreme.process(peak, {0, 1});
  REQUIRE_TRUE(std::isfinite(peak[0].left),
               "maximum-band full-scale signal did not saturate and report guard");
  REQUIRE_TRUE(std::isfinite(peak[0].right),
               "maximum-band full-scale signal did not saturate and report guard");
  REQUIRE_EQ(peak[0].left, std::numeric_limits<float>::max(),
             "maximum-band full-scale signal did not saturate and report guard");
  REQUIRE_EQ(peak[0].right, -std::numeric_limits<float>::max(),
             "maximum-band full-scale signal did not saturate and report guard");
  REQUIRE_EQ(extreme.guarded_frames_after_quiesce(), 1,
             "maximum-band full-scale signal did not saturate and report guard");
}

void block_boundary_mailbox_keeps_audio_and_history() {
  const auto original =
      take(prepare_eq(std::array{EqBandSettings{EqBandKind::low_pass, 800.0, 0.707, 0.0}}, 48'000));
  EqRuntime queued(original);
  EqRuntime unchanged(original);
  EqRuntime abrupt(original);
  EqRuntime partitioned(original);
  std::array<engine::StereoFrame, 64> first{};
  first[0] = {1.0F, -1.0F};
  auto expected_first = first;
  auto abrupt_first = first;
  auto partitioned_first = first;
  queued.process(first, {0, 1});
  unchanged.process(expected_first, {0, 1});
  abrupt.process(abrupt_first, {0, 1});
  partitioned.process(partitioned_first, {0, 1});
  const auto equal_frames = [](engine::StereoFrame left, engine::StereoFrame right) {
    return left.left == right.left && left.right == right.right;
  };
  require(std::ranges::equal(first, expected_first, equal_frames),
          "mailbox fixture began with different EQ history");

  for (std::uint64_t revision = 1; revision <= 4; ++revision) {
    const auto prepared = take(prepare_eq(
        std::array{EqBandSettings{EqBandKind::low_pass,
                                  800.0 + 200.0 * static_cast<double>(revision), 0.707, 0.0}},
        48'000));
    REQUIRE_TRUE(queued.offer_prepared(prepared, revision), "free EQ mailbox rejected a revision");
    REQUIRE_TRUE(partitioned.offer_prepared(prepared, revision),
                 "free EQ mailbox rejected a revision");
    if (revision == 4)
      abrupt.replace_prepared_while_held(prepared);
  }
  REQUIRE_TRUE(!queued.offer_prepared(original, 5),
               "full EQ mailbox accepted a fifth edit or reported it audible early");
  REQUIRE_EQ(queued.applied_prepared_revision(), 0,
             "full EQ mailbox accepted a fifth edit or reported it audible early");
  std::array<engine::StereoFrame, 64> next{};
  next[0] = {0.25F, -0.25F};
  auto old_next = next;
  auto abrupt_next = next;
  auto partitioned_next = next;
  counted_allocations = 0;
  counting_allocations = true;
  queued.process(next, {64, 1});
  const auto allocations = counted_allocations;
  counting_allocations = false;
  unchanged.process(old_next, {64, 1});
  abrupt.process(abrupt_next, {64, 1});
  for (std::size_t index = 0; index < partitioned_next.size(); ++index)
    partitioned.process(std::span(partitioned_next).subspan(index, 1), {64 + index, 1});
  REQUIRE_EQ(
      allocations, 0,
      "EQ mailbox transition allocated, depended on block size or jumped to the final setting");
  REQUIRE_TRUE(
      std::ranges::equal(next, partitioned_next, equal_frames),
      "EQ mailbox transition allocated, depended on block size or jumped to the final setting");
  REQUIRE_EQ(
      queued.applied_prepared_revision(), 4,
      "EQ mailbox transition allocated, depended on block size or jumped to the final setting");
  REQUIRE_LT(
      std::abs(next.front().left - old_next.front().left),
      std::abs(abrupt_next.front().left - old_next.front().left),
      "EQ mailbox transition allocated, depended on block size or jumped to the final setting");
  require(!queued.offer_prepared(original, 4), "stale EQ revision was accepted");
  REQUIRE_TRUE(!queued.offer_prepared(take(prepare_eq({}, 48'000)), 5),
               "EQ mailbox accepted an incompatible band count or sample rate");
  REQUIRE_TRUE(!queued.offer_prepared(take(prepare_eq({}, 44'100)), 5),
               "EQ mailbox accepted an incompatible band count or sample rate");
  require(queued.offer_prepared(original, 5), "consumed mailbox slots were not released");
  queued.reset_after_quiesce();
  REQUIRE_EQ(queued.applied_prepared_revision(), 0,
             "quiescent EQ reset retained a stale applied revision");
}

void concurrent_mailbox_revisions_are_monotonic() {
  constexpr std::uint64_t last_revision = 1'000;
  const auto initial = take(
      prepare_eq(std::array{EqBandSettings{EqBandKind::peaking, 1'000.0, 0.707, 0.0}}, 48'000));
  const auto changed = take(
      prepare_eq(std::array{EqBandSettings{EqBandKind::peaking, 1'000.0, 0.707, 6.0}}, 48'000));
  EqRuntime runtime(initial);
  std::jthread producer([&](const std::stop_token& stop) {
    for (std::uint64_t revision = 1; revision <= last_revision; ++revision) {
      const auto prepared = revision % 2 == 0 ? initial : changed;
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
    runtime.process(frames, {0, 1});
    const auto active = runtime.applied_prepared_revision();
    REQUIRE_GE(active, observed,
               "concurrent EQ mailbox acknowledgement regressed or escaped its offered range");
    REQUIRE_LE(active, last_revision,
               "concurrent EQ mailbox acknowledgement regressed or escaped its offered range");
    observed = active;
    std::this_thread::yield();
  }
  producer.request_stop();
  producer.join();
  REQUIRE_EQ(observed, last_revision, "concurrent EQ mailbox failed to apply the final revision");
}

void partial_transition_acknowledges_only_after_completion() {
  const auto low =
      take(prepare_eq(std::array{EqBandSettings{EqBandKind::low_pass, 300.0, 0.707, 0.0}}, 48'000));
  const auto high = take(
      prepare_eq(std::array{EqBandSettings{EqBandKind::low_pass, 9'000.0, 0.707, 0.0}}, 48'000));
  EqRuntime filter(low);
  require(filter.offer_prepared(high, 1), "transition fixture could not offer target");
  std::array<engine::StereoFrame, 32> first{};
  first.fill({0.25F, -0.25F});
  filter.process(first, {0, 1});
  REQUIRE_EQ(filter.applied_prepared_revision(), 0,
             "partly transitioned EQ setting was acknowledged as fully audible");
  std::array<engine::StereoFrame, 32> second{};
  second.fill({0.25F, -0.25F});
  filter.process(second, {32, 1});
  REQUIRE_EQ(filter.applied_prepared_revision(), 1,
             "EQ setting was not acknowledged on its final transition sample");
  REQUIRE_TRUE(std::ranges::all_of(first,
                                   [](const auto& frame) {
                                     return std::isfinite(frame.left) && std::isfinite(frame.right);
                                   }),
               "wide EQ transition produced a nonfinite sample");
  REQUIRE_TRUE(std::ranges::all_of(second,
                                   [](const auto& frame) {
                                     return std::isfinite(frame.left) && std::isfinite(frame.right);
                                   }),
               "wide EQ transition produced a nonfinite sample");

  EqRuntime superseded(low);
  require(superseded.offer_prepared(high, 1), "supersession fixture rejected first offer");
  first.fill({0.25F, -0.25F});
  superseded.process(first, {0, 1});
  REQUIRE_EQ(superseded.applied_prepared_revision(), 0,
             "an unfinished EQ transition could not be superseded");
  REQUIRE_TRUE(superseded.offer_prepared(low, 2),
               "an unfinished EQ transition could not be superseded");
  std::array<engine::StereoFrame, 64> replacement{};
  replacement.fill({0.25F, -0.25F});
  superseded.process(replacement, {32, 1});
  REQUIRE_EQ(superseded.applied_prepared_revision(), 2,
             "EQ supersession acknowledged an unfinished revision or lost stable output");
  REQUIRE_TRUE(std::ranges::all_of(replacement,
                                   [](const auto& frame) {
                                     return std::isfinite(frame.left) && std::isfinite(frame.right);
                                   }),
               "EQ supersession acknowledged an unfinished revision or lost stable output");
}

void structural_band_mailbox_preserves_survivors() {
  const EqBandSettings low{EqBandKind::low_pass, 650.0, 0.707, 0.0};
  const EqBandSettings neutral{EqBandKind::peaking, 1'000.0, 0.707, 0.0};
  const EqBandSettings high{EqBandKind::high_pass, 120.0, 0.707, 0.0};
  const auto initial = take(prepare_eq(std::array{low, neutral, high}, 48'000));
  EqRuntime changed(initial);
  EqRuntime reference(take(prepare_eq(std::array{low, high}, 48'000)));
  std::array<engine::StereoFrame, 64> first{}, expected{};
  first.back() = {1.0F, -1.0F};
  expected = first;
  changed.process(first, {0, 1});
  reference.process(expected, {0, 1});
  const auto after_removal = take(prepare_eq(std::array{low, high}, 48'000));
  REQUIRE_TRUE(changed.offer_band_change(after_removal, 1, 1),
               "structural mailbox accepted a second edit before acknowledgement");
  REQUIRE_TRUE(!changed.offer_band_change(after_removal, 0, 2),
               "structural mailbox accepted a second edit before acknowledgement");
  REQUIRE_TRUE(!changed.offer_prepared(initial, 2),
               "structural mailbox accepted a second edit before acknowledgement");
  std::array<engine::StereoFrame, 32> half{}, expected_half{};
  counted_allocations = 0;
  counting_allocations = true;
  changed.process(half, {64, 1});
  const auto allocations = counted_allocations;
  counting_allocations = false;
  reference.process(expected_half, {64, 1});
  REQUIRE_EQ(allocations, 0, "partial band removal allocated or became active early");
  REQUIRE_EQ(changed.applied_prepared_revision(), 0,
             "partial band removal allocated or became active early");
  half.fill({});
  expected_half.fill({});
  changed.process(half, {96, 1});
  reference.process(expected_half, {96, 1});
  REQUIRE_EQ(changed.applied_prepared_revision(), 1,
             "band removal did not acknowledge after 64 frames");
  std::array<engine::StereoFrame, 64> continued{}, reference_continued{};
  changed.process(continued, {128, 1});
  reference.process(reference_continued, {128, 1});
  for (std::size_t index = 0; index < continued.size(); ++index)
    require(continued[index].left == reference_continued[index].left &&
                continued[index].right == reference_continued[index].right,
            "removing middle band reset the two surviving filter histories");

  const auto appended = take(prepare_eq(std::array{low, high, neutral}, 48'000));
  require(changed.offer_band_change(appended, 2, 2), "append after removal was rejected");
  continued.fill({});
  counted_allocations = 0;
  counting_allocations = true;
  changed.process(continued, {192, 1});
  const auto add_allocations = counted_allocations;
  counting_allocations = false;
  REQUIRE_EQ(add_allocations, 0, "band append allocated or did not acknowledge at its final frame");
  REQUIRE_EQ(changed.applied_prepared_revision(), 2,
             "band append allocated or did not acknowledge at its final frame");
  require(!changed.offer_band_change(take(prepare_eq(std::array{low, high}, 48'000)), 3, 3),
          "invalid structural index was accepted");
}

void planned_loop_wrap_keeps_filter_history() {
  auto wrapped = runtime({EqBandKind::low_pass, 600.0, 0.707, 0.0});
  auto reference = runtime({EqBandKind::low_pass, 600.0, 0.707, 0.0});
  std::array<engine::StereoFrame, 64> before{}, reference_before{};
  before.back() = {1.0F, -1.0F};
  reference_before = before;
  wrapped.process(before, {100, 1});
  reference.process(reference_before, {100, 1});
  std::array<engine::StereoFrame, 64> after{}, reference_after{};
  wrapped.process(after, {0, 2, 1, true});
  reference.process(reference_after, {164, 1});
  REQUIRE_GT(std::fabs(after.front().left), 0.0001F,
             "loop fixture did not exercise retained EQ history");
  for (std::size_t frame = 0; frame < after.size(); ++frame)
    require(after[frame].left == reference_after[frame].left &&
                after[frame].right == reference_after[frame].right,
            "planned loop wrap reset the running EQ filter");
  auto fresh = runtime({EqBandKind::low_pass, 600.0, 0.707, 0.0});
  after.fill({});
  reference_after.fill({});
  wrapped.process(after, {200, 3});
  fresh.process(reference_after, {200, 3});
  for (std::size_t frame = 0; frame < after.size(); ++frame)
    require(after[frame].left == reference_after[frame].left &&
                after[frame].right == reference_after[frame].right,
            "ordinary seek retained EQ history");
}

} // namespace

int main() {
  try {
    preparation_rejects_bad_controls();
    bounded_tail_contract();
    block_size_invariance();
    impulse_and_state_continuity();
    frequency_gain_oracles();
    pass_stages_are_butterworth();
    display_response_matches_rendered_filter();
    bypass_multiband_and_guards();
    block_boundary_mailbox_keeps_audio_and_history();
    partial_transition_acknowledges_only_after_completion();
    structural_band_mailbox_preserves_survivors();
    concurrent_mailbox_revisions_are_monotonic();
    planned_loop_wrap_keeps_filter_history();
    std::cout << "native EQ contract passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

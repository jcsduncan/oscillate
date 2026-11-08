#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

namespace {
using oscillate::native_dsp::IntegratedLoudnessAnalyzer;
using oscillate::native_dsp::LoudnessLayout;
using oscillate::native_dsp::LoudnessStatus;

using oscillate::test::require;

double measured(const oscillate::native_dsp::LoudnessResult& result) {
  return result.integrated_lkfs.value_or(std::numeric_limits<double>::quiet_NaN());
}

// 997 Hz at 48 kHz is the independent reference specified by BS.1770-5
// Annex 1: full-scale sine on one front channel reads -3.01 LKFS.
std::vector<float> sine(std::size_t frames, LoudnessLayout layout, std::size_t channel,
                        double amplitude = 1.0,
                        std::uint32_t sample_rate = oscillate::test::sample_rate_48k,
                        double frequency = 997.0) {
  const std::size_t channels = layout == LoudnessLayout::mono     ? 1
                               : layout == LoudnessLayout::stereo ? 2
                                                                  : 6;
  std::vector<float> samples(frames * channels, 0.0F);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    samples[frame * channels + channel] =
        static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                static_cast<double>(frame) / sample_rate));
  }
  return samples;
}

void multirate_reference_levels() {
  // Reference values are from FFmpeg's independent ebur128 filter applied to
  // two seconds of full-scale mono float sine at the indicated physical rates.
  // Its display rounds to 0.1 LUFS, so leave margin for that quantization.
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    for (const auto [frequency, expected] : {std::pair{100.0, rate < 88'200 ? -4.8 : -4.9},
                                             std::pair{997.0, -3.0}, std::pair{10'000.0, 0.3}}) {
      IntegratedLoudnessAnalyzer meter(rate, LoudnessLayout::mono);
      const auto signal =
          sine(static_cast<std::size_t>(rate) * 2, LoudnessLayout::mono, 0, 1.0, rate, frequency);
      REQUIRE_EQ(meter.append(signal), LoudnessStatus::ok,
                 "rate-specific K weighting differs from the independent loudness reference");
      REQUIRE_LT(std::abs(measured(meter.finish()) - expected), 0.16,
                 "rate-specific K weighting differs from the independent loudness reference");
    }
    IntegratedLoudnessAnalyzer duration(rate, LoudnessLayout::mono);
    std::vector<float> silence(rate, 0.0F);
    REQUIRE_EQ(duration.append(silence), LoudnessStatus::ok,
               "one second did not produce seven 400 ms/100 ms windows");
    REQUIRE_EQ(duration.finish().total_blocks, 7,
               "one second did not produce seven 400 ms/100 ms windows");
    duration.reset();
    REQUIRE_EQ(
        duration.append(std::span(silence).first(static_cast<std::size_t>(rate) * 4 / 10 - 1)),
        LoudnessStatus::ok, "a short rate-specific window was measured");
    REQUIRE_EQ(duration.finish().total_blocks, 0, "a short rate-specific window was measured");
  }
  IntegratedLoudnessAnalyzer surround(96'000, LoudnessLayout::surround_5_1);
  const auto surround_signal = sine(96'000, LoudnessLayout::surround_5_1, 4, 1.0, 96'000);
  REQUIRE_EQ(surround.append(surround_signal), LoudnessStatus::ok,
             "96 kHz surround reference rejected");
  IntegratedLoudnessAnalyzer front(96'000, LoudnessLayout::mono);
  REQUIRE_EQ(front.append(sine(96'000, LoudnessLayout::mono, 0, 1.0, 96'000)), LoudnessStatus::ok,
             "multirate surround channel weighting changed");
  REQUIRE_LT(
      std::abs(measured(surround.finish()) - measured(front.finish()) - 10.0 * std::log10(1.41)),
      0.01, "multirate surround channel weighting changed");
}

void reference_levels_and_layouts() {
  constexpr std::size_t frames = std::size_t{48'000} * 2;
  IntegratedLoudnessAnalyzer mono(48'000, LoudnessLayout::mono);
  REQUIRE_EQ(mono.append(sine(frames, LoudnessLayout::mono, 0)), LoudnessStatus::ok,
             "mono reference rejected");
  const auto mono_result = mono.finish();
  REQUIRE_LT(std::abs(measured(mono_result) + 3.01), 0.10,
             "BS.1770 full-scale 997 Hz mono reference failed");

  IntegratedLoudnessAnalyzer stereo(48'000, LoudnessLayout::stereo);
  auto stereo_samples = sine(frames, LoudnessLayout::stereo, 0);
  const auto right = sine(frames, LoudnessLayout::stereo, 1);
  std::ranges::transform(stereo_samples, right, stereo_samples.begin(), std::plus<>{});
  REQUIRE_EQ(stereo.append(stereo_samples), LoudnessStatus::ok, "stereo reference rejected");
  REQUIRE_LT(std::abs(measured(stereo.finish()) - measured(mono_result) - 10.0 * std::log10(2.0)),
             0.01, "equal stereo channels did not add 3.01 LKFS");

  IntegratedLoudnessAnalyzer surround(48'000, LoudnessLayout::surround_5_1);
  REQUIRE_EQ(surround.append(sine(frames, LoudnessLayout::surround_5_1, 4)), LoudnessStatus::ok,
             "surround reference rejected");
  REQUIRE_LT(
      std::abs(measured(surround.finish()) - measured(mono_result) - 10.0 * std::log10(1.41)), 0.01,
      "surround channel weighting is wrong");

  IntegratedLoudnessAnalyzer lfe(48'000, LoudnessLayout::surround_5_1);
  REQUIRE_EQ(lfe.append(sine(frames, LoudnessLayout::surround_5_1, 3)), LoudnessStatus::ok,
             "LFE input rejected");
  REQUIRE_TRUE(!lfe.finish().integrated_lkfs, "LFE must not enter loudness summation");
  REQUIRE_EQ(lfe.finish().absolute_gated_blocks, 0, "LFE must not enter loudness summation");
}

void silence_short_input_and_gates() {
  IntegratedLoudnessAnalyzer meter(48'000, LoudnessLayout::mono);
  std::vector<float> silence(48'000, 0.0F);
  REQUIRE_EQ(meter.append(silence), LoudnessStatus::ok, "silence rejected");
  REQUIRE_TRUE(!meter.finish().integrated_lkfs,
               "silence must yield no measured loudness and seven complete windows");
  REQUIRE_EQ(meter.finish().total_blocks, 7,
             "silence must yield no measured loudness and seven complete windows");
  meter.reset();
  REQUIRE_EQ(meter.append(std::span(silence).first(19'199)), LoudnessStatus::ok,
             "short input rejected");
  REQUIRE_EQ(meter.finish().total_blocks, 0, "incomplete window was measured");
  REQUIRE_TRUE(!meter.finish().integrated_lkfs, "incomplete window was measured");

  meter.reset();
  const auto loud = sine(48'000, LoudnessLayout::mono, 0, 0.5);
  const auto very_quiet = sine(48'000, LoudnessLayout::mono, 0, 0.005);
  REQUIRE_EQ(meter.append(loud), LoudnessStatus::ok, "gating fixture rejected");
  REQUIRE_EQ(meter.append(very_quiet), LoudnessStatus::ok, "gating fixture rejected");
  const auto gated = meter.finish();
  IntegratedLoudnessAnalyzer reference(48'000, LoudnessLayout::mono);
  REQUIRE_EQ(reference.append(loud), LoudnessStatus::ok, "loud reference rejected");
  REQUIRE_LT(std::abs(measured(gated) - measured(reference.finish())), 1.5,
             "relative gate did not exclude quiet programme blocks");
  REQUIRE_LT(gated.relative_gated_blocks, gated.absolute_gated_blocks,
             "relative gate did not exclude quiet programme blocks");

  meter.reset();
  const auto below_gate = sine(48'000, LoudnessLayout::mono, 0, 0.00001);
  REQUIRE_EQ(meter.append(below_gate), LoudnessStatus::ok,
             "absolute gate did not suppress below -70 LKFS input");
  REQUIRE_TRUE(!meter.finish().integrated_lkfs,
               "absolute gate did not suppress below -70 LKFS input");
}

void momentary_and_short_term_windows() {
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    IntegratedLoudnessAnalyzer meter(rate, LoudnessLayout::mono);
    const auto first = sine(static_cast<std::size_t>(rate) * 2, LoudnessLayout::mono, 0, 1.0, rate);
    REQUIRE_EQ(meter.append(first), LoudnessStatus::ok, "momentary reference rejected");
    const auto two_seconds = meter.finish();
    REQUIRE_TRUE(two_seconds.momentary_lkfs,
                 "momentary value or incomplete short-term window is wrong");
    REQUIRE_LT(std::abs(*two_seconds.momentary_lkfs + 3.01), 0.12,
               "momentary value or incomplete short-term window is wrong");
    REQUIRE_TRUE(!two_seconds.short_term_lkfs,
                 "momentary value or incomplete short-term window is wrong");

    const auto third = sine(rate, LoudnessLayout::mono, 0, 1.0, rate);
    REQUIRE_EQ(meter.append(third), LoudnessStatus::ok, "short-term reference rejected");
    const auto three_seconds = meter.finish();
    REQUIRE_TRUE(three_seconds.short_term_lkfs, "complete three-second reference is wrong");
    REQUIRE_LT(std::abs(*three_seconds.short_term_lkfs + 3.01), 0.12,
               "complete three-second reference is wrong");

    const std::vector<float> silence(rate, 0.0F);
    REQUIRE_EQ(meter.append(silence), LoudnessStatus::ok, "silence after reference rejected");
    const auto four_seconds = meter.finish();
    REQUIRE_TRUE((!four_seconds.momentary_lkfs || *four_seconds.momentary_lkfs < -80.0),
                 "ungated windows did not follow signal-to-silence transition");
    REQUIRE_TRUE(four_seconds.short_term_lkfs,
                 "ungated windows did not follow signal-to-silence transition");
    REQUIRE_LT(std::abs(*four_seconds.short_term_lkfs -
                        (*three_seconds.short_term_lkfs + 10.0 * std::log10(2.0 / 3.0))),
               0.12, "ungated windows did not follow signal-to-silence transition");

    meter.reset();
    REQUIRE_TRUE(!meter.finish().momentary_lkfs, "reset retained rolling loudness");
    REQUIRE_TRUE(!meter.finish().short_term_lkfs, "reset retained rolling loudness");
  }

  const auto signal = sine(std::size_t{48'000} * 4, LoudnessLayout::stereo, 0, 0.25);
  IntegratedLoudnessAnalyzer whole(48'000, LoudnessLayout::stereo);
  IntegratedLoudnessAnalyzer chunks(48'000, LoudnessLayout::stereo);
  REQUIRE_EQ(whole.append(signal), LoudnessStatus::ok, "whole window fixture rejected");
  for (std::size_t offset = 0; offset < signal.size(); offset += 128) {
    REQUIRE_EQ(chunks.append(std::span(signal).subspan(
                   offset, std::min<std::size_t>(128, signal.size() - offset))),
               LoudnessStatus::ok, "chunked window fixture rejected");
  }
  const auto whole_result = whole.finish();
  const auto chunked_result = chunks.finish();
  REQUIRE_TRUE(whole_result.momentary_lkfs, "chunk boundaries changed rolling loudness");
  REQUIRE_TRUE(whole_result.short_term_lkfs, "chunk boundaries changed rolling loudness");
  REQUIRE_TRUE(chunked_result.momentary_lkfs, "chunk boundaries changed rolling loudness");
  REQUIRE_TRUE(chunked_result.short_term_lkfs, "chunk boundaries changed rolling loudness");
  REQUIRE_LT(std::abs(*whole_result.momentary_lkfs - *chunked_result.momentary_lkfs),
             tolerance::double_rounding, "chunk boundaries changed rolling loudness");
  REQUIRE_LT(std::abs(*whole_result.short_term_lkfs - *chunked_result.short_term_lkfs),
             tolerance::double_rounding, "chunk boundaries changed rolling loudness");
}

void chunking_and_rejection() {
  const auto samples = sine(48'000, LoudnessLayout::stereo, 0, 0.25);
  IntegratedLoudnessAnalyzer whole(48'000, LoudnessLayout::stereo);
  IntegratedLoudnessAnalyzer chunks(48'000, LoudnessLayout::stereo);
  REQUIRE_EQ(whole.append(samples), LoudnessStatus::ok, "whole fixture rejected");
  for (std::size_t offset = 0; offset < samples.size(); offset += 128) {
    REQUIRE_EQ(chunks.append(std::span(samples).subspan(
                   offset, std::min<std::size_t>(128, samples.size() - offset))),
               LoudnessStatus::ok, "chunked fixture rejected");
  }
  REQUIRE_LT(std::abs(measured(whole.finish()) - measured(chunks.finish())),
             tolerance::double_rounding, "chunk boundaries changed integrated loudness");

  std::vector<float> malformed{0.0F, std::numeric_limits<float>::quiet_NaN()};
  REQUIRE_EQ(chunks.append(malformed), LoudnessStatus::malformed_input,
             "non-finite frame partly mutated measurement");
  REQUIRE_EQ(chunks.frames(), 48'000, "non-finite frame partly mutated measurement");
  REQUIRE_EQ(chunks.append(std::span(malformed).first(1)), LoudnessStatus::malformed_input,
             "partial interleaved frame accepted");
  IntegratedLoudnessAnalyzer capped(48'000, LoudnessLayout::mono, 1);
  std::vector<float> too_long(24'000, 0.0F);
  REQUIRE_EQ(capped.append(too_long), LoudnessStatus::capacity_exceeded,
             "block cap did not reject atomically");
  REQUIRE_EQ(capped.frames(), 0, "block cap did not reject atomically");
  REQUIRE_EQ(capped.append(std::span(too_long).first(19'200)), LoudnessStatus::ok,
             "capacity rejection made analyzer unusable");
  REQUIRE_EQ(capped.finish().total_blocks, 1, "capacity rejection made analyzer unusable");
  REQUIRE_EQ(IntegratedLoudnessAnalyzer(22'050, LoudnessLayout::mono).status(),
             LoudnessStatus::unsupported_rate,
             "unsupported sample rate silently used 48 kHz coefficients");
  // Exercise an invalid enum tag from an external decoder.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  REQUIRE_EQ(IntegratedLoudnessAnalyzer(48'000, static_cast<LoudnessLayout>(99)).status(),
             LoudnessStatus::invalid_configuration, "unsupported layout accepted");
  REQUIRE_EQ(IntegratedLoudnessAnalyzer(48'000, LoudnessLayout::mono, 0).status(),
             LoudnessStatus::invalid_configuration, "zero block cap accepted");
}
} // namespace

int main() {
  try {
    reference_levels_and_layouts();
    multirate_reference_levels();
    silence_short_input_and_gates();
    momentary_and_short_term_windows();
    chunking_and_rejection();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

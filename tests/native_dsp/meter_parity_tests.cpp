#include "effect_test_support.hpp"

#include "oscillate/native_dsp/effects/loudness.hpp"
#include "oscillate/native_dsp/loudness.hpp"
#include "oscillate/native_dsp/true_peak.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace tolerance = oscillate::test::tolerance;

// One BS.1770 core: the export analyser (IntegratedLoudnessAnalyzer), the
// master and export true-peak meter (StereoTruePeakMeter) and the Loudness
// effect report identical loudness and peaks on the same signal, at every
// project rate and whatever the block size.
namespace {
using namespace effect_test;
namespace loudness = oscillate::native_dsp::effects::loudness;
namespace native = oscillate::native_dsp;

// Twenty seconds whose level moves through both gates: loud and quiet
// passages, a stretch under the absolute gate, inter-sample peaks near full
// scale, then silence long enough to drain the true-peak interpolator.
Stereo programme(double rate) {
  const auto frames = static_cast<std::size_t>(20.0 * rate);
  Stereo out{std::vector<double>(frames + 256), std::vector<double>(frames + 256)};
  std::uint32_t state = 777;
  const auto random = [&] {
    state = state * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(state >> 8U) / 8'388'608.0 - 1.0;
  };
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double level = t < 6.0 ? 0.3 : (t < 9.0 ? 0.02 : (t < 11.0 ? 0.0002 : 0.5));
    const double tone = std::sin(2.0 * std::numbers::pi * 0.25 * rate * t + 0.7);
    out.left[i] = level * (0.6 * random() + 0.8 * tone);
    out.right[i] = level * (0.6 * random() + 0.5 * std::sin(2.0 * std::numbers::pi * 997.0 * t));
  }
  return out;
}

double to_db(double x) { return x > 0.0 ? 20.0 * std::log10(x) : loudness::silent; }

void meters_agree(double rate, std::size_t block) {
  const auto input = programme(rate);
  // Export: interleaved floats.
  native::IntegratedLoudnessAnalyzer analyser(static_cast<std::uint32_t>(rate),
                                              native::LoudnessLayout::stereo);
  std::vector<float> interleaved;
  std::vector<engine::StereoFrame> frames;
  for (std::size_t i = 0; i < input.size(); ++i) {
    interleaved.push_back(static_cast<float>(input.left[i]));
    interleaved.push_back(static_cast<float>(input.right[i]));
    frames.push_back({static_cast<float>(input.left[i]), static_cast<float>(input.right[i])});
  }
  REQUIRE_EQ(analyser.append(interleaved), native::LoudnessStatus::ok, "the analyser takes it");
  const auto exported = analyser.finish();
  REQUIRE_TRUE(exported.integrated_lkfs, "the analyser measures");
  REQUIRE_TRUE(exported.momentary_lkfs, "the analyser measures");
  REQUIRE_TRUE(exported.short_term_lkfs, "the analyser measures");
  REQUIRE_LT(exported.relative_gated_blocks, exported.absolute_gated_blocks,
             "both gates exclude blocks");
  REQUIRE_LT(exported.absolute_gated_blocks, exported.total_blocks, "both gates exclude blocks");

  // The master (and export) true-peak meter.
  native::StereoTruePeakMeter meter;
  require(meter.prepare(static_cast<std::uint32_t>(rate)), "the meter prepares");
  for (std::size_t i = 0; i < frames.size(); i += 4'096) {
    require(meter.process(
                std::span(frames).subspan(i, std::min<std::size_t>(4'096, frames.size() - i))),
            "the meter takes a block");
  }
  require(meter.finish(), "the meter drains");
  const auto peaks = meter.reading();

  // The Loudness effect, in blocks.
  auto runtime =
      make_effect(loudness::engine(), default_values(*loudness::engine().descriptor), rate);
  (void)process(*runtime, input, {.block = block, .rate = rate});
  std::vector<float> t;
  REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
  REQUIRE_EQ(t.size(), loudness::telemetry::size, "telemetry");

  const std::string at =
      " at " + std::to_string(static_cast<int>(rate)) + " Hz, blocks of " + std::to_string(block);
  REQUIRE_EQ(t[loudness::telemetry::integrated], static_cast<float>(*exported.integrated_lkfs),
             "integrated loudness is identical" + at);
  REQUIRE_EQ(t[loudness::telemetry::momentary], static_cast<float>(*exported.momentary_lkfs),
             "momentary loudness is identical" + at);
  REQUIRE_EQ(t[loudness::telemetry::short_term], static_cast<float>(*exported.short_term_lkfs),
             "short-term loudness is identical" + at);
  REQUIRE_EQ(t[loudness::telemetry::sample_peak],
             static_cast<float>(to_db(std::max(peaks.left_sample_peak, peaks.right_sample_peak))),
             "sample peak is identical" + at);
  // The meter reports its true peak as a float; the effect keeps the same
  // double and converts to dB first. They agree to that float rounding.
  const auto close = [](float effect_db, float meter_linear) {
    return std::fabs(static_cast<double>(effect_db) - to_db(meter_linear)) <
           tolerance::float_round_trip;
  };
  require(close(t[loudness::telemetry::true_peak_left], peaks.left_true_peak) &&
              close(t[loudness::telemetry::true_peak_right], peaks.right_true_peak),
          "true peak is identical to float precision" + at);
  REQUIRE_GT(peaks.left_true_peak, peaks.left_sample_peak, "the signal has inter-sample peaks");
}

} // namespace

int main() {
  try {
    for (const double rate : {44'100.0, 48'000.0, 96'000.0}) {
      meters_agree(rate, 128);
    }
    meters_agree(48'000.0, 0);
    meters_agree(48'000.0, 1'024);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native meter parity contract passed\n";
  return 0;
}

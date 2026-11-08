#include "effect_test_support.hpp"

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/loudness.hpp"
#include "oscillate/native_dsp/true_peak.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Loudness (ADR-0042) against the EBU's own conformance signals:
// Tech 3341 (momentary, short-term and gated integrated loudness, built as
// specified from stereo 1 kHz sines) and Tech 3342 (loudness range), at 44.1,
// 48 and 96 kHz; true peak against the project's StereoTruePeakMeter and an
// intersample case; audio untouched; Integrate clears and restarts; the
// histories carry the readings.
namespace {
using namespace effect_test;
namespace loudness = oscillate::native_dsp::effects::loudness;

const EngineDescriptor& table() { return *loudness::engine().descriptor; }
std::size_t at(std::string_view id) { return index(table(), id); }

// Stereo 1 kHz sine segments of (dBFS peak, seconds).
Stereo segments(const std::vector<std::pair<double, double>>& parts,
                double rate = oscillate::test::sample_rate_48k_hz, double hz = 1'000.0) {
  Stereo out;
  double phase = 0.0;
  for (const auto& [level, seconds] : parts) {
    const auto frames = static_cast<std::size_t>(std::lround(seconds * rate));
    const double amplitude = oscillate::native_dsp::dsp::db_to_gain(level);
    for (std::size_t i = 0; i < frames; ++i) {
      const double x = amplitude * std::sin(2.0 * std::numbers::pi * phase);
      phase += hz / rate;
      out.left.push_back(x);
      out.right.push_back(x);
    }
  }
  return out;
}

std::vector<float> measure(const Stereo& input, double rate = oscillate::test::sample_rate_48k_hz,
                           std::vector<double> values = {}) {
  if (values.empty()) {
    values = preset(table(), "EBU R128 Broadcast");
  }
  auto runtime = make_effect(loudness::engine(), values, rate);
  (void)process(*runtime, input, {.rate = rate});
  std::vector<float> t;
  REQUIRE_TRUE(runtime->read_telemetry(t), "telemetry");
  REQUIRE_EQ(t.size(), loudness::telemetry::size, "telemetry");
  return t;
}

void tech_3341_cases() {
  struct Case {
    std::string_view name;
    std::vector<std::pair<double, double>> parts;
    double integrated;
  };
  const std::vector<Case> cases{
      {"case 1", {{-23.0, 20.0}}, -23.0},
      {"case 2", {{-33.0, 20.0}}, -33.0},
      {"case 3 (relative gate)", {{-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}}, -23.0},
      {"case 4 (both gates)",
       {{-72.0, 10.0}, {-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}, {-72.0, 10.0}},
       -23.0},
      {"case 5", {{-26.0, 20.0}, {-20.0, 20.1}, {-26.0, 20.0}}, -23.0},
  };
  for (const double rate : {44'100.0, 48'000.0, 96'000.0}) {
    for (const auto& c : cases) {
      const auto t = measure(segments(c.parts, rate), rate);
      const double integrated = t[loudness::telemetry::integrated];
      REQUIRE_LT(std::fabs(integrated - c.integrated), 0.1,
                 std::string(c.name) + " at " + std::to_string(rate) + " Hz: integrated " +
                     std::to_string(integrated) + " LUFS, expected " +
                     std::to_string(c.integrated));
    }
    // Momentary and short-term of a steady -23 dBFS sine.
    const auto t = measure(segments({{-23.0, 5.0}}, rate), rate);
    require(std::fabs(t[loudness::telemetry::momentary] + 23.0F) < 0.1F &&
                std::fabs(t[loudness::telemetry::short_term] + 23.0F) < 0.1F,
            "momentary and short-term read a steady sine at " + std::to_string(rate) + " Hz");
  }
  std::cout << "  Tech 3341 cases 1-5 within 0.1 LU at 44.1, 48 and 96 kHz\n";
}

void tech_3342_cases() {
  struct Case {
    std::string_view name;
    std::vector<std::pair<double, double>> parts;
    double range;
  };
  const std::vector<Case> cases{
      {"case 1", {{-20.0, 20.0}, {-30.0, 20.0}}, 10.0},
      {"case 2", {{-20.0, 20.0}, {-15.0, 20.0}}, 5.0},
      {"case 3", {{-40.0, 20.0}, {-20.0, 20.0}}, 20.0},
      {"case 4", {{-50.0, 20.0}, {-35.0, 20.0}, {-20.0, 20.0}, {-35.0, 20.0}, {-50.0, 20.0}}, 15.0},
  };
  for (const auto& c : cases) {
    const auto t = measure(segments(c.parts));
    const double range = t[loudness::telemetry::range];
    REQUIRE_LT(std::fabs(range - c.range), 1.0,
               std::string("Tech 3342 ") + std::string(c.name) + ": range " +
                   std::to_string(range) + " LU, expected " + std::to_string(c.range));
  }
  std::cout << "  Tech 3342 cases 1-4 within 1 LU\n";
}

void true_peak_matches_the_project_meter() {
  oscillate::native_dsp::StereoTruePeakMeter reference;
  require(reference.prepare(48'000), "the reference meter prepares");
  // Programme-like: noise plus high sines near Nyquist, where intersample
  // peaks live.
  auto input = noise(0.3, std::size_t{4} * 48'000);
  for (const double hz : {9'000.0, 15'000.0, 19'500.0}) {
    const auto tone = sine(hz, 0.2, input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      input.left[i] += tone.left[i];
      input.right[i] += 0.8 * tone.right[i];
    }
  }
  std::vector<engine::StereoFrame> frames(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    frames[i] = {static_cast<float>(input.left[i]), static_cast<float>(input.right[i])};
  }
  for (std::size_t start = 0; start < frames.size(); start += 1'024) {
    require(reference.process(std::span(frames).subspan(
                start, std::min<std::size_t>(1'024, frames.size() - start))),
            "the reference meter runs");
  }
  require(reference.finish(), "the reference meter finishes");
  const auto r = reference.reading();
  const double expected = db(std::max(r.left_true_peak, r.right_true_peak));
  const auto t = measure(input);
  const double ours = t[loudness::telemetry::true_peak];
  REQUIRE_LT(std::fabs(ours - expected), 0.1,
             "true peak " + std::to_string(ours) + " dBTP against the project meter's " +
                 std::to_string(expected));
  // An intersample peak: a full-scale sine at fs/4, sampled 45 degrees off
  // its peaks, has sample peaks at -3 dB but a true peak at 0 dBTP.
  Stereo quarter{std::vector<double>(48'000), std::vector<double>(48'000)};
  for (std::size_t i = 0; i < quarter.size(); ++i) {
    quarter.left[i] = quarter.right[i] =
        std::sin(2.0 * std::numbers::pi * 0.25 * static_cast<double>(i) + std::numbers::pi / 4.0);
  }
  const auto q = measure(quarter);
  REQUIRE_LT(std::fabs(q[loudness::telemetry::sample_peak] + 3.01F), 0.05F,
             "the samples peak at -3 dB");
  REQUIRE_LT(std::fabs(q[loudness::telemetry::true_peak]), 0.5F,
             "the true peak is found between them: " +
                 std::to_string(q[loudness::telemetry::true_peak]) + " dBTP");
  std::cout << "  true peak within 0.1 dB of the project meter (" << ours << " vs " << expected
            << " dBTP)\n";
}

void audio_passes_untouched_and_integrate_restarts() {
  const auto input = segments({{-20.0, 5.0}});
  auto values = preset(table(), "Streaming (Spotify, YouTube)");
  auto runtime = make_effect(loudness::engine(), values);
  const auto out = process(*runtime, input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    REQUIRE_EQ(out.left[i], static_cast<double>(static_cast<float>(input.left[i])),
               "the meter never touches the audio");
  }
  std::vector<float> t;
  require(runtime->read_telemetry(t), "telemetry");
  REQUIRE_LT(std::fabs(t[loudness::telemetry::duration] - 4.7F), 0.15F,
             "integration counts its time: " + std::to_string(t[loudness::telemetry::duration]));
  auto off = values;
  off[at("loudness.integrate")] = 0.0;
  require(runtime->offer_values(off, 1), "Integrate off offered");
  (void)process(*runtime, segments({{-30.0, 1.0}}), {}, input.size());
  require(runtime->read_telemetry(t), "telemetry");
  REQUIRE_LE(t[loudness::telemetry::integrated], -199.0F, "switching Integrate off clears it");
  REQUIRE_EQ(t[loudness::telemetry::duration], 0.0F, "switching Integrate off clears it");
  require(runtime->offer_values(values, 2), "Integrate on offered");
  (void)process(*runtime, segments({{-30.0, 6.0}}), {}, input.size() + 48'000);
  require(runtime->read_telemetry(t), "telemetry");
  REQUIRE_LT(std::fabs(t[loudness::telemetry::integrated] + 30.0F), 0.1F,
             "and on again measures afresh: " + std::to_string(t[loudness::telemetry::integrated]));
  // PLR: a sine's true peak over its loudness is its 3 dB crest plus the
  // K-weighting at 1 kHz.
  REQUIRE_LT(std::fabs(t[loudness::telemetry::plr] - (-30.0F + 30.0F)), 0.6F,
             "PLR of a -30 dBFS sine reading -30 LUFS is about 0 LU");
  // The history ends with the latest readings.
  REQUIRE_LT(std::fabs(t[loudness::telemetry::short_term_history + loudness::history - 1] + 30.0F),
             0.1F, "the history ends on the latest short-term reading");
}

// A moved-from meter refuses work until it is prepared again.
void moved_from_true_peak_meter_refuses_work() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  oscillate::native_dsp::StereoTruePeakMeter source;
  require(source.prepare(rate), "the moved meter prepares");
  const std::vector<engine::StereoFrame> block(1, {0.5F, 0.5F});
  oscillate::native_dsp::StereoTruePeakMeter moved(std::move(source));
  require(moved.process(block), "the moved-to meter works");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the point of the test
  require(!source.process(block), "a moved-from meter processed audio");
  require(!source.finish(), "a moved-from meter finished");
  require(source.prepare(rate) && source.process(block), "a re-prepared meter works again");
}

} // namespace

int main() {
  try {
    tech_3341_cases();
    tech_3342_cases();
    true_peak_matches_the_project_meter();
    moved_from_true_peak_meter_refuses_work();
    audio_passes_untouched_and_integrate_restarts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "native loudness contract passed\n";
  return 0;
}

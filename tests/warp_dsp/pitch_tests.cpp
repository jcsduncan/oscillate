#include "fixtures.hpp"
#include "oscillate/test/audio_constants.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/warp_dsp/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace warp = oscillate::warp_dsp;
namespace analysis = oscillate::warp_dsp::analysis;

using oscillate::test::require;

warp_fixtures::Stereo harmonic_tone(std::uint32_t rate, double f0, double seconds) {
  auto signal = warp_fixtures::silence(static_cast<std::size_t>(seconds * rate));
  for (std::size_t index = 0; index < signal.left.size(); ++index) {
    const double t = static_cast<double>(index) / rate;
    double value = 0.0;
    for (int harmonic = 1; harmonic <= 8; ++harmonic)
      value += std::sin(2.0 * std::numbers::pi * f0 * harmonic * t + harmonic) / harmonic;
    signal.left[index] = static_cast<float>(0.2 * value);
    signal.right[index] = signal.left[index];
  }
  return signal;
}

void tracks_harmonic_tones() {
  for (const std::uint32_t rate : {44'100U, 48'000U, 96'000U}) {
    for (const double f0 : {65.0, 110.0, 196.0, 440.0, 880.0}) {
      const auto tone = harmonic_tone(rate, f0, 1.5);
      warp::MemorySourceView view(tone.left, tone.right);
      const auto track = analysis::track_pitch(view, {rate, 60.0F, 1'000.0F, 0.15F});
      REQUIRE_OK(track, "pitch tracking failed");
      double worst = 0.0;
      std::size_t voiced = 0;
      std::size_t counted = 0;
      for (const auto& frame : track.value()) {
        if (frame.frame < rate / 10 || frame.frame + rate / 10 > tone.left.size())
          continue;
        ++counted;
        if (frame.frequency > 0.0F) {
          ++voiced;
          worst = std::max(worst, std::fabs(frame.frequency / f0 - 1.0));
        }
      }
      std::printf("pitch %6u Hz f0 %6.1f: voiced %zu/%zu worst error %.4f%%\n", rate, f0, voiced,
                  counted, 100.0 * worst);
      REQUIRE_EQ(voiced, counted, "harmonic tone not voiced throughout");
      REQUIRE_LT(worst, 0.001, "pitch estimate off by more than 0.1%");
    }
  }
}

void rejects_noise_and_silence() {
  auto noise = warp_fixtures::silence(48'000 * 2);
  warp_fixtures::add_noise_floor(noise, 0.3, 9);
  warp::MemorySourceView noisy(noise.left, noise.right);
  const auto track = analysis::track_pitch(noisy, {48'000, 60.0F, 1'000.0F, 0.15F});
  REQUIRE_OK(track, "noise tracking failed");
  const auto voiced = std::ranges::count_if(
      track.value(), [](const auto& frame) { return frame.frequency > 0.0F; });
  std::printf("white noise: %td/%zu frames voiced\n", voiced, track.value().size());
  REQUIRE_LT(static_cast<double>(voiced), 0.05 * static_cast<double>(track.value().size()),
             "white noise reported as voiced");
  const auto quiet = warp_fixtures::silence(48'000);
  warp::MemorySourceView silent(quiet.left, quiet.right);
  const auto none = analysis::track_pitch(silent, {48'000, 60.0F, 1'000.0F, 0.15F});
  REQUIRE_OK(none, "silence reported as voiced");
  REQUIRE_TRUE(
      std::ranges::all_of(none.value(), [](const auto& frame) { return frame.frequency == 0.0F; }),
      "silence reported as voiced");
}

void marks_follow_the_period() {
  const std::uint32_t rate = oscillate::test::sample_rate_48k;
  for (const double f0 : {98.0, 150.0, 330.0}) {
    const auto tone = harmonic_tone(rate, f0, 2.0);
    warp::MemorySourceView view(tone.left, tone.right);
    const auto track = analysis::track_pitch(view, {rate, 60.0F, 1'000.0F, 0.15F});
    REQUIRE_OK(track, "pitch tracking failed");
    const auto marks = analysis::place_pitch_marks(view, track.value(), rate);
    REQUIRE_OK(marks, "pitch marking failed");
    const double period = rate / f0;
    double worst = 0.0;
    for (std::size_t index = 1; index < marks.value().size(); ++index) {
      const double spacing = static_cast<double>(marks.value()[index].frame) -
                             static_cast<double>(marks.value()[index - 1].frame);
      worst = std::max(worst, std::fabs(spacing - period));
    }
    const double expected = static_cast<double>(tone.left.size()) / period;
    std::printf("marks f0 %.0f: %zu marks (expected ~%.0f), worst spacing error %.2f frames\n", f0,
                marks.value().size(), expected, worst);
    REQUIRE_GT(static_cast<double>(marks.value().size()), 0.9 * expected,
               "pitch marks missing in a voiced tone");
    REQUIRE_LE(worst, 1.0, "pitch marks drift from the period");
  }
}

// A tone voiced from the very first frame starts its voiced run before frame
// 0; the run must still be marked from its first period onwards.
void marks_a_run_voiced_from_the_source_start() {
  const std::uint32_t rate = oscillate::test::sample_rate_48k;
  for (const double f0 : {330.0, 440.0}) {
    auto tone = warp_fixtures::silence(rate);
    for (std::size_t index = 0; index < tone.left.size(); ++index) {
      const double value =
          0.4 * std::sin(2.0 * std::numbers::pi * f0 * static_cast<double>(index) / rate);
      tone.left[index] = static_cast<float>(value);
      tone.right[index] = static_cast<float>(-0.5 * value);
    }
    warp::MemorySourceView view(tone.left, tone.right);
    const auto track = analysis::track_pitch(view, {rate, 60.0F, 1'000.0F, 0.15F});
    REQUIRE_OK(track, "fixture is not voiced from its first frame");
    REQUIRE_TRUE(!track.value().empty(), "fixture is not voiced from its first frame");
    REQUIRE_GT(track.value().front().frequency, 0.0F, "fixture is not voiced from its first frame");
    const auto marks = analysis::place_pitch_marks(view, track.value(), rate);
    REQUIRE_OK(marks, "pitch marking failed");
    const double period = rate / f0;
    REQUIRE_GT(static_cast<double>(marks.value().size()), 0.9 * (rate / period),
               "a run voiced from the source start lost its pitch marks");
    REQUIRE_LT(static_cast<double>(marks.value().front().frame), 2.0 * period,
               "a run voiced from the source start lost its pitch marks");
  }
}

// A voiced frequency whose period the mark arithmetic cannot hold is refused;
// unvoiced frames stay accepted.
void marks_refuse_unrepresentable_periods() {
  constexpr std::uint32_t rate = oscillate::test::sample_rate_48k;
  constexpr float vanishing_hz = 1e-30F;
  const auto tone = harmonic_tone(rate, 110.0, 0.1);
  warp::MemorySourceView view(tone.left, tone.right);
  const std::vector<analysis::PitchFrame> unvoiced{{0, 0.0F, 0.0F}};
  REQUIRE_OK(analysis::place_pitch_marks(view, unvoiced, rate), "an unvoiced frame was refused");
  constexpr float lowest_pitch_hz = analysis::lowest_pitch_hz;
  for (const float frequency :
       {vanishing_hz, std::nextafter(lowest_pitch_hz, 0.0F),
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    const std::vector<analysis::PitchFrame> invalid{{0, frequency, 1.0F}};
    const auto refused = analysis::place_pitch_marks(view, invalid, rate);
    REQUIRE_TRUE(!refused, "an unrepresentable pitch period was accepted");
    REQUIRE_EQ(refused.error().code, oscillate::foundation::ErrorCode::invalid_argument,
               "an unrepresentable pitch period was accepted");
  }
  const std::vector<analysis::PitchFrame> lowest{{0, lowest_pitch_hz, 1.0F}};
  REQUIRE_OK(analysis::place_pitch_marks(view, lowest, rate), "the lowest pitch was refused");
}

} // namespace

int main() {
  try {
    tracks_harmonic_tones();
    rejects_noise_and_silence();
    marks_follow_the_period();
    marks_a_run_voiced_from_the_source_start();
    marks_refuse_unrepresentable_periods();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

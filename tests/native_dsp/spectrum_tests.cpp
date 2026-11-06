#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/spectrum.hpp"
#include "oscillate/test/check.hpp"
#include "oscillate/test/tolerances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tolerance = oscillate::test::tolerance;

namespace {
using oscillate::engine::StereoFrame;
using oscillate::native_dsp::StereoSpectrum;
using oscillate::native_dsp::StereoSpectrumAnalyzer;

constexpr auto window_frames = StereoSpectrum::window_frames;

using oscillate::test::require;

StereoSpectrum analyzed(std::span<const StereoFrame> frames, std::uint32_t rate) {
  const auto result = StereoSpectrumAnalyzer::analyze(frames, rate);
  if (!result) {
    throw std::runtime_error("valid spectrum window rejected");
  }
  return *result;
}

double bin_frequency(const StereoSpectrum& spectrum, std::size_t bin) {
  const auto frequency = spectrum.frequency_hz(bin);
  if (!frequency) {
    throw std::runtime_error("valid spectrum bin rejected");
  }
  return *frequency;
}

void silence_and_rate_bounds() {
  std::array<StereoFrame, window_frames> silence{};
  for (const auto rate : {44'100U, 48'000U, 88'200U, 96'000U}) {
    const auto spectrum = analyzed(silence, rate);
    REQUIRE_EQ(spectrum.sample_rate, rate, "supported rate or complete silence rejected");
    REQUIRE_EQ(spectrum.invalid_frames, 0, "supported rate or complete silence rejected");
    REQUIRE_TRUE(std::ranges::all_of(spectrum.left, [](float value) { return value == 0.0F; }),
                 "silence had nonzero spectrum");
    REQUIRE_TRUE(std::ranges::all_of(spectrum.right, [](float value) { return value == 0.0F; }),
                 "silence had nonzero spectrum");
    REQUIRE_EQ(spectrum.frequency_hz(0), 0.0, "frequency-bin bounds or Nyquist mapping failed");
    REQUIRE_EQ(spectrum.frequency_hz(window_frames / 2), static_cast<double>(rate) / 2.0,
               "frequency-bin bounds or Nyquist mapping failed");
    REQUIRE_TRUE(!spectrum.frequency_hz(StereoSpectrum::bin_count),
                 "frequency-bin bounds or Nyquist mapping failed");
  }
  REQUIRE_TRUE(!StereoSpectrumAnalyzer::analyze(silence, 0),
               "unsupported rate or incomplete window accepted");
  REQUIRE_TRUE(!StereoSpectrumAnalyzer::analyze(silence, 44'000),
               "unsupported rate or incomplete window accepted");
  REQUIRE_TRUE(
      !StereoSpectrumAnalyzer::analyze(std::span(silence).first(window_frames - 1), 48'000),
      "unsupported rate or incomplete window accepted");
  REQUIRE_TRUE(!StereoSpectrumAnalyzer::analyze(std::span(silence).last(window_frames - 1), 48'000),
               "unsupported rate or incomplete window accepted");
  StereoSpectrum uninitialized{};
  require(!uninitialized.frequency_hz(0), "uninitialized spectrum exposed frequency");
}

void bin_centered_stereo_sines() {
  std::array<StereoFrame, window_frames> frames{};
  for (std::size_t index = 0; index < window_frames; ++index) {
    const auto position = static_cast<double>(index) / static_cast<double>(window_frames);
    frames[index].left =
        static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 37.0 * position));
    frames[index].right =
        static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * 401.0 * position));
  }
  const auto spectrum = analyzed(frames, 48'000);
  REQUIRE_LT(std::abs(spectrum.left[37] - 0.8F), tolerance::float_processing_f,
             "bin-centred Hann amplitude normalization failed");
  REQUIRE_LT(std::abs(spectrum.right[401] - 0.25F), tolerance::float_processing_f,
             "bin-centred Hann amplitude normalization failed");
  REQUIRE_LT(std::abs(spectrum.left[36] - 0.4F), tolerance::float_processing_f,
             "periodic Hann neighbouring-bin shape failed");
  REQUIRE_LT(std::abs(spectrum.left[38] - 0.4F), tolerance::float_processing_f,
             "periodic Hann neighbouring-bin shape failed");
  REQUIRE_LT(std::abs(spectrum.right[400] - 0.125F), tolerance::float_processing_f,
             "periodic Hann neighbouring-bin shape failed");
  REQUIRE_LT(spectrum.left[401], 1e-5F, "stereo channels leaked into each other");
  REQUIRE_LT(spectrum.right[37], 1e-5F, "stereo channels leaked into each other");
  REQUIRE_LT(std::abs(bin_frequency(spectrum, 37) - 37.0 * 48'000.0 / window_frames),
             tolerance::double_accumulation, "frequency-bin mapping failed");
}

void dc_and_nyquist() {
  std::array<StereoFrame, window_frames> frames{};
  for (std::size_t index = 0; index < window_frames; ++index) {
    frames[index] = {0.5F, index % 2 == 0 ? 0.25F : -0.25F};
  }
  const auto spectrum = analyzed(frames, 96'000);
  REQUIRE_LT(std::abs(spectrum.left[0] - 0.5F), tolerance::float_processing_f,
             "DC or Nyquist bin incorrectly doubled");
  REQUIRE_LT(std::abs(spectrum.right[window_frames / 2] - 0.25F), tolerance::float_processing_f,
             "DC or Nyquist bin incorrectly doubled");
  REQUIRE_LT(spectrum.left[window_frames / 2], 1e-5F, "DC and Nyquist channels were mixed");
  REQUIRE_LT(spectrum.right[0], 1e-5F, "DC and Nyquist channels were mixed");
}

// Independent O(N) single-bin DFT of a deterministic broadband fixture.
struct ReferenceBin {
  std::span<const StereoFrame> frames;
  std::size_t bin;
  bool right;
};

double reference_bin(ReferenceBin reference) {
  const auto frames = reference.frames;
  const auto bin = reference.bin;
  const auto right = reference.right;
  std::complex<double> sum{};
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const auto position = static_cast<double>(index) / static_cast<double>(frames.size());
    const auto window = oscillate::native_dsp::dsp::hann(position, 1.0);
    const auto phase = -2.0 * std::numbers::pi * static_cast<double>(bin) * position;
    const auto value = static_cast<double>(right ? frames[index].right : frames[index].left);
    sum += value * window * std::complex<double>{std::cos(phase), std::sin(phase)};
  }
  const auto scale = (bin == 0 || bin == frames.size() / 2) ? 1.0 : 2.0;
  return std::abs(sum) * scale / (static_cast<double>(frames.size()) / 2.0);
}

void broadband_reference() {
  std::array<StereoFrame, window_frames> frames{};
  for (std::size_t index = 0; index < window_frames; ++index) {
    const auto value = static_cast<double>((index * 71 + index * index * 13) % 101) / 101.0;
    frames[index] = {static_cast<float>(value - 0.5), static_cast<float>(0.2 * value - 0.1)};
  }
  const auto spectrum = analyzed(frames, 44'100);
  for (const auto bin : {0U, 1U, 7U, 137U, 701U, 1024U}) {
    REQUIRE_LT(std::abs(spectrum.left[bin] - reference_bin({frames, bin, false})),
               tolerance::float_processing, "FFT differs from independent single-bin DFT");
    REQUIRE_LT(std::abs(spectrum.right[bin] - reference_bin({frames, bin, true})),
               tolerance::float_processing, "FFT differs from independent single-bin DFT");
  }
}

void malformed_input_remains_finite() {
  std::array<StereoFrame, window_frames> frames{};
  frames[3].left = std::numeric_limits<float>::quiet_NaN();
  frames[7].left = std::numeric_limits<float>::infinity();
  frames[7].right = -std::numeric_limits<float>::infinity();
  frames[11].right = -std::numeric_limits<float>::infinity();
  const auto spectrum = analyzed(frames, 48'000);
  REQUIRE_EQ(spectrum.invalid_frames, 3, "invalid stereo frames counted incorrectly");
  REQUIRE_TRUE(
      std::ranges::all_of(spectrum.left,
                          [](float value) { return std::isfinite(value) && value == 0.0F; }),
      "non-finite source contaminated result");
  REQUIRE_TRUE(
      std::ranges::all_of(spectrum.right,
                          [](float value) { return std::isfinite(value) && value == 0.0F; }),
      "non-finite source contaminated result");
  frames.fill({std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()});
  const auto extreme = analyzed(frames, 48'000);
  REQUIRE_EQ(extreme.invalid_frames, 0, "finite extreme source overflowed output");
  REQUIRE_TRUE(std::ranges::all_of(extreme.left, [](float value) { return std::isfinite(value); }),
               "finite extreme source overflowed output");
  REQUIRE_TRUE(std::ranges::all_of(extreme.right, [](float value) { return std::isfinite(value); }),
               "finite extreme source overflowed output");
}
} // namespace

int main() {
  try {
    silence_and_rate_bounds();
    bin_centered_stereo_sines();
    dc_and_nyquist();
    broadband_reference();
    malformed_input_remains_finite();
  } catch (const std::exception& error) {
    std::cerr << "spectrum contract: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

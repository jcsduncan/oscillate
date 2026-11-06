#include "oscillate/native_dsp/spectrum.hpp"

#include "dsp/fft.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>

namespace oscillate::native_dsp {
namespace {

constexpr auto window_frames = StereoSpectrum::window_frames;
using TransformBuffer = std::array<std::complex<double>, window_frames>;

[[nodiscard]] const std::array<double, window_frames>& hann_window() noexcept {
  static const auto coefficients = [] {
    std::array<double, window_frames> values{};
    for (std::size_t index = 0; index < window_frames; ++index) {
      values[index] = oscillate::native_dsp::dsp::hann(static_cast<double>(index),
                                                       static_cast<double>(window_frames));
    }
    return values;
  }();
  return coefficients;
}

[[nodiscard]] constexpr bool supported_rate(std::uint32_t rate) noexcept {
  return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000;
}

// One shared table set; analyze() runs on a worker, never the callback.
[[nodiscard]] const dsp::Fft& window_fft() {
  static const dsp::Fft fft(window_frames);
  return fft;
}

void transform(TransformBuffer& samples) noexcept { window_fft().forward(samples); }

void fill_amplitudes(const TransformBuffer& transformed,
                     std::array<float, StereoSpectrum::bin_count>& amplitudes) noexcept {
  // A periodic Hann window sums to N/2. Double interior one-sided bins, but
  // not DC or Nyquist, which have no mirrored partner in the returned range.
  const auto coherent_sum = static_cast<double>(window_frames) / 2.0;
  for (std::size_t bin = 0; bin < StereoSpectrum::bin_count; ++bin) {
    const auto one_sided = (bin == 0 || bin == window_frames / 2) ? 1.0 : 2.0;
    const auto amplitude = std::abs(transformed[bin]) * one_sided / coherent_sum;
    amplitudes[bin] = static_cast<float>(
        std::min(amplitude, static_cast<double>(std::numeric_limits<float>::max())));
  }
}

} // namespace

std::optional<double> StereoSpectrum::frequency_hz(std::size_t bin) const noexcept {
  if (bin >= bin_count || !supported_rate(sample_rate)) {
    return std::nullopt;
  }
  return static_cast<double>(bin) * static_cast<double>(sample_rate) /
         static_cast<double>(window_frames);
}

std::optional<StereoSpectrum>
StereoSpectrumAnalyzer::analyze(std::span<const engine::StereoFrame> frames,
                                std::uint32_t sample_rate) noexcept {
  if (frames.size() != window_frames || !supported_rate(sample_rate)) {
    return std::nullopt;
  }

  TransformBuffer left{};
  TransformBuffer right{};
  StereoSpectrum reading{};
  reading.sample_rate = sample_rate;
  const auto& window = hann_window();
  for (std::size_t index = 0; index < window_frames; ++index) {
    const auto left_valid = std::isfinite(frames[index].left);
    const auto right_valid = std::isfinite(frames[index].right);
    reading.invalid_frames += static_cast<std::uint32_t>(!left_valid || !right_valid);
    left[index] = (left_valid ? static_cast<double>(frames[index].left) : 0.0) * window[index];
    right[index] = (right_valid ? static_cast<double>(frames[index].right) : 0.0) * window[index];
  }

  transform(left);
  transform(right);
  fill_amplitudes(left, reading.left);
  fill_amplitudes(right, reading.right);
  return reading;
}

} // namespace oscillate::native_dsp

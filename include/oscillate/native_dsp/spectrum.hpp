#pragma once

#include "oscillate/engine/graph.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace oscillate::native_dsp {

// One independent, worker-side window. Bin values are one-sided linear peak
// amplitudes (not power or dB), normalized for a periodic Hann window. A
// bin-centred, unit-amplitude sine yields approximately 1 at its bin.
struct StereoSpectrum {
  static constexpr std::size_t window_frames = 2048;
  static constexpr std::size_t bin_count = window_frames / 2 + 1;

  std::array<float, bin_count> left{};
  std::array<float, bin_count> right{};
  std::uint32_t sample_rate = 0;
  std::uint32_t invalid_frames = 0;

  [[nodiscard]] std::optional<double> frequency_hz(std::size_t bin) const noexcept;
};

class StereoSpectrumAnalyzer final {
public:
  // Call only on a worker/control thread with a copied, complete window.
  // No callback accumulation or publication is provided. Returns nullopt for
  // a wrong-sized window or unsupported project sample rate. Non-finite
  // channel values are individually replaced with silence and counted once
  // per affected stereo frame.
  [[nodiscard]] static std::optional<StereoSpectrum>
  analyze(std::span<const engine::StereoFrame> frames, std::uint32_t sample_rate) noexcept;
};

} // namespace oscillate::native_dsp

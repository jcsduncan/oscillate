#pragma once

#include "oscillate/warp_dsp/real_fft.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::clay_dsp::detail {

inline constexpr double two_pi = 6.283185307179586;
inline constexpr double pi = 3.141592653589793;
// Smallest and largest analysis FFTs.
inline constexpr std::size_t min_fft = 256;
inline constexpr std::size_t max_fft = std::size_t{1} << 16;

// The smallest power of two at least `samples`, within [min_fft, max_fft].
[[nodiscard]] std::size_t fft_size_at_least(double samples) noexcept;

// A sinusoidal peak: reassigned position in bins, amplitude of the sinusoid
// and its phase at the window's centre.
struct Peak {
  double bin = 0.0;
  double amplitude = 0.0;
  double phase = 0.0;
};

// Zero-phase Hann analysis of one window of a signal, with frequency
// reassignment (Auger & Flandrin 1995) from the derivative window. Samples
// outside the signal read as zero.
class FrameAnalyser {
public:
  explicit FrameAnalyser(std::size_t size);

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t bins() const noexcept { return size_ / 2 + 1; }
  void analyse(std::span<const float> signal, std::int64_t centre) noexcept;
  // Sinusoid-amplitude scale: a full-scale sine at a bin centre reads 1.
  [[nodiscard]] std::span<const float> magnitude() const noexcept { return magnitude_; }
  [[nodiscard]] Peak peak(std::size_t bin) const noexcept;
  // Local maxima at least `floor` (sinusoid amplitude), by bin.
  void peaks(double floor, std::vector<Peak>& out) const;

private:
  [[nodiscard]] double lobe(double bins) const noexcept;

  std::size_t size_;
  warp_dsp::RealFft fft_;
  warp_dsp::AlignedBuffer buffer_;
  warp_dsp::AlignedBuffer spectrum_;
  warp_dsp::AlignedBuffer derivative_;
  std::vector<float> window_;     // zero-phase order
  std::vector<float> d_window_;   // derivative, zero-phase order
  std::vector<float> magnitude_;  // sinusoid amplitude per bin
  std::vector<float> lobe_table_; // |W(delta)| / W(0) for delta in [0, lobe_span]
  double scale_ = 0.0;            // |X| -> sinusoid amplitude at a bin centre
};

// True envelope (Röbel & Rodet 2005) of a magnitude spectrum: cepstral
// smoothing iterated so the envelope rides the harmonic peaks.
class EnvelopeEstimator {
public:
  explicit EnvelopeEstimator(std::size_t size);
  // `magnitude` has size/2 + 1 bins; writes natural-log amplitudes per bin.
  void estimate(std::span<const float> magnitude, double f0, std::uint32_t sample_rate,
                std::span<double> log_envelope);

private:
  void smooth(std::size_t order) noexcept;

  std::size_t size_;
  warp_dsp::RealFft fft_;
  warp_dsp::AlignedBuffer even_;
  warp_dsp::AlignedBuffer cepstrum_;
  std::vector<double> target_;
  std::vector<double> smoothed_;
};

// Natural-log amplitude at `hz` read from a per-bin envelope.
[[nodiscard]] double read_bins(std::span<const double> log_envelope, double hz,
                               double bin_hz) noexcept;

} // namespace oscillate::clay_dsp::detail

#include "spectrum.hpp"

#include <algorithm>
#include <cmath>

namespace oscillate::clay_dsp::detail {
namespace {

// The window's main lobe is tabulated out to this many bins.
constexpr double lobe_span = 2.0;
constexpr std::size_t lobe_points = 512;
// Lobe gains below this are not trusted for amplitude correction.
constexpr double min_lobe = 0.05;
// Reassignment never moves a peak further than this from its bin.
constexpr double max_reassign_bins = 1.0;
// Magnitudes are floored here before taking logs.
constexpr double log_floor = 1e-9;
// True envelope: iterate until the envelope lies within this much (natural
// log, about 2 dB) under every bin of the spectrum, or this many times.
constexpr double envelope_converged = 0.23;
constexpr int envelope_iterations = 64;
constexpr std::size_t min_cepstral_order = 4;
// The cepstral order is the pitch period over this. At 2 (fs / 2f0, the
// largest order that can still pass between harmonics) the envelope ripples
// at twice f0 and its peaks land on single harmonics; 3 keeps the formants
// (resolution about 3 f0) without the ripple.
constexpr double cepstral_periods = 3.0;

// Bin `index` of a transform of `size` bins.
struct Bin {
  std::size_t index = 0;
  std::size_t size = 0;
};
[[nodiscard]] std::int64_t centred_index(Bin bin) noexcept {
  const auto n = static_cast<std::int64_t>(bin.index);
  const auto half = static_cast<std::int64_t>(bin.size / 2);
  return n < half ? n : n - static_cast<std::int64_t>(bin.size);
}

} // namespace

std::size_t fft_size_at_least(double samples) noexcept {
  std::size_t size = min_fft;
  while (static_cast<double>(size) < samples && size < max_fft)
    size <<= 1;
  return size;
}

FrameAnalyser::FrameAnalyser(std::size_t size)
    : size_(size), fft_(size), buffer_(size), spectrum_(size), derivative_(size), window_(size),
      d_window_(size), magnitude_(size / 2 + 1), lobe_table_(lobe_points + 1) {
  const auto n_size = static_cast<double>(size);
  double sum = 0.0;
  for (std::size_t i = 0; i < size; ++i) {
    const auto n = static_cast<double>(centred_index({.index = i, .size = size}));
    window_[i] = static_cast<float>(0.5 + 0.5 * std::cos(two_pi * n / n_size));
    d_window_[i] = static_cast<float>(-(pi / n_size) * std::sin(two_pi * n / n_size));
    sum += window_[i];
  }
  scale_ = 2.0 / sum;
  for (std::size_t p = 0; p <= lobe_points; ++p) {
    const double delta = lobe_span * static_cast<double>(p) / static_cast<double>(lobe_points);
    double value = 0.0;
    for (std::size_t i = 0; i < size; ++i) {
      const auto n = static_cast<double>(centred_index({.index = i, .size = size}));
      value += window_[i] * std::cos(two_pi * delta * n / n_size);
    }
    lobe_table_[p] = static_cast<float>(std::abs(value) / sum);
  }
}

double FrameAnalyser::lobe(double bins) const noexcept {
  const double position =
      std::clamp(std::abs(bins), 0.0, lobe_span) / lobe_span * static_cast<double>(lobe_points);
  const auto below = std::min(static_cast<std::size_t>(position), lobe_points - 1);
  const double t = position - static_cast<double>(below);
  return std::max(min_lobe, (1.0 - t) * lobe_table_[below] + t * lobe_table_[below + 1]);
}

void FrameAnalyser::analyse(std::span<const float> signal, std::int64_t centre) noexcept {
  const auto length = static_cast<std::int64_t>(signal.size());
  for (std::size_t i = 0; i < size_; ++i) {
    const auto at = centre + centred_index({.index = i, .size = size_});
    const float value = at >= 0 && at < length ? signal[static_cast<std::size_t>(at)] : 0.0F;
    buffer_[i] = value * window_[i];
  }
  fft_.forward(buffer_.data(), spectrum_.data());
  for (std::size_t i = 0; i < size_; ++i) {
    const auto at = centre + centred_index({.index = i, .size = size_});
    const float value = at >= 0 && at < length ? signal[static_cast<std::size_t>(at)] : 0.0F;
    buffer_[i] = value * d_window_[i];
  }
  fft_.forward(buffer_.data(), derivative_.data());
  magnitude_[0] = static_cast<float>(std::abs(spectrum_[0]) * scale_);
  magnitude_[size_ / 2] = static_cast<float>(std::abs(spectrum_[1]) * scale_);
  for (std::size_t k = 1; k < size_ / 2; ++k) {
    const double re = spectrum_[2 * k];
    const double im = spectrum_[2 * k + 1];
    magnitude_[k] = static_cast<float>(std::sqrt(re * re + im * im) * scale_);
  }
}

Peak FrameAnalyser::peak(std::size_t bin) const noexcept {
  if (bin == 0 || bin >= size_ / 2)
    return {static_cast<double>(bin), magnitude_[std::min(bin, size_ / 2)], 0.0};
  const double re = spectrum_[2 * bin];
  const double im = spectrum_[2 * bin + 1];
  const double d_re = derivative_[2 * bin];
  const double d_im = derivative_[2 * bin + 1];
  const double power = re * re + im * im;
  if (power <= 0.0)
    return {static_cast<double>(bin), 0.0, 0.0};
  // omega_0 = omega_k - Im(X_dh conj(X_h)) / |X_h|^2, in radians per sample.
  const double offset_radians = -(d_im * re - d_re * im) / power;
  const double offset = std::clamp(offset_radians * static_cast<double>(size_) / two_pi,
                                   -max_reassign_bins, max_reassign_bins);
  return {static_cast<double>(bin) + offset, magnitude_[bin] / lobe(offset), std::atan2(im, re)};
}

void FrameAnalyser::peaks(double floor, std::vector<Peak>& out) const {
  out.clear();
  for (std::size_t k = 1; k + 1 < magnitude_.size(); ++k) {
    const float m = magnitude_[k];
    if (m >= floor && m > magnitude_[k - 1] && m >= magnitude_[k + 1])
      out.push_back(peak(k));
  }
}

EnvelopeEstimator::EnvelopeEstimator(std::size_t size)
    : size_(size), fft_(size), even_(size), cepstrum_(size), target_(size / 2 + 1),
      smoothed_(size / 2 + 1) {}

void EnvelopeEstimator::smooth(std::size_t order) noexcept {
  const std::size_t half = size_ / 2;
  for (std::size_t k = 0; k <= half; ++k)
    even_[k] = static_cast<float>(smoothed_[k]);
  for (std::size_t k = 1; k < half; ++k)
    even_[size_ - k] = even_[k];
  fft_.forward(even_.data(), cepstrum_.data());
  // The real cepstrum of an even sequence is real: c[q] = Re(F[q]) / N.
  const double n = static_cast<double>(size_);
  std::fill_n(even_.data(), size_, 0.0F);
  even_[0] = static_cast<float>(cepstrum_[0] / n);
  for (std::size_t q = 1; q <= order && q < half; ++q) {
    even_[q] = static_cast<float>(cepstrum_[2 * q] / n);
    even_[size_ - q] = even_[q];
  }
  fft_.forward(even_.data(), cepstrum_.data());
  smoothed_[0] = cepstrum_[0];
  smoothed_[half] = cepstrum_[1];
  for (std::size_t k = 1; k < half; ++k)
    smoothed_[k] = cepstrum_[2 * k];
}

void EnvelopeEstimator::estimate(std::span<const float> magnitude, double f0,
                                 std::uint32_t sample_rate, std::span<double> log_envelope) {
  const std::size_t half = size_ / 2;
  for (std::size_t k = 0; k <= half; ++k)
    target_[k] = std::log(std::max(static_cast<double>(magnitude[k]), log_floor));
  const double period = f0 > 0.0 ? static_cast<double>(sample_rate) / f0 : 0.0;
  const auto order =
      std::clamp(static_cast<std::size_t>(period / cepstral_periods), min_cepstral_order, half - 1);
  smoothed_ = target_;
  for (int iteration = 0; iteration < envelope_iterations; ++iteration) {
    smooth(order);
    double above = 0.0;
    for (std::size_t k = 0; k <= half; ++k)
      above = std::max(above, target_[k] - smoothed_[k]);
    if (above < envelope_converged || iteration + 1 == envelope_iterations)
      break;
    for (std::size_t k = 0; k <= half; ++k)
      smoothed_[k] = std::max(target_[k], smoothed_[k]);
  }
  std::copy_n(smoothed_.begin(), std::min(log_envelope.size(), smoothed_.size()),
              log_envelope.begin());
}

double read_bins(std::span<const double> log_envelope, double hz, double bin_hz) noexcept {
  if (log_envelope.empty())
    return 0.0;
  const double last = static_cast<double>(log_envelope.size() - 1);
  const double position = std::clamp(hz / bin_hz, 0.0, last);
  const auto below = std::min(static_cast<std::size_t>(position), log_envelope.size() - 1);
  if (below + 1 >= log_envelope.size())
    return log_envelope[below];
  const double t = position - static_cast<double>(below);
  return (1.0 - t) * log_envelope[below] + t * log_envelope[below + 1];
}

} // namespace oscillate::clay_dsp::detail

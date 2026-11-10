#include "linear_bank.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace oscillate::native_dsp::effects::dsp {
namespace {

constexpr double bank_base_rate = 48'000.0;
constexpr std::size_t bank_base_half = 1'023;
constexpr std::size_t bank_base_hop = 512;
constexpr std::size_t bank_base_fft = 4'096;
// The sizes double once the rate passes 1.5 times the base rate's multiple.
constexpr double bank_rate_margin = 1.5;
// The Blackman window's terms.
constexpr double blackman_a0 = 0.42;
constexpr double blackman_a1 = 0.5;
constexpr double blackman_a2 = 0.08;

} // namespace

BankFrames bank_frames(double sample_rate) noexcept {
  std::size_t scale = 1;
  while (static_cast<double>(scale) * bank_base_rate * bank_rate_margin < sample_rate) {
    scale *= 2;
  }
  return {bank_base_half * scale, bank_base_hop * scale, bank_base_fft * scale};
}

LinearBank::LinearBank(const BankFrames& frames, double sample_rate)
    : rate_(sample_rate), half_(frames.half_length), hop_(frames.hop), size_(frames.fft),
      fft_(frames.fft), history_(frames.fft), work_(frames.fft), lowpass_(frames.fft),
      previous_(frames.fft) {
  for (auto& s : spectra_) {
    s.assign(size_, {});
  }
  for (auto& o : out_) {
    o.assign(hop_, {});
  }
  taps_.assign(2 * half_ + 1, 0.0);
}

void LinearBank::set(std::size_t bands, std::span<const double> crossovers) noexcept {
  bands = std::clamp<std::size_t>(bands, 1, max_bands);
  const std::size_t count = bands - 1;
  if (bands != bands_ ||
      !std::equal(crossovers.begin(), crossovers.begin() + static_cast<std::ptrdiff_t>(count),
                  crossovers_.begin())) {
    bands_ = bands;
    std::copy_n(crossovers.begin(), count, crossovers_.begin());
    dirty_ = true;
  }
}

void LinearBank::reset() noexcept {
  std::ranges::fill(history_, std::complex<double>{});
  for (auto& o : out_) {
    std::ranges::fill(o, std::complex<double>{});
  }
  position_ = 0;
}

void LinearBank::process(double left, double right, std::span<double> out_left,
                         std::span<double> out_right) noexcept {
  for (std::size_t k = 0; k < bands_; ++k) {
    out_left[k] = out_[k][position_].real();
    out_right[k] = out_[k][position_].imag();
  }
  history_[size_ - hop_ + position_] = {left, right};
  if (++position_ == hop_) {
    run();
    position_ = 0;
  }
}

void LinearBank::run() noexcept {
  if (dirty_) {
    design();
  }
  std::ranges::copy(history_, work_.begin());
  fft_.forward(work_);
  for (std::size_t k = 0; k < bands_; ++k) {
    for (std::size_t i = 0; i < size_; ++i) {
      lowpass_[i] = work_[i] * spectra_[k][i];
    }
    fft_.inverse(lowpass_);
    std::copy(lowpass_.begin() + static_cast<std::ptrdiff_t>(size_ - hop_), lowpass_.end(),
              out_[k].begin());
  }
  std::copy(history_.begin() + static_cast<std::ptrdiff_t>(hop_), history_.end(), history_.begin());
}

// Band k = lowpass(k) - lowpass(k - 1); the first band is lowpass(0) and
// the last the delay less lowpass(n - 2), so the bands sum to the delay.
void LinearBank::design() noexcept {
  dirty_ = false;
  const std::size_t n = bands_;
  const auto length = static_cast<double>(taps_.size() - 1);
  for (std::size_t i = 0; i < size_; ++i) {
    previous_[i] = {};
  }
  for (std::size_t k = 0; k + 1 < n; ++k) {
    const double cutoff = 2.0 * crossovers_[k] / rate_;
    double sum = 0.0;
    for (std::size_t m = 0; m < taps_.size(); ++m) {
      const double x = static_cast<double>(m) - static_cast<double>(half_);
      const double sinc =
          x == 0.0 ? 1.0
                   : std::sin(std::numbers::pi * cutoff * x) / (std::numbers::pi * cutoff * x);
      const double phase = 2.0 * std::numbers::pi * static_cast<double>(m) / length;
      const double blackman =
          blackman_a0 - blackman_a1 * std::cos(phase) + blackman_a2 * std::cos(2.0 * phase);
      taps_[m] = cutoff * sinc * blackman;
      sum += taps_[m];
    }
    for (std::size_t i = 0; i < size_; ++i) {
      lowpass_[i] =
          i < taps_.size() ? std::complex<double>{taps_[i] / sum, 0.0} : std::complex<double>{};
    }
    fft_.forward(lowpass_);
    for (std::size_t i = 0; i < size_; ++i) {
      spectra_[k][i] = lowpass_[i] - previous_[i];
      previous_[i] = lowpass_[i];
    }
  }
  for (std::size_t i = 0; i < size_; ++i) {
    const double angle = -2.0 * std::numbers::pi * static_cast<double>(i) *
                         static_cast<double>(half_) / static_cast<double>(size_);
    spectra_[n - 1][i] = std::polar(1.0, angle) - previous_[i];
  }
}

} // namespace oscillate::native_dsp::effects::dsp

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
      fft_(frames.fft), history_{Buffer(frames.fft), Buffer(frames.fft)},
      spectrum_{Buffer(frames.fft), Buffer(frames.fft)}, product_(frames.fft), lowpass_(frames.fft),
      previous_(frames.fft) {
  for (auto& s : spectra_) {
    s = Buffer(size_);
  }
  for (auto& band : out_) {
    for (auto& channel : band) {
      channel.assign(hop_, 0.0);
    }
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
  for (auto& h : history_) {
    h.zero();
  }
  for (auto& band : out_) {
    for (auto& channel : band) {
      std::ranges::fill(channel, 0.0);
    }
  }
  position_ = 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): left then right, as everywhere.
void LinearBank::process(double left, double right, std::span<double> out_left,
                         std::span<double> out_right) noexcept {
  for (std::size_t k = 0; k < bands_; ++k) {
    out_left[k] = out_[k][0][position_];
    out_right[k] = out_[k][1][position_];
  }
  history_[0][size_ - hop_ + position_] = static_cast<float>(left);
  history_[1][size_ - hop_ + position_] = static_cast<float>(right);
  if (++position_ == hop_) {
    run();
    position_ = 0;
  }
}

// Overlap-save: each band's last hop of the circular product is the band.
void LinearBank::run() noexcept {
  if (dirty_) {
    design();
  }
  const std::size_t last = bands_ - 1;
  for (std::size_t c = 0; c < 2; ++c) {
    const float* history = history_[c].data();
    if (last > 0) {
      fft_.forward_unordered(history, spectrum_[c].data());
    }
    // The last band starts as the input half_ frames back.
    auto& rest = out_[last][c];
    for (std::size_t j = 0; j < hop_; ++j) {
      rest[j] = history[size_ - hop_ + j - half_];
    }
    for (std::size_t k = 0; k < last; ++k) {
      // The band spectra carry the inverse's 1 / size_.
      fft_.multiply(spectrum_[c].data(), spectra_[k].data(), product_.data(), 1.0F);
      fft_.inverse_unordered(product_.data(), product_.data());
      const float* band = product_.data() + (size_ - hop_);
      auto& out = out_[k][c];
      for (std::size_t j = 0; j < hop_; ++j) {
        out[j] = band[j];
        rest[j] -= out[j];
      }
    }
    std::copy(history + hop_, history + size_, history_[c].data());
  }
}

// Band k = lowpass(k) - lowpass(k - 1), the first band lowpass(0); the
// last is the rest of the delayed input. The FIRs are designed in double
// and transformed scaled by 1 / size_.
void LinearBank::design() noexcept {
  dirty_ = false;
  const auto length = static_cast<double>(taps_.size() - 1);
  const double scale = 1.0 / static_cast<double>(size_);
  previous_.zero();
  for (std::size_t k = 0; k + 1 < bands_; ++k) {
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
    lowpass_.zero();
    for (std::size_t m = 0; m < taps_.size(); ++m) {
      lowpass_[m] = static_cast<float>(taps_[m] / sum * scale);
    }
    fft_.forward_unordered(lowpass_.data(), lowpass_.data());
    for (std::size_t i = 0; i < size_; ++i) {
      spectra_[k][i] = lowpass_[i] - previous_[i];
      previous_[i] = lowpass_[i];
    }
  }
}

} // namespace oscillate::native_dsp::effects::dsp

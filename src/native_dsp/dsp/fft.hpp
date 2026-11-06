#pragma once

#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

// The one complex FFT of the native DSP and analysis code. Double precision
// throughout: wavetables, analysers and key detection all want it. (Warp
// and convolution keep the pinned PFFFT for their float real transforms.)
namespace oscillate::native_dsp::dsp {

// An in-place radix-2 transform of one power-of-two size. Its tables are
// built on construction (control only); forward() and inverse() allocate
// nothing and are safe on the callback. Each stage's twiddles are computed
// directly, not by recurrence, so long transforms stay accurate.
//
// The butterflies are written out in real arithmetic: a std::complex product
// compiles to a NaN-checking library call, which made this transform about
// four times slower. The operations are the same, so the results are too.
class Fft {
public:
  explicit Fft(std::size_t size)
      : size_(size), cos_(size > 1 ? size - 1 : 0), sin_(size > 1 ? size - 1 : 0), reversed_(size) {
    std::size_t at = 0;
    for (std::size_t length = 2; length <= size; length <<= 1U) {
      const double angle = -2.0 * std::numbers::pi / static_cast<double>(length);
      for (std::size_t k = 0; k < length / 2; ++k) {
        const auto twiddle = std::polar(1.0, angle * static_cast<double>(k));
        cos_[at] = twiddle.real();
        sin_[at++] = twiddle.imag();
      }
    }
    for (std::size_t i = 1, j = 0; i < size; ++i) {
      std::size_t bit = size >> 1U;
      for (; (j & bit) != 0; bit >>= 1U) {
        j ^= bit;
      }
      j ^= bit;
      reversed_[i] = j;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  // X[k] = sum x[n] e^(-2 pi i k n / N), unscaled.
  void forward(std::span<std::complex<double>> data) const noexcept { run(data, 1.0); }
  // The exact inverse: e^(+2 pi i k n / N), scaled by 1 / N.
  void inverse(std::span<std::complex<double>> data) const noexcept {
    run(data, -1.0);
    for (auto& value : data) {
      value /= static_cast<double>(size_);
    }
  }

private:
  // sign is 1 forward and -1 inverse, conjugating the twiddles.
  void run(std::span<std::complex<double>> data, double sign) const noexcept {
    for (std::size_t i = 1; i < size_; ++i) {
      if (i < reversed_[i]) {
        std::swap(data[i], data[reversed_[i]]);
      }
    }
    // std::complex<double> is an array of its real and imaginary parts.
    double* const x = reinterpret_cast<double*>(data.data());
    std::size_t at = 0;
    for (std::size_t length = 2; length <= size_; length <<= 1U) {
      const std::size_t half = length / 2;
      const double* const cos = cos_.data() + at;
      const double* const sin = sin_.data() + at;
      for (std::size_t start = 0; start < size_; start += length) {
        double* const even = x + 2 * start;
        double* const odd = even + 2 * half;
        for (std::size_t k = 0; k < half; ++k) {
          const double wr = cos[k];
          const double wi = sign * sin[k];
          const double odd_r = odd[2 * k];
          const double odd_i = odd[2 * k + 1];
          const double tr = odd_r * wr - odd_i * wi;
          const double ti = odd_r * wi + odd_i * wr;
          const double even_r = even[2 * k];
          const double even_i = even[2 * k + 1];
          even[2 * k] = even_r + tr;
          even[2 * k + 1] = even_i + ti;
          odd[2 * k] = even_r - tr;
          odd[2 * k + 1] = even_i - ti;
        }
      }
      at += half;
    }
  }

  std::size_t size_;
  std::vector<double> cos_;
  std::vector<double> sin_;
  std::vector<std::size_t> reversed_;
};

} // namespace oscillate::native_dsp::dsp

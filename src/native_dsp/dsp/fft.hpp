#pragma once

#include <algorithm>
#include <complex>
#include <cstddef>
#include <emmintrin.h>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

// The one complex FFT of the native DSP and analysis code. Double precision
// throughout: wavetables, analysers and key detection all want it. (Warp
// and Cast keep the pinned PFFFT for their float real transforms.)
namespace oscillate::native_dsp::dsp {

// An in-place radix-2 transform of one power-of-two size. Its tables are
// built on construction (control only); forward() and inverse() allocate
// nothing and are safe on the callback. Each stage's twiddles are computed
// directly, not by recurrence, so long transforms stay accurate.
//
// The butterflies are written out in SSE2 arithmetic: a std::complex product
// compiles to a NaN-checking library call, which made this transform about
// four times slower. A whole transform runs the stages two at a time, each
// group of four values through both before the next (the same butterflies
// on the same values, in half the passes), and reorders by a list of its
// swaps. The operations are the same, so the results are too.
class Fft {
public:
  explicit Fft(std::size_t size)
      : size_(size), cos_(size > 1 ? size - 1 : 0), sin_(size > 1 ? size - 1 : 0) {
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
      if (i < j) {
        swaps_.emplace_back(i, j);
      }
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  // X[k] = sum x[n] e^(-2 pi i k n / N), unscaled.
  void forward(std::span<std::complex<double>> data) const noexcept { run(data, 1.0); }
  // The exact inverse: e^(+2 pi i k n / N), scaled by 1 / N. N is a power
  // of two, so multiplying by its reciprocal is the division, bit for bit.
  void inverse(std::span<std::complex<double>> data) const noexcept {
    run(data, -1.0);
    const __m128d scale = _mm_set1_pd(1.0 / static_cast<double>(size_));
    double* const x = reinterpret_cast<double*>(data.data());
    for (std::size_t i = 0; i < data.size(); ++i) {
      _mm_storeu_pd(x + 2 * i, _mm_mul_pd(_mm_loadu_pd(x + 2 * i), scale));
    }
  }

  // The forward transform in steps, so one transform can be spread over
  // many callbacks: reorder(), then every stage in order, each whole or in
  // pieces of its size / 2 butterflies in any order. The result is
  // forward()'s, bit for bit. reorder() too may run in pieces.
  [[nodiscard]] std::size_t stages() const noexcept {
    std::size_t count = 0;
    for (std::size_t length = 2; length <= size_; length <<= 1U) {
      ++count;
    }
    return count;
  }
  void reorder(std::span<std::complex<double>> data) const noexcept { reorder(data, 0, size_); }
  // The reordering of indices [first, last) only (each swap is made from
  // its lower index).
  void reorder(std::span<std::complex<double>> data, std::size_t first,
               std::size_t last) const noexcept {
    // The swaps are in order of their lower index.
    auto swap = std::ranges::lower_bound(swaps_, first, {}, &Swap::first);
    for (; swap != swaps_.end() && swap->first < last; ++swap) {
      std::swap(data[swap->first], data[swap->second]);
    }
  }
  // Butterflies [first, last) of stage `stage` (0 pairs neighbours).
  void stage(std::span<std::complex<double>> data, std::size_t stage, std::size_t first,
             std::size_t last) const noexcept {
    butterflies(data, stage, first, last, 1.0);
  }

private:
  using Swap = std::pair<std::size_t, std::size_t>;

  // The odd value of a butterfly times its twiddle, one complex value per
  // SSE2 register: (o_r w_r - o_i w_i, o_i w_r + o_r w_i), the scalar
  // products and sums exactly (a sum's order does not change it; a
  // difference is the sum with the sign flipped).
  [[nodiscard]] static __m128d twiddled(__m128d odd, double w_r, double w_i) noexcept {
    // Flips the sign of a register's real (low) half.
    const __m128d negate_real = _mm_set_pd(0.0, -0.0);
    const __m128d direct = _mm_mul_pd(odd, _mm_set1_pd(w_r));
    const __m128d crossed = _mm_mul_pd(_mm_shuffle_pd(odd, odd, 1), _mm_set1_pd(w_i));
    return _mm_add_pd(direct, _mm_xor_pd(crossed, negate_real));
  }

  // One butterfly in place: even + t, even - t.
  static void butterfly(double* even, double* odd, double w_r, double w_i) noexcept {
    const __m128d t = twiddled(_mm_loadu_pd(odd), w_r, w_i);
    const __m128d e = _mm_loadu_pd(even);
    _mm_storeu_pd(even, _mm_add_pd(e, t));
    _mm_storeu_pd(odd, _mm_sub_pd(e, t));
  }

  // sign is 1 forward and -1 inverse, conjugating the twiddles.
  void run(std::span<std::complex<double>> data, double sign) const noexcept {
    reorder(data);
    const std::size_t count = stages();
    std::size_t s = 0;
    for (; s + 1 < count; s += 2) {
      stage_pair(data, s, sign);
    }
    if (s < count) {
      butterflies(data, s, 0, size_ / 2, sign);
    }
  }

  // Stages s and s + 1 together: in each group of four values a, b, c, d
  // (half apart), stage s's butterflies (a, b) and (c, d) at one twiddle,
  // then stage s + 1's (a, c) and (b, d) at theirs.
  void stage_pair(std::span<std::complex<double>> data, std::size_t stage,
                  double sign) const noexcept {
    const std::size_t half = std::size_t{1} << stage;
    double* const x = reinterpret_cast<double*>(data.data());
    // A stage's twiddles follow those of the stages before it.
    const double* const cos_first = cos_.data() + half - 1;
    const double* const sin_first = sin_.data() + half - 1;
    const double* const cos_second = cos_.data() + 2 * half - 1;
    const double* const sin_second = sin_.data() + 2 * half - 1;
    // Doubles from one value to the next of its group.
    const std::size_t step = 2 * half;
    for (std::size_t base = 0; base < size_; base += 4 * half) {
      for (std::size_t k = 0; k < half; ++k) {
        double* const a = x + 2 * (base + k);
        double* const b = a + step;
        double* const c = b + step;
        double* const d = c + step;
        const double first_r = cos_first[k];
        const double first_i = sign * sin_first[k];
        const __m128d a0 = _mm_loadu_pd(a);
        const __m128d c0 = _mm_loadu_pd(c);
        const __m128d t_ab = twiddled(_mm_loadu_pd(b), first_r, first_i);
        const __m128d t_cd = twiddled(_mm_loadu_pd(d), first_r, first_i);
        const __m128d a1 = _mm_add_pd(a0, t_ab);
        const __m128d b1 = _mm_sub_pd(a0, t_ab);
        const __m128d c1 = _mm_add_pd(c0, t_cd);
        const __m128d d1 = _mm_sub_pd(c0, t_cd);
        const __m128d t_ac = twiddled(c1, cos_second[k], sign * sin_second[k]);
        const __m128d t_bd = twiddled(d1, cos_second[k + half], sign * sin_second[k + half]);
        _mm_storeu_pd(a, _mm_add_pd(a1, t_ac));
        _mm_storeu_pd(c, _mm_sub_pd(a1, t_ac));
        _mm_storeu_pd(b, _mm_add_pd(b1, t_bd));
        _mm_storeu_pd(d, _mm_sub_pd(b1, t_bd));
      }
    }
  }

  void butterflies(std::span<std::complex<double>> data, std::size_t stage, std::size_t first,
                   std::size_t last, double sign) const noexcept {
    const std::size_t half = std::size_t{1} << stage;
    const std::size_t length = 2 * half;
    // The twiddles of stage s follow those of the stages before it.
    const std::size_t at = half - 1;
    // std::complex<double> is an array of its real and imaginary parts.
    double* const x = reinterpret_cast<double*>(data.data());
    const double* const cos = cos_.data() + at;
    const double* const sin = sin_.data() + at;
    std::size_t block = first / half;
    std::size_t k_first = first - block * half;
    std::size_t remaining = last > first ? last - first : 0;
    while (remaining > 0) {
      const std::size_t k_last = std::min(half, k_first + remaining);
      double* const even = x + 2 * block * length;
      double* const odd = even + 2 * half;
      for (std::size_t k = k_first; k < k_last; ++k) {
        butterfly(even + 2 * k, odd + 2 * k, cos[k], sign * sin[k]);
      }
      remaining -= k_last - k_first;
      ++block;
      k_first = 0;
    }
  }

  std::size_t size_;
  std::vector<double> cos_;
  std::vector<double> sin_;
  std::vector<Swap> swaps_;
};

} // namespace oscillate::native_dsp::dsp

#pragma once

#include <emmintrin.h>

// Two doubles in one SSE2 register, for two independent lanes (a stereo
// pair, two delay lines) through the same code at once. Each lane performs
// exactly the scalar operation: SSE2 double arithmetic, which baseline
// x86-64 never contracts, so a paired loop gives the scalar loop's results
// to the bit.
namespace oscillate::native_dsp::dsp {

struct Pair {
  __m128d v;
  Pair(__m128d value) noexcept : v(value) {}
  Pair(double first, double second) noexcept : v(_mm_set_pd(second, first)) {}
  [[nodiscard]] static Pair both(double value) noexcept { return _mm_set1_pd(value); }
  [[nodiscard]] double first() const noexcept { return _mm_cvtsd_f64(v); }
  [[nodiscard]] double second() const noexcept { return _mm_cvtsd_f64(_mm_unpackhi_pd(v, v)); }
};
[[nodiscard]] inline Pair operator+(Pair a, Pair b) noexcept { return _mm_add_pd(a.v, b.v); }
[[nodiscard]] inline Pair operator-(Pair a, Pair b) noexcept { return _mm_sub_pd(a.v, b.v); }
[[nodiscard]] inline Pair operator*(Pair a, Pair b) noexcept { return _mm_mul_pd(a.v, b.v); }
[[nodiscard]] inline Pair operator/(Pair a, Pair b) noexcept { return _mm_div_pd(a.v, b.v); }
[[nodiscard]] inline Pair operator+(double a, Pair b) noexcept {
  return _mm_add_pd(_mm_set1_pd(a), b.v);
}
[[nodiscard]] inline Pair operator-(double a, Pair b) noexcept {
  return _mm_sub_pd(_mm_set1_pd(a), b.v);
}
[[nodiscard]] inline Pair operator*(double a, Pair b) noexcept {
  return _mm_mul_pd(_mm_set1_pd(a), b.v);
}
[[nodiscard]] inline Pair operator*(Pair a, double b) noexcept {
  return _mm_mul_pd(a.v, _mm_set1_pd(b));
}
[[nodiscard]] inline Pair operator/(Pair a, double b) noexcept {
  return _mm_div_pd(a.v, _mm_set1_pd(b));
}
// As the scalar clamp_symmetric for finite x, -0.0 included: max keeps its
// first operand unless the second is greater, min unless it is smaller.
[[nodiscard]] inline Pair clamp_symmetric(Pair x, double limit) noexcept {
  return _mm_min_pd(_mm_max_pd(x.v, _mm_set1_pd(-limit)), _mm_set1_pd(limit));
}

} // namespace oscillate::native_dsp::dsp

#pragma once

#include <array>
#include <cstddef>
#include <emmintrin.h>
#include <span>

// e^x, ln x and the sine and cosine of four floats at once in SSE2, for
// spectra on the audio thread (Temper's filter design, ADR-0126). They are
// Cephes's single-precision approximations (S. L. Moshier): a range
// reduction exact in float, then a minimax polynomial, within a few units
// in the last place of the C library's float functions over the ranges
// below. Arguments must be finite.
namespace oscillate::native_dsp::dsp::vector_math {

namespace detail {

// e^x: x = n ln 2 + r, ln 2 split in two so n ln 2 is exact in float.
inline constexpr float exp_high = 88.3762626647949F; // e^x overflows past this,
inline constexpr float exp_low = -88.3762626647949F; // and is denormal below.
inline constexpr float log2_e = 1.44269504088896341F;
inline constexpr float ln2_upper = 0.693359375F;
inline constexpr float ln2_lower = -2.12194440e-4F;
// e^r - 1 - r over r^2, |r| <= ln 2 / 2, highest power first.
inline constexpr std::array<float, 6> exp_series{1.9875691500E-4F, 1.3981999507E-3F,
                                                 8.3334519073E-3F, 4.1665795894E-2F,
                                                 1.6666665459E-1F, 5.0000001201E-1F};
// The float exponent's bias and the mantissa's width.
inline constexpr int exponent_bias = 127;
inline constexpr int mantissa_bits = 23;

// ln x: x = m 2^e with m in [sqrt(1/2), sqrt(2)), then ln(1 + (m - 1)).
inline constexpr float smallest_normal = 1.17549435e-38F;
inline constexpr float sqrt_half = 0.707106781186547524F;
// (ln(1 + x) - x + x^2 / 2) / x^3, highest power first.
inline constexpr std::array<float, 9> log_series{
    7.0376836292E-2F,  -1.1514610310E-1F, 1.1676998740E-1F,  -1.2420140846E-1F, 1.4249322787E-1F,
    -1.6668057665E-1F, 2.0000714765E-1F,  -2.4999993993E-1F, 3.3333331174E-1F};
// The exponent read from the bits is one more than e, m being in [1/2, 1).
inline constexpr int log_exponent_bias = 126;

// sin and cos: x = j pi / 4 + r, pi / 4 split in three so j pi / 4 is exact
// in float for |x| < 8192; then the octant picks the polynomial and sign.
inline constexpr float four_over_pi = 1.27323954473516F;
inline constexpr float pi_4_upper = -0.78515625F;
inline constexpr float pi_4_middle = -2.4187564849853515625e-4F;
inline constexpr float pi_4_lower = -3.77489497744594108e-8F;
// sin r = r + r^3 (s0 r^4 + s1 r^2 + s2), |r| <= pi / 4.
inline constexpr std::array<float, 3> sin_series{-1.9515295891E-4F, 8.3321608736E-3F,
                                                 -1.6666654611E-1F};
// cos r = 1 - r^2 / 2 + r^4 (c0 r^4 + c1 r^2 + c2).
inline constexpr std::array<float, 3> cos_series{2.443315711809948E-005F, -1.388731625493765E-003F,
                                                 4.166664568298827E-002F};
// The octant's bits: 2 picks the polynomial, 4 the sign.
inline constexpr int octant_polynomial = 2;
inline constexpr int octant_sign = 4;
// Shifts an octant's sign bit (4) to the float's sign bit (31).
inline constexpr int octant_sign_shift = 29;

template <std::size_t N>
[[nodiscard]] inline __m128 polynomial(const std::array<float, N>& series, __m128 x) noexcept {
  __m128 y = _mm_set1_ps(series[0]);
  for (std::size_t i = 1; i < N; ++i) {
    y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(series[i]));
  }
  return y;
}

// floor(x) for |x| < 2^31 (SSE2 has no rounding instruction).
[[nodiscard]] inline __m128 floor(__m128 x) noexcept {
  const __m128 truncated = _mm_cvtepi32_ps(_mm_cvttps_epi32(x));
  const __m128 above = _mm_cmpgt_ps(truncated, x);
  return _mm_sub_ps(truncated, _mm_and_ps(above, _mm_set1_ps(1.0F)));
}

} // namespace detail

// e^x, flushed to zero below e^-88.
[[nodiscard]] inline __m128 exp(__m128 x) noexcept {
  using namespace detail;
  x = _mm_min_ps(_mm_max_ps(x, _mm_set1_ps(exp_low)), _mm_set1_ps(exp_high));
  // n = round(x / ln 2).
  const __m128 n = detail::floor(_mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(log2_e)), _mm_set1_ps(0.5F)));
  x = _mm_sub_ps(x, _mm_mul_ps(n, _mm_set1_ps(ln2_upper)));
  x = _mm_sub_ps(x, _mm_mul_ps(n, _mm_set1_ps(ln2_lower)));
  const __m128 square = _mm_mul_ps(x, x);
  __m128 y = _mm_mul_ps(polynomial(exp_series, x), square);
  y = _mm_add_ps(_mm_add_ps(y, x), _mm_set1_ps(1.0F));
  // 2^n from its exponent bits.
  const __m128i bits = _mm_slli_epi32(
      _mm_add_epi32(_mm_cvttps_epi32(n), _mm_set1_epi32(exponent_bias)), mantissa_bits);
  return _mm_mul_ps(y, _mm_castsi128_ps(bits));
}

// ln x for x > 0; ln of the smallest normal float below it.
[[nodiscard]] inline __m128 log(__m128 x) noexcept {
  using namespace detail;
  x = _mm_max_ps(x, _mm_set1_ps(smallest_normal));
  const __m128i bits = _mm_castps_si128(x);
  __m128 e = _mm_cvtepi32_ps(
      _mm_sub_epi32(_mm_srli_epi32(bits, mantissa_bits), _mm_set1_epi32(log_exponent_bias)));
  // The mantissa as a float in [1/2, 1).
  const __m128i mantissa_mask = _mm_set1_epi32((1 << mantissa_bits) - 1);
  const __m128i half_bits = _mm_castps_si128(_mm_set1_ps(0.5F));
  __m128 m = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(bits, mantissa_mask), half_bits));
  // Below sqrt(1/2), take 2m and one less from the exponent.
  const __m128 low = _mm_cmplt_ps(m, _mm_set1_ps(sqrt_half));
  const __m128 one = _mm_set1_ps(1.0F);
  e = _mm_sub_ps(e, _mm_and_ps(one, low));
  m = _mm_add_ps(_mm_sub_ps(m, one), _mm_and_ps(m, low));
  const __m128 square = _mm_mul_ps(m, m);
  __m128 y = _mm_mul_ps(_mm_mul_ps(polynomial(log_series, m), m), square);
  y = _mm_add_ps(y, _mm_mul_ps(e, _mm_set1_ps(ln2_lower)));
  y = _mm_sub_ps(y, _mm_mul_ps(square, _mm_set1_ps(0.5F)));
  return _mm_add_ps(_mm_add_ps(m, y), _mm_mul_ps(e, _mm_set1_ps(ln2_upper)));
}

// sin x and cos x together, for |x| < 8192.
inline void sincos(__m128 x, __m128& sine, __m128& cosine) noexcept {
  using namespace detail;
  const __m128 sign_mask = _mm_set1_ps(-0.0F);
  __m128 sin_sign = _mm_and_ps(x, sign_mask);
  x = _mm_andnot_ps(sign_mask, x);
  // The octant j, rounded up to even, and the remainder r.
  __m128i j = _mm_cvttps_epi32(_mm_mul_ps(x, _mm_set1_ps(four_over_pi)));
  j = _mm_and_si128(_mm_add_epi32(j, _mm_set1_epi32(1)), _mm_set1_epi32(~1));
  const __m128 y = _mm_cvtepi32_ps(j);
  x = _mm_add_ps(x, _mm_mul_ps(y, _mm_set1_ps(pi_4_upper)));
  x = _mm_add_ps(x, _mm_mul_ps(y, _mm_set1_ps(pi_4_middle)));
  x = _mm_add_ps(x, _mm_mul_ps(y, _mm_set1_ps(pi_4_lower)));
  // Octants 2 and 6 (mod 8) swap the polynomials.
  const __m128 swap = _mm_castsi128_ps(
      _mm_cmpeq_epi32(_mm_and_si128(j, _mm_set1_epi32(octant_polynomial)), _mm_setzero_si128()));
  sin_sign =
      _mm_xor_ps(sin_sign, _mm_castsi128_ps(_mm_slli_epi32(
                               _mm_and_si128(j, _mm_set1_epi32(octant_sign)), octant_sign_shift)));
  const __m128 cos_sign = _mm_castsi128_ps(
      _mm_slli_epi32(_mm_andnot_si128(_mm_sub_epi32(j, _mm_set1_epi32(octant_polynomial)),
                                      _mm_set1_epi32(octant_sign)),
                     octant_sign_shift));
  const __m128 square = _mm_mul_ps(x, x);
  // cos r.
  __m128 c = _mm_mul_ps(_mm_mul_ps(polynomial(cos_series, square), square), square);
  c = _mm_add_ps(_mm_sub_ps(c, _mm_mul_ps(square, _mm_set1_ps(0.5F))), _mm_set1_ps(1.0F));
  // sin r.
  __m128 s = _mm_mul_ps(_mm_mul_ps(polynomial(sin_series, square), square), x);
  s = _mm_add_ps(s, x);
  // swap is all ones where sin x = +-sin r and cos x = +-cos r.
  const __m128 sin_part = _mm_or_ps(_mm_and_ps(swap, s), _mm_andnot_ps(swap, c));
  const __m128 cos_part = _mm_or_ps(_mm_and_ps(swap, c), _mm_andnot_ps(swap, s));
  sine = _mm_xor_ps(sin_part, sin_sign);
  cosine = _mm_xor_ps(cos_part, cos_sign);
}

// e^z for each complex value of an interleaved array (real, imaginary, ...)
// in place: e^re (cos im, sin im). Its size is a multiple of four floats.
inline void complex_exp(std::span<float> interleaved) noexcept {
  constexpr std::size_t floats_per_step = 8; // Four complex values.
  constexpr int pick_even = _MM_SHUFFLE(2, 0, 2, 0);
  constexpr int pick_odd = _MM_SHUFFLE(3, 1, 3, 1);
  std::size_t i = 0;
  for (; i + floats_per_step <= interleaved.size(); i += floats_per_step) {
    const __m128 first = _mm_loadu_ps(interleaved.data() + i);
    const __m128 second = _mm_loadu_ps(interleaved.data() + i + floats_per_step / 2);
    const __m128 re = _mm_shuffle_ps(first, second, pick_even);
    const __m128 im = _mm_shuffle_ps(first, second, pick_odd);
    __m128 sine;
    __m128 cosine;
    sincos(im, sine, cosine);
    const __m128 magnitude = exp(re);
    const __m128 out_re = _mm_mul_ps(magnitude, cosine);
    const __m128 out_im = _mm_mul_ps(magnitude, sine);
    _mm_storeu_ps(interleaved.data() + i, _mm_unpacklo_ps(out_re, out_im));
    _mm_storeu_ps(interleaved.data() + i + floats_per_step / 2, _mm_unpackhi_ps(out_re, out_im));
  }
  if (i < interleaved.size()) {
    // The last two complex values.
    const __m128 pair = _mm_loadu_ps(interleaved.data() + i);
    const __m128 re = _mm_shuffle_ps(pair, pair, pick_even);
    const __m128 im = _mm_shuffle_ps(pair, pair, pick_odd);
    __m128 sine;
    __m128 cosine;
    sincos(im, sine, cosine);
    const __m128 magnitude = exp(re);
    _mm_storeu_ps(interleaved.data() + i,
                  _mm_unpacklo_ps(_mm_mul_ps(magnitude, cosine), _mm_mul_ps(magnitude, sine)));
  }
}

// e^x and ln x of every value in place. Any size.
inline void exp_all(std::span<float> values) noexcept {
  constexpr std::size_t lanes = 4;
  std::size_t i = 0;
  for (; i + lanes <= values.size(); i += lanes) {
    _mm_storeu_ps(values.data() + i, exp(_mm_loadu_ps(values.data() + i)));
  }
  for (; i < values.size(); ++i) {
    values[i] = _mm_cvtss_f32(exp(_mm_set_ss(values[i])));
  }
}
inline void log_all(std::span<float> values) noexcept {
  constexpr std::size_t lanes = 4;
  std::size_t i = 0;
  for (; i + lanes <= values.size(); i += lanes) {
    _mm_storeu_ps(values.data() + i, log(_mm_loadu_ps(values.data() + i)));
  }
  for (; i < values.size(); ++i) {
    values[i] = _mm_cvtss_f32(log(_mm_set_ss(values[i])));
  }
}

} // namespace oscillate::native_dsp::dsp::vector_math

#pragma once

// Named comparison tolerances. Pick the one whose meaning matches the check;
// a check that needs a different bound names its own constant with a comment
// saying why. Float variants (suffix _f) keep a float comparison in float:
// widening 1e-5F to the double 1e-5 would move the bound.

namespace oscillate::test::tolerance {

// Bit-identical output: two renders of the same input, a copy, a round trip
// through an exact format.
inline constexpr double bit_exact = 0.0;
inline constexpr float bit_exact_f = 0.0F;

// Rounding noise of a few double operations (an exact formula recomputed in
// a different order).
inline constexpr double double_rounding = 1e-12;

// Double results after long accumulation or iteration (sums over thousands of
// samples, filter recurrences, solver convergence).
inline constexpr double double_accumulation = 1e-9;

// A value that went through float once: a stored parameter, a float buffer,
// float <-> double round trips.
inline constexpr double float_round_trip = 1e-6;
inline constexpr float float_round_trip_f = 1e-6F;

// Float DSP output against a double oracle after a block of processing.
inline constexpr double float_processing = 1e-5;
inline constexpr float float_processing_f = 1e-5F;

// Signal comparisons where -80 dB of difference is inaudible (resampled,
// interpolated or crossfaded audio against its oracle).
inline constexpr double near_silence = 1e-4;
inline constexpr float near_silence_f = 1e-4F;

// Coarse audio checks: -60 dB, a millisecond, a thousandth of a unit.
inline constexpr double coarse = 1e-3;
inline constexpr float coarse_f = 1e-3F;

} // namespace oscillate::test::tolerance

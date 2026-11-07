#pragma once

#include <string>
#include <string_view>

// The one rule for every number the user interface shows: a true minus sign
// (U+2212), never a negative zero, one decimals rule by magnitude and unit,
// and one spelling per unit ("ct" for cents, "°" for degrees, "♯" in note
// names, "%" set apart from its number like every other unit). Panels and
// parameter tables format through it rather than printing numbers
// themselves.
namespace oscillate::foundation {

// U+2212 MINUS SIGN, U+221E INFINITY, U+266F MUSIC SHARP SIGN and U+266D
// MUSIC FLAT SIGN, in UTF-8.
inline constexpr std::string_view minus_sign = "\xE2\x88\x92";
inline constexpr std::string_view infinity_sign = "\xE2\x88\x9E";
inline constexpr std::string_view sharp_sign = "\xE2\x99\xAF";
inline constexpr std::string_view flat_sign = "\xE2\x99\xAD";

// Units the formatter spells itself. A parameter table's unit string is one
// of these or any other short word, which follows its number after a space.
inline constexpr std::string_view decibels_unit = "dB";
inline constexpr std::string_view hertz_unit = "Hz";
inline constexpr std::string_view kilohertz_unit = "kHz";
inline constexpr std::string_view seconds_unit = "s";
inline constexpr std::string_view milliseconds_unit = "ms";
inline constexpr std::string_view percent_unit = "%";
inline constexpr std::string_view cents_unit = "ct";
inline constexpr std::string_view semitones_unit = "st";
inline constexpr std::string_view degrees_unit = "\xC2\xB0";
inline constexpr std::string_view times_unit = "\xC3\x97";

// A positive number's sign: none, or "+" for values that are offsets
// (cents, semitones, gain changes). Zero never has a sign.
enum class ValueSign : unsigned char { plain, explicit_plus };

// The decimals the rule gives a magnitude with no unit of its own: two
// below 10, one below 100, none from 100 up.
inline constexpr int automatic_decimals = -1;
[[nodiscard]] int display_decimals(double magnitude) noexcept;

// `value` to `decimals` places (automatic_decimals: by magnitude), with a
// minus sign when it rounds below zero. Non-finite values read "∞" or
// "−∞"; NaN reads "–" (an en dash: no value).
[[nodiscard]] std::string format_number(double value, int decimals = automatic_decimals,
                                        ValueSign sign = ValueSign::plain);

// `value` in `unit` with the unit's own decimals and spelling:
//   dB, dBFS, LU, LUFS: one decimal; -∞ for −infinity.
//   Hz: kHz from 1000 Hz. s: ms below one second. ms: s from 1000 ms.
//   %, ct, °: whole numbers. %, ct and words follow after a space; °
//   and × follow directly. "deg", "x", "cents" and "¢" are read as °, ×
//   and ct.
// `decimals` overrides the unit's rule (automatic_decimals keeps it).
[[nodiscard]] std::string format_quantity(double value, std::string_view unit,
                                          ValueSign sign = ValueSign::plain,
                                          int decimals = automatic_decimals);

} // namespace oscillate::foundation

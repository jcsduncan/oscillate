#include "oscillate/foundation/value_text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <system_error>

namespace oscillate::foundation {
namespace {

// The magnitudes where the rule drops a decimal, and its most decimals.
constexpr double one_decimal_from = 10.0;
constexpr double no_decimals_from = 100.0;
constexpr int most_decimals = 2;
// The most places a caller may ask for; more is noise in any readout.
constexpr int max_decimals = 6;
// Level units: one decimal always.
constexpr int level_decimals = 1;
// Whole-number units.
constexpr int whole_decimals = 0;
// Hz from here on reads in kHz, and s below here in ms.
constexpr double hertz_per_kilohertz = 1'000.0;
constexpr double milliseconds_per_second = 1'000.0;
// U+2013 EN DASH: a reading with no value.
constexpr std::string_view no_value = "\xE2\x80\x93";

constexpr std::array<std::string_view, 5> level_units{"dB", "dBFS", "dBTP", "LU", "LUFS"};

bool is_level(std::string_view unit) noexcept {
  return std::ranges::find(level_units, unit) != level_units.end();
}

// The unit's one spelling.
std::string_view spelled(std::string_view unit) noexcept {
  if (unit == "deg")
    return degrees_unit;
  if (unit == "x")
    return times_unit;
  if (unit == "cents" || unit == "\xC2\xA2" || unit == "c")
    return cents_unit;
  return unit;
}

// Room for the digits of any finite double in fixed notation.
constexpr std::size_t digits_room = 400;

// Digits of a non-negative magnitude to `decimals` places, with a full
// stop whatever the process's locale.
std::string digits(double magnitude, int decimals) {
  std::array<char, digits_room> text{};
  const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), magnitude,
                                          std::chars_format::fixed, decimals);
  if (error != std::errc{})
    return std::string(no_value);
  return {text.data(), end};
}

double parsed(const std::string& text) {
  double value = 0.0;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

bool reads_as_zero(std::string_view text) noexcept {
  return text.find_first_not_of("0.") == std::string_view::npos;
}

} // namespace

int display_decimals(double magnitude) noexcept {
  magnitude = std::fabs(magnitude);
  if (magnitude >= no_decimals_from)
    return 0;
  if (magnitude >= one_decimal_from)
    return 1;
  return most_decimals;
}

// The value and its decimals are distinct types callers name at each call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::string format_number(double value, int decimals, ValueSign sign) {
  if (std::isnan(value))
    return std::string(no_value);
  if (std::isinf(value))
    return value < 0.0 ? std::string(minus_sign) + std::string(infinity_sign)
                       : std::string(infinity_sign);
  const double magnitude = std::fabs(value);
  std::string text;
  if (decimals == automatic_decimals) {
    // Rounding can carry a value over a boundary (99.96 -> 100.0); the
    // rounded reading decides its own decimals.
    const int first = display_decimals(magnitude);
    text = digits(magnitude, first);
    const int settled = display_decimals(parsed(text));
    if (settled != first)
      text = digits(magnitude, settled);
  } else {
    text = digits(magnitude, std::clamp(decimals, 0, max_decimals));
  }
  if (reads_as_zero(text))
    return text;
  if (value < 0.0)
    return std::string(minus_sign) + text;
  return sign == ValueSign::explicit_plus ? "+" + text : text;
}

std::string format_quantity(double value, std::string_view unit, ValueSign sign, int decimals) {
  unit = spelled(unit);
  // The number and the unit's decimals are named at each call below.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  const auto with = [&](double number, int unit_decimals, std::string_view shown) {
    const int places = decimals == automatic_decimals ? unit_decimals : decimals;
    std::string text = format_number(number, places, sign);
    if (shown.empty())
      return text;
    if (shown != degrees_unit && shown != times_unit)
      text += ' ';
    return text + std::string(shown);
  };
  if (is_level(unit))
    return with(value, level_decimals, unit);
  if (unit == hertz_unit && std::fabs(value) >= hertz_per_kilohertz)
    return with(value / hertz_per_kilohertz, automatic_decimals, kilohertz_unit);
  if (unit == seconds_unit && std::fabs(value) * milliseconds_per_second < milliseconds_per_second)
    return with(value * milliseconds_per_second, automatic_decimals, milliseconds_unit);
  if (unit == milliseconds_unit && std::fabs(value) >= milliseconds_per_second)
    return with(value / milliseconds_per_second, automatic_decimals, seconds_unit);
  if (unit == percent_unit || unit == cents_unit || unit == degrees_unit)
    return with(value, whole_decimals, unit);
  return with(value, automatic_decimals, unit);
}

} // namespace oscillate::foundation

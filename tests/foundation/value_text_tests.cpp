#include "oscillate/foundation/value_text.hpp"
#include "oscillate/test/check.hpp"

#include <iostream>
#include <limits>
#include <string>
#include <string_view>

// The display formatter: minus signs, negative zero, the decimals rule and
// each unit's spelling, against hand-written expected text.
namespace {
namespace foundation = oscillate::foundation;
using foundation::format_number;
using foundation::format_quantity;
using foundation::ValueSign;

// U+2212, spelled out so the expectations below read as the user sees them.
const std::string minus = "\xE2\x88\x92";
constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();

void minus_sign_and_negative_zero() {
  REQUIRE_EQ(format_number(-6.0, 1), minus + "6.0", "a negative number takes U+2212");
  REQUIRE_EQ(format_number(-0.0, 1), "0.0", "negative zero reads as zero");
  REQUIRE_EQ(format_number(-0.04, 1), "0.0", "a value that rounds to zero has no sign");
  REQUIRE_EQ(format_number(-0.04, 1, ValueSign::explicit_plus), "0.0",
             "nor a plus when one is asked for");
  REQUIRE_EQ(format_number(0.04, 1, ValueSign::explicit_plus), "0.0", "zero never has a sign");
  REQUIRE_EQ(format_number(0.06, 1, ValueSign::explicit_plus), "+0.1",
             "an offset above zero takes a plus");
  REQUIRE_EQ(format_number(-0.06, 1), minus + "0.1", "below zero after rounding keeps its minus");
  REQUIRE_EQ(format_quantity(-0.01, "dB"), "0.0 dB", "no \"-0.0 dB\"");
  const auto text = format_quantity(-12.5, "dB");
  REQUIRE_TRUE(text.find('-') == std::string::npos, "no hyphen-minus anywhere");
}

void decimals_rule() {
  REQUIRE_EQ(foundation::display_decimals(0.5), 2);
  REQUIRE_EQ(foundation::display_decimals(-9.99), 2, "the rule reads the magnitude");
  REQUIRE_EQ(foundation::display_decimals(10.0), 1);
  REQUIRE_EQ(foundation::display_decimals(99.9), 1);
  REQUIRE_EQ(foundation::display_decimals(100.0), 0);
  REQUIRE_EQ(format_number(1.234), "1.23");
  REQUIRE_EQ(format_number(12.34), "12.3");
  REQUIRE_EQ(format_number(123.4), "123");
  REQUIRE_EQ(format_number(99.96), "100", "rounding over a boundary takes the next rule");
  REQUIRE_EQ(format_number(9.996), "10.0");
  REQUIRE_EQ(format_number(2.6, 0), "3", "explicit decimals override the rule");
  REQUIRE_EQ(format_number(infinity), "\xE2\x88\x9E");
  REQUIRE_EQ(format_number(-infinity), minus + "\xE2\x88\x9E");
  REQUIRE_EQ(format_number(not_a_number), "\xE2\x80\x93", "no value reads as an en dash");
}

void units() {
  // Levels: one decimal, always.
  REQUIRE_EQ(format_quantity(-6.0, "dB"), minus + "6.0 dB");
  REQUIRE_EQ(format_quantity(-14.04, "LUFS"), minus + "14.0 LUFS");
  REQUIRE_EQ(format_quantity(3.0, "dB", ValueSign::explicit_plus), "+3.0 dB");
  REQUIRE_EQ(format_quantity(-infinity, "dB"), minus + "\xE2\x88\x9E dB");
  // Frequency: kHz from 1000 Hz, its case kept.
  REQUIRE_EQ(format_quantity(440.0, "Hz"), "440 Hz");
  REQUIRE_EQ(format_quantity(55.0, "Hz"), "55.0 Hz");
  REQUIRE_EQ(format_quantity(1'200.0, "Hz"), "1.20 kHz");
  REQUIRE_EQ(format_quantity(12'000.0, "Hz"), "12.0 kHz");
  // Time: ms below a second, s from 1000 ms.
  REQUIRE_EQ(format_quantity(0.25, "s"), "250 ms");
  REQUIRE_EQ(format_quantity(0.0125, "s"), "12.5 ms");
  REQUIRE_EQ(format_quantity(1.5, "s"), "1.50 s");
  REQUIRE_EQ(format_quantity(375.0, "ms"), "375 ms");
  REQUIRE_EQ(format_quantity(2'000.0, "ms"), "2.00 s");
  // Whole-number units, % spaced like every other unit.
  REQUIRE_EQ(format_quantity(75.0, "%"), "75 %");
  REQUIRE_EQ(format_quantity(-12.0, "ct", ValueSign::explicit_plus), minus + "12 ct");
  REQUIRE_EQ(format_quantity(7.0, "cents", ValueSign::explicit_plus), "+7 ct",
             "cents are spelled ct");
  REQUIRE_EQ(format_quantity(3.0, "\xC2\xA2"), "3 ct", "so is the cent sign");
  REQUIRE_EQ(format_quantity(90.0, "deg"), "90\xC2\xB0", "degrees take the degree sign");
  REQUIRE_EQ(format_quantity(-45.0, "\xC2\xB0"), minus + "45\xC2\xB0");
  REQUIRE_EQ(format_quantity(1.5, "x"), "1.50\xC3\x97", "a multiplier takes the times sign");
  // Other words follow their number.
  REQUIRE_EQ(format_quantity(-7.0, "st", ValueSign::explicit_plus, 0), minus + "7 st");
  REQUIRE_EQ(format_quantity(2.0, "oct"), "2.00 oct");
  REQUIRE_EQ(format_quantity(0.5, ""), "0.50", "no unit, no trailing space");
}

} // namespace

int main() {
  try {
    minus_sign_and_negative_zero();
    decimals_rule();
    units();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "value text contract passed\n";
  return 0;
}

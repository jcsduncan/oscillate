#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/test/check.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using oscillate::foundation::ErrorCode;
using oscillate::media::source_rate_frame_count;
using oscillate::media::source_rate_supported;

using oscillate::test::require;

int run_tests() {
  REQUIRE_OK(source_rate_supported(48'000, 48'000), "same-rate pair rejected");
  REQUIRE_TRUE(!source_rate_supported(44'117, 48'000), "unbounded phase period accepted");
  REQUIRE_EQ(source_rate_supported(44'117, 48'000).error().code, ErrorCode::unsupported,
             "unbounded phase period accepted");
  const auto standard_pair = source_rate_supported(44'100, 48'000);
#if OSCILLATE_HAVE_SOURCE_RESAMPLER
  REQUIRE_OK(standard_pair, "standard conversion pair rejected");
#else
  require(!standard_pair && standard_pair.error().code == ErrorCode::unsupported,
          "converter-OFF pair accepted");
#endif
  const auto exact = source_rate_frame_count(48'000, 48'000, 44'100);
  REQUIRE_TRUE(exact, "exact duration changed");
  REQUIRE_EQ(exact.value(), 44'100, "exact duration changed");
  const auto rounded_down = source_rate_frame_count(1, 96'000, 44'100);
  REQUIRE_TRUE(rounded_down, "sub-half frame did not round down");
  REQUIRE_EQ(rounded_down.value(), 0, "sub-half frame did not round down");
  const auto rounded_up = source_rate_frame_count(1, 48'000, 96'000);
  REQUIRE_TRUE(rounded_up, "double-rate frame changed");
  REQUIRE_EQ(rounded_up.value(), 2, "double-rate frame changed");
  const auto invalid = source_rate_frame_count(100, 0, 48'000);
  REQUIRE_TRUE(!invalid, "zero source rate was accepted");
  REQUIRE_EQ(invalid.error().code, ErrorCode::invalid_argument, "zero source rate was accepted");
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  const auto whole_overflow = source_rate_frame_count(max, 44'100, 96'000);
  REQUIRE_TRUE(!whole_overflow, "whole duration overflow was accepted");
  REQUIRE_EQ(whole_overflow.error().code, ErrorCode::resource_exhausted,
             "whole duration overflow was accepted");
  const auto whole = max / 96'000;
  const auto fractional_overflow = source_rate_frame_count(whole * 44'100 + 44'099, 44'100, 96'000);
  REQUIRE_TRUE(!fractional_overflow, "fractional duration overflow was accepted");
  REQUIRE_EQ(fractional_overflow.error().code, ErrorCode::resource_exhausted,
             "fractional duration overflow was accepted");
  return 0;
}
} // namespace

int main() {
  try {
    return run_tests();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#pragma once

// Assertions for Oscillate's tests. A failed check throws TestFailure (a
// std::runtime_error, so a test's own `catch (const std::exception&)` and the
// case runner in cases.hpp both report it) whose message names the file and
// line, the checked expression and, for comparisons, both values:
//
//   REQUIRE_TRUE(engine.ready());
//   REQUIRE_EQ(frames.size(), expected_frames, "the render covers the clip");
//   REQUIRE_NEAR(gain, unity, tolerance::float_round_trip);
//   REQUIRE_LT(std::abs(drift), limit);
//   REQUIRE_OK(store.save(state));
//   auto model = take(ProjectModel::create(...));
//
// The trailing message is optional on every macro. require(bool, message)
// remains for checks that are not one comparison.

#include "oscillate/foundation/error.hpp"

#include <atomic>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <optional>
#include <ostream>
#include <ranges>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace oscillate::test {

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

namespace detail {

// A failure names at most this many elements of a range.
inline constexpr std::size_t described_elements = 16;

template <typename T>
concept Streamable = requires(std::ostream& out, const T& value) { out << value; };

template <typename T> struct IsOptional : std::false_type {};
template <typename T> struct IsOptional<std::optional<T>> : std::true_type {};

template <typename T> void describe_into(std::ostream& out, const T& value);

template <typename T> void describe_optional(std::ostream& out, const std::optional<T>& value) {
  if (value) {
    describe_into(out, *value);
  } else {
    out << "nullopt";
  }
}

template <typename T> void describe_into(std::ostream& out, const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    out << (value ? "true" : "false");
  } else if constexpr (std::is_same_v<T, char> || std::is_same_v<T, signed char> ||
                       std::is_same_v<T, unsigned char>) {
    out << static_cast<int>(value);
  } else if constexpr (std::is_floating_point_v<T>) {
    out << std::setprecision(std::numeric_limits<T>::max_digits10) << value;
  } else if constexpr (std::is_enum_v<T>) {
    out << static_cast<long long>(static_cast<std::underlying_type_t<T>>(value));
  } else if constexpr (Streamable<T>) {
    out << value;
  } else if constexpr (IsOptional<T>::value) {
    describe_optional(out, value);
  } else if constexpr (std::ranges::input_range<T>) {
    out << '{';
    std::size_t count = 0;
    for (const auto& element : value) {
      if (count == described_elements) {
        out << ", ...";
        break;
      }
      if (count != 0) {
        out << ", ";
      }
      describe_into(out, element);
      ++count;
    }
    out << '}';
  } else {
    out << "<unprintable>";
  }
}

template <typename T> struct IsAtomic : std::false_type {};
template <typename T> struct IsAtomic<std::atomic<T>> : std::true_type {};

// An atomic operand is read once, as the built-in comparison's implicit
// conversion would; anything else is passed through untouched.
template <typename T> [[nodiscard]] constexpr decltype(auto) observed(const T& value) {
  if constexpr (IsAtomic<T>::value) {
    return value.load();
  } else {
    return (value);
  }
}

template <typename T> [[nodiscard]] std::string describe(const T& value) {
  std::ostringstream out;
  describe_into(out, value);
  return out.str();
}

// `what` is the failed check, `message` the caller's note.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[noreturn]] inline void fail(std::string_view what, std::string_view message,
                              const std::source_location& where) {
  std::string text = std::string(where.file_name()) + ':' + std::to_string(where.line()) + ": " +
                     std::string(what);
  if (!message.empty()) {
    text += ": ";
    text += message;
  }
  throw TestFailure(text);
}

// Integers compare by value whatever their signedness, so
// REQUIRE_EQ(items.size(), 3) is neither a sign-compare warning nor a
// wrapped comparison.
template <typename T>
concept SafeInteger = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
                      !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> &&
                      !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

// Mixed arithmetic operands convert to their common type explicitly, which
// is what the built-in operator does implicitly, without the conversion
// warning a template would raise where the original literal did not.
template <typename A, typename B>
concept MixedArithmetic = std::is_arithmetic_v<A> && std::is_arithmetic_v<B> &&
                          !std::is_same_v<A, B> && !(SafeInteger<A> && SafeInteger<B>);

template <typename A, typename B> [[nodiscard]] constexpr auto common(const A& value) {
  return static_cast<std::common_type_t<A, B>>(value);
}

template <typename A, typename B> [[nodiscard]] constexpr bool equal(const A& a, const B& b) {
  if constexpr (SafeInteger<A> && SafeInteger<B>) {
    return std::cmp_equal(a, b);
  } else if constexpr (MixedArithmetic<A, B>) {
    return common<A, B>(a) == common<B, A>(b);
  } else {
    return a == b;
  }
}

template <typename A, typename B> [[nodiscard]] constexpr bool less(const A& a, const B& b) {
  if constexpr (SafeInteger<A> && SafeInteger<B>) {
    return std::cmp_less(a, b);
  } else if constexpr (MixedArithmetic<A, B>) {
    return common<A, B>(a) < common<B, A>(b);
  } else {
    return a < b;
  }
}

template <typename A, typename B> [[nodiscard]] constexpr bool greater(const A& a, const B& b) {
  if constexpr (SafeInteger<A> && SafeInteger<B>) {
    return std::cmp_greater(a, b);
  } else if constexpr (MixedArithmetic<A, B>) {
    return common<A, B>(a) > common<B, A>(b);
  } else {
    return a > b;
  }
}

template <typename A, typename B> [[nodiscard]] constexpr bool less_equal(const A& a, const B& b) {
  if constexpr (SafeInteger<A> && SafeInteger<B>) {
    return std::cmp_less_equal(a, b);
  } else if constexpr (MixedArithmetic<A, B>) {
    return common<A, B>(a) <= common<B, A>(b);
  } else {
    return a <= b;
  }
}

template <typename A, typename B>
[[nodiscard]] constexpr bool greater_equal(const A& a, const B& b) {
  if constexpr (SafeInteger<A> && SafeInteger<B>) {
    return std::cmp_greater_equal(a, b);
  } else if constexpr (MixedArithmetic<A, B>) {
    return common<A, B>(a) >= common<B, A>(b);
  } else {
    return a >= b;
  }
}

template <typename A, typename B>
[[noreturn]] void fail_compare(std::string_view macro, std::string_view actual_text,
                               std::string_view expected_text, const A& actual, const B& expected,
                               std::string_view message, const std::source_location& where) {
  std::string what = std::string(macro) + '(' + std::string(actual_text) + ", " +
                     std::string(expected_text) + ") failed: " + describe(actual) + " vs " +
                     describe(expected);
  fail(what, message, where);
}

// Each comparison evaluates both operands once, exactly as the expression
// `actual OP expected` would, and the macros pass their text for the report.
template <typename A, typename B>
void require_eq(const A& actual, const B& expected, std::string_view actual_text,
                std::string_view expected_text, const std::source_location& where,
                std::string_view message = {}) {
  if constexpr (IsAtomic<A>::value || IsAtomic<B>::value) {
    require_eq(observed(actual), observed(expected), actual_text, expected_text, where, message);
  } else {
    if (!equal(actual, expected)) {
      fail_compare("REQUIRE_EQ", actual_text, expected_text, actual, expected, message, where);
    }
  }
}

template <typename A, typename B>
void require_ne(const A& actual, const B& expected, std::string_view actual_text,
                std::string_view expected_text, const std::source_location& where,
                std::string_view message = {}) {
  if constexpr (IsAtomic<A>::value || IsAtomic<B>::value) {
    require_ne(observed(actual), observed(expected), actual_text, expected_text, where, message);
  } else {
    if (equal(actual, expected)) {
      fail_compare("REQUIRE_NE", actual_text, expected_text, actual, expected, message, where);
    }
  }
}

template <typename A, typename B>
void require_lt(const A& actual, const B& expected, std::string_view actual_text,
                std::string_view expected_text, const std::source_location& where,
                std::string_view message = {}) {
  if constexpr (IsAtomic<A>::value || IsAtomic<B>::value) {
    require_lt(observed(actual), observed(expected), actual_text, expected_text, where, message);
  } else {
    if (!less(actual, expected)) {
      fail_compare("REQUIRE_LT", actual_text, expected_text, actual, expected, message, where);
    }
  }
}

template <typename A, typename B>
void require_le(const A& actual, const B& expected, std::string_view actual_text,
                std::string_view expected_text, const std::source_location& where,
                std::string_view message = {}) {
  if constexpr (IsAtomic<A>::value || IsAtomic<B>::value) {
    require_le(observed(actual), observed(expected), actual_text, expected_text, where, message);
  } else {
    if (!less_equal(actual, expected)) {
      fail_compare("REQUIRE_LE", actual_text, expected_text, actual, expected, message, where);
    }
  }
}

template <typename A, typename B>
void require_gt(const A& actual, const B& expected, std::string_view actual_text,
                std::string_view expected_text, const std::source_location& where,
                std::string_view message = {}) {
  if constexpr (IsAtomic<A>::value || IsAtomic<B>::value) {
    require_gt(observed(actual), observed(expected), actual_text, expected_text, where, message);
  } else {
    if (!greater(actual, expected)) {
      fail_compare("REQUIRE_GT", actual_text, expected_text, actual, expected, message, where);
    }
  }
}

template <typename A, typename B>
void require_ge(const A& actual, const B& expected, std::string_view actual_text,
                std::string_view expected_text, const std::source_location& where,
                std::string_view message = {}) {
  if constexpr (IsAtomic<A>::value || IsAtomic<B>::value) {
    require_ge(observed(actual), observed(expected), actual_text, expected_text, where, message);
  } else {
    if (!greater_equal(actual, expected)) {
      fail_compare("REQUIRE_GE", actual_text, expected_text, actual, expected, message, where);
    }
  }
}

// |actual - expected| <= tolerance, computed in the operands' own types as
// the expression `std::abs(actual - expected) <= tolerance` would be. A NaN
// on either side fails.
template <typename A, typename B, typename T>
void require_near(const A& actual, const B& expected, const T& tolerance,
                  std::string_view actual_text, std::string_view expected_text,
                  const std::source_location& where, std::string_view message = {}) {
  const auto difference = [&] {
    if constexpr (MixedArithmetic<A, B>) {
      return std::abs(common<A, B>(actual) - common<B, A>(expected));
    } else {
      return std::abs(actual - expected);
    }
  }();
  if (!less_equal(difference, tolerance)) {
    std::string what = "REQUIRE_NEAR(" + std::string(actual_text) + ", " +
                       std::string(expected_text) + ") failed: " + describe(actual) + " vs " +
                       describe(expected) + ", difference " + describe(difference) +
                       " > tolerance " + describe(tolerance);
    fail(what, message, where);
  }
}

[[nodiscard]] inline std::string describe_error(const foundation::Error& error);

// A failed Result (or anything else whose error() carries a message) also
// reports its error.
template <typename C> [[nodiscard]] std::string error_suffix(const C& condition) {
  if constexpr (requires { condition.error().message; }) {
    return " with " + describe_error(condition.error());
  } else {
    return {};
  }
}

template <typename C>
void require_true(const C& condition, std::string_view text, const std::source_location& where,
                  std::string_view message = {}) {
  if (!static_cast<bool>(condition)) {
    fail("REQUIRE_TRUE(" + std::string(text) + ") failed" + error_suffix(condition), message,
         where);
  }
}

template <typename C>
void require_false(const C& condition, std::string_view text, const std::source_location& where,
                   std::string_view message = {}) {
  if (static_cast<bool>(condition)) {
    fail("REQUIRE_FALSE(" + std::string(text) + ") failed", message, where);
  }
}

[[nodiscard]] inline std::string describe_error(const foundation::Error& error) {
  return "error " + std::to_string(static_cast<int>(error.code)) + ": " + error.message;
}

// Anything with has_value() (a Result, an optional); a Result's failure
// also reports its error code and message.
template <typename R>
void require_ok(const R& result, std::string_view text, const std::source_location& where,
                std::string_view message = {}) {
  if (!result.has_value()) {
    if constexpr (requires { result.error().message; }) {
      fail("REQUIRE_OK(" + std::string(text) + ") failed with " + describe_error(result.error()),
           message, where);
    } else {
      fail("REQUIRE_OK(" + std::string(text) + ") failed: no value", message, where);
    }
  }
}

} // namespace detail

// The plain check: `message` says what should have held.
inline void require(bool condition, std::string_view message = {},
                    std::source_location where = std::source_location::current()) {
  if (!condition) {
    detail::fail("check failed", message, where);
  }
}

// The value of a successful Result; a failure reports its error message.
template <typename T>
[[nodiscard]] T take(foundation::Result<T> result,
                     std::source_location where = std::source_location::current()) {
  if (!result) {
    detail::fail("take() of a failed Result: " + detail::describe_error(result.error()), {}, where);
  }
  return std::move(result).value();
}

inline void take(foundation::Result<void> result,
                 std::source_location where = std::source_location::current()) {
  if (!result) {
    detail::fail("take() of a failed Result: " + detail::describe_error(result.error()), {}, where);
  }
}

} // namespace oscillate::test

#define OSCILLATE_TEST_HERE() ::std::source_location::current()

#define REQUIRE_TRUE(condition, ...)                                                               \
  ::oscillate::test::detail::require_true((condition), #condition,                                 \
                                          OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_FALSE(condition, ...)                                                              \
  ::oscillate::test::detail::require_false((condition), #condition,                                \
                                           OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_EQ(actual, expected, ...)                                                          \
  ::oscillate::test::detail::require_eq((actual), (expected), #actual, #expected,                  \
                                        OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_NE(actual, expected, ...)                                                          \
  ::oscillate::test::detail::require_ne((actual), (expected), #actual, #expected,                  \
                                        OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_LT(actual, expected, ...)                                                          \
  ::oscillate::test::detail::require_lt((actual), (expected), #actual, #expected,                  \
                                        OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_LE(actual, expected, ...)                                                          \
  ::oscillate::test::detail::require_le((actual), (expected), #actual, #expected,                  \
                                        OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_GT(actual, expected, ...)                                                          \
  ::oscillate::test::detail::require_gt((actual), (expected), #actual, #expected,                  \
                                        OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_GE(actual, expected, ...)                                                          \
  ::oscillate::test::detail::require_ge((actual), (expected), #actual, #expected,                  \
                                        OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_NEAR(actual, expected, tolerance, ...)                                             \
  ::oscillate::test::detail::require_near((actual), (expected), (tolerance), #actual, #expected,   \
                                          OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)
#define REQUIRE_OK(result, ...)                                                                    \
  ::oscillate::test::detail::require_ok((result), #result,                                         \
                                        OSCILLATE_TEST_HERE() __VA_OPT__(, ) __VA_ARGS__)

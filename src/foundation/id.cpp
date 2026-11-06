#include "oscillate/foundation/id.hpp"

#include <array>
#include <cstddef>

namespace oscillate::foundation {
namespace {

constexpr std::size_t kTextLength = 36;

[[nodiscard]] constexpr bool is_separator(std::size_t index) noexcept {
  return index == 8 || index == 13 || index == 18 || index == 23;
}

[[nodiscard]] constexpr int hex_value(char digit) noexcept {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  return -1;
}

} // namespace

Result<IdBits> parse_id_bits(std::string_view text) {
  if (text.size() != kTextLength) {
    return Result<IdBits>::failure({ErrorCode::invalid_argument, "ID must contain 36 characters"});
  }

  IdBits result;
  std::size_t digit_count = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (is_separator(index)) {
      if (text[index] != '-') {
        return Result<IdBits>::failure({ErrorCode::invalid_argument, "invalid ID separator"});
      }
      continue;
    }

    const int digit = hex_value(text[index]);
    if (digit < 0) {
      return Result<IdBits>::failure({ErrorCode::invalid_argument, "invalid ID hex digit"});
    }
    if (digit_count < 16) {
      result.high = (result.high << 4U) | static_cast<std::uint64_t>(digit);
    } else {
      result.low = (result.low << 4U) | static_cast<std::uint64_t>(digit);
    }
    ++digit_count;
  }
  if (result.is_nil()) {
    return Result<IdBits>::failure({ErrorCode::invalid_argument, "zero ID is reserved"});
  }
  return Result<IdBits>::success(result);
}

std::string format_id_bits(IdBits bits) {
  constexpr std::array<char, 16> hex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result(kTextLength, '-');
  std::size_t digit_count = 0;
  for (std::size_t index = 0; index < result.size(); ++index) {
    if (is_separator(index)) {
      continue;
    }
    const std::uint64_t word = digit_count < 16 ? bits.high : bits.low;
    const std::size_t word_digit = digit_count < 16 ? digit_count : digit_count - 16;
    const unsigned shift = static_cast<unsigned>((15 - word_digit) * 4);
    result[index] = hex[(word >> shift) & 0xfU];
    ++digit_count;
  }
  return result;
}

} // namespace oscillate::foundation

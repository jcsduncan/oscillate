#include "oscillate/foundation/hex.hpp"

#include <algorithm>

namespace oscillate::foundation {
namespace {

constexpr std::string_view digits = "0123456789abcdef";
constexpr unsigned nibble_bits = 4;
constexpr unsigned nibble_mask = 0x0fU;

} // namespace

std::string to_hex(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size() * hex_digits_per_byte);
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    text.push_back(digits[value >> nibble_bits]);
    text.push_back(digits[value & nibble_mask]);
  }
  return text;
}

bool is_lowercase_hex(std::string_view text, std::size_t length) noexcept {
  return text.size() == length && std::ranges::all_of(text, [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

} // namespace oscillate::foundation

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace oscillate::foundation {

inline constexpr std::size_t hex_digits_per_byte = 2;

// Lowercase hexadecimal text of `bytes`, most significant nibble first.
[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);
[[nodiscard]] inline std::string to_hex(std::span<const std::uint8_t> bytes) {
  return to_hex(std::as_bytes(bytes));
}

// True when `text` is exactly `length` lowercase hexadecimal digits.
[[nodiscard]] bool is_lowercase_hex(std::string_view text, std::size_t length) noexcept;

} // namespace oscillate::foundation

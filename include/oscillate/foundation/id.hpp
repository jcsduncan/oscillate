#pragma once

#include "oscillate/foundation/error.hpp"

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace oscillate::foundation {

struct IdBits {
  std::uint64_t high = 0;
  std::uint64_t low = 0;

  [[nodiscard]] constexpr bool is_nil() const noexcept { return high == 0 && low == 0; }
  friend constexpr bool operator==(IdBits, IdBits) = default;
  friend constexpr auto operator<=>(IdBits, IdBits) = default;
};

// Canonical UUID-shaped form: 8-4-4-4-12 lowercase hexadecimal digits.
// Input accepts uppercase hex but requires exact hyphen positions and no whitespace.
[[nodiscard]] Result<IdBits> parse_id_bits(std::string_view text);
[[nodiscard]] std::string format_id_bits(IdBits bits);

// Tag types distinguish unrelated persisted entities at compile time. The all-zero ID is reserved.
// The owner of each entity chooses unique bits; this module intentionally does not source entropy.
template <typename Tag> class Id {
public:
  [[nodiscard]] static Result<Id> from_bits(IdBits bits) {
    if (bits.is_nil()) {
      return Result<Id>::failure({ErrorCode::invalid_argument, "zero ID is reserved"});
    }
    return Result<Id>::success(Id(bits));
  }

  [[nodiscard]] static Result<Id> parse(std::string_view text) {
    auto parsed = parse_id_bits(text);
    if (!parsed) {
      return Result<Id>::failure(parsed.error());
    }
    return from_bits(parsed.value());
  }

  [[nodiscard]] constexpr IdBits bits() const noexcept { return bits_; }
  [[nodiscard]] std::string to_string() const { return format_id_bits(bits_); }

  friend constexpr bool operator==(Id, Id) = default;
  friend constexpr auto operator<=>(Id, Id) = default;

private:
  explicit constexpr Id(IdBits bits) noexcept : bits_(bits) {}

  IdBits bits_;
};

} // namespace oscillate::foundation

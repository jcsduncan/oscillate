#pragma once

#include <cstdint>

// SplitMix64 (Steele, Lea and Flood): a small, well-mixed, deterministic
// generator for seeds, dither and pattern variation. Not for cryptography.
namespace oscillate::foundation {

inline constexpr std::uint64_t split_mix_increment = 0x9e3779b97f4a7c15ULL;

// The SplitMix64 output function: a bijective mix of one 64-bit value.
[[nodiscard]] constexpr std::uint64_t split_mix(std::uint64_t value) noexcept {
  constexpr std::uint64_t first_multiplier = 0xbf58476d1ce4e5b9ULL;
  constexpr std::uint64_t second_multiplier = 0x94d049bb133111ebULL;
  constexpr unsigned first_shift = 30;
  constexpr unsigned second_shift = 27;
  constexpr unsigned final_shift = 31;
  value = (value ^ (value >> first_shift)) * first_multiplier;
  value = (value ^ (value >> second_shift)) * second_multiplier;
  return value ^ (value >> final_shift);
}

class SplitMix64 {
public:
  explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    state_ += split_mix_increment;
    return split_mix(state_);
  }

  // Uniform in [0, 1) from the top 53 bits.
  [[nodiscard]] constexpr double unit() noexcept {
    constexpr unsigned mantissa_bits = 53;
    constexpr unsigned discarded_bits = 64 - mantissa_bits;
    constexpr double scale = 1.0 / static_cast<double>(std::uint64_t{1} << mantissa_bits);
    return static_cast<double>(next() >> discarded_bits) * scale;
  }

  [[nodiscard]] constexpr std::uint64_t state() const noexcept { return state_; }

private:
  std::uint64_t state_;
};

} // namespace oscillate::foundation

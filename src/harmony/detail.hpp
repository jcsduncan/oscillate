#pragma once

#include "oscillate/harmony/harmony.hpp"

#include <array>
#include <cstdint>

// Shared by the harmony library's translation units.
namespace oscillate::harmony::detail {

inline constexpr int octave = 12;
inline constexpr std::size_t scale_size = 7;
using Scale = std::array<std::uint8_t, scale_size>;
inline constexpr Scale major_scale{0, 2, 4, 5, 7, 9, 11};
inline constexpr Scale natural_minor_scale{0, 2, 3, 5, 7, 8, 10};

// The quality's interval from the tonic that names the dominant (V).
inline constexpr std::uint8_t dominant_degree = 7;

[[nodiscard]] constexpr std::uint8_t pc(int value) noexcept {
  return static_cast<std::uint8_t>(((value % octave) + octave) % octave);
}
// Semitones from the key's tonic up to a pitch class.
[[nodiscard]] constexpr std::uint8_t from_tonic(std::uint8_t pitch_class,
                                                const MusicalKey& key) noexcept {
  return pc(static_cast<int>(pitch_class) - static_cast<int>(key.tonic));
}
// The shorter way round the circle between two pitch classes.
[[nodiscard]] constexpr int step_between(std::uint8_t a, std::uint8_t b) noexcept {
  const int d = pc(static_cast<int>(a) - static_cast<int>(b));
  return d < octave - d ? d : octave - d;
}
[[nodiscard]] constexpr const Scale& key_scale(const MusicalKey& key) noexcept {
  return key.mode == project::KeyMode::minor ? natural_minor_scale : major_scale;
}
// Bit n set for each pitch class of the key's scale.
[[nodiscard]] std::uint16_t key_set(const MusicalKey& key) noexcept;
[[nodiscard]] int count_bits(std::uint16_t bits) noexcept;
[[nodiscard]] constexpr std::uint16_t bit(std::uint8_t pitch_class) noexcept {
  return static_cast<std::uint16_t>(1U << pitch_class);
}

// SplitMix64: the same numbers on every standard library.
class SplitMix64 {
public:
  explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  // Uniform in [0, 1).
  [[nodiscard]] constexpr double unit() noexcept {
    constexpr int mantissa_bits = 53;
    constexpr double scale = 1.0 / static_cast<double>(1ULL << mantissa_bits);
    return static_cast<double>(next() >> (64U - mantissa_bits)) * scale;
  }

private:
  std::uint64_t state_;
};

// Quality maps shared by the Lab, substitutions and hints. Each returns
// false when the quality has no such neighbour.
[[nodiscard]] bool richer(ChordQuality in, ChordQuality& out) noexcept;
[[nodiscard]] bool simpler(ChordQuality in, ChordQuality& out) noexcept;
// The same root from the opposite mode (minor <-> major family).
[[nodiscard]] bool flipped(ChordQuality in, ChordQuality& out) noexcept;
[[nodiscard]] ChordQuality suspended(ChordQuality in) noexcept;

// The region's end tick.
[[nodiscard]] constexpr std::int64_t end_of(const ChordRegion& region) noexcept {
  return region.position.value + region.length_ticks;
}

} // namespace oscillate::harmony::detail

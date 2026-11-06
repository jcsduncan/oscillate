#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/fret.hpp"
#include "oscillate/native_dsp/synth/fret_feel.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

// Fret's sample library (ADR-0088), on the shared streaming core. Private
// to the engine.
namespace oscillate::native_dsp::synth::fret::detail {

// The library's sample kinds, in manifest order: Feel's four articulations
// and the string release.
enum class Kind : std::uint8_t { pluck, staccato, ghost, bridge, release };
inline constexpr std::size_t kind_count = 5;

[[nodiscard]] constexpr Kind kind_of(feel::Articulation articulation) noexcept {
  switch (articulation) {
  case feel::Articulation::pluck:
    return Kind::pluck;
  case feel::Articulation::staccato:
    return Kind::staccato;
  case feel::Articulation::ghost:
    return Kind::ghost;
  case feel::Articulation::bridge:
    break;
  }
  return Kind::bridge;
}

struct Sample {
  Kind kind = Kind::pluck;
  int key = 0;
  std::uint32_t layer = 0;
  std::uint32_t layers = 1;
  std::uint32_t round_robin = 0;
  double peak_db = 0.0; // The recording's peak before normalisation.
};

// Bounds on what one key and kind may hold.
inline constexpr std::uint32_t max_layers = 8;
inline constexpr std::uint32_t max_round_robins = 8;

class Library final : public sample_stream::Library {
public:
  [[nodiscard]] static foundation::Result<std::unique_ptr<Library>>
  open(const std::filesystem::path& directory);

  [[nodiscard]] const Sample& sample(std::uint32_t index) const noexcept { return samples_[index]; }
  [[nodiscard]] const sample_stream::Sample& stream(std::uint32_t index) const noexcept {
    return pack_.sample(index);
  }
  [[nodiscard]] const std::int16_t* resident(std::uint32_t index) const noexcept {
    return pack_.resident(pack_.sample(index));
  }
  // A key's samples of one kind for one layer, by round robin; empty when
  // the library has none.
  [[nodiscard]] std::span<const std::uint32_t> round_robins(Kind kind, int key,
                                                            std::uint32_t layer) const noexcept;
  [[nodiscard]] std::uint32_t layers(Kind kind, int key) const noexcept;
  // The longest release sample, in pack frames (the tail bound).
  [[nodiscard]] std::uint32_t longest_release() const noexcept { return longest_release_; }
  [[nodiscard]] const library::Status& status() const noexcept { return status_; }

private:
  Library() = default;

  struct Cell {
    std::array<std::vector<std::uint32_t>, max_layers> layers;
    std::uint32_t count = 0;
  };

  std::vector<Sample> samples_;
  std::array<std::array<Cell, key_count>, kind_count> cells_{};
  std::uint32_t longest_release_ = 0;
  library::Status status_;
};

[[nodiscard]] sample_stream::Registry& registry();
// The library new and idle kernels use; null while missing. Callback safe.
[[nodiscard]] const Library* current() noexcept;

} // namespace oscillate::native_dsp::synth::fret::detail

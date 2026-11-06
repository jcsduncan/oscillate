#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"
#include "oscillate/native_dsp/synth/wren.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

// Wren's sample library (ADR-0089), on the shared sample core with every
// sample resident: nothing streams, so no stream slots and no disk access
// after loading. Private to the engine.
namespace oscillate::native_dsp::synth::wren::detail {

// The two takes of every note, in manifest order.
enum class Kind : std::uint8_t { vibrato, plain };
inline constexpr std::size_t kind_count = 2;

struct Sample {
  Kind kind = Kind::plain;
  int key = 0;             // The key it was played as.
  std::uint32_t layer = 0; // 0 pp, 1 mf, 2 ff.
  // Pack frames from the sample's start: where the attack has settled (a
  // slur starts here) and the loop, whose crossfade the installer baked in.
  std::uint32_t attack_end = 0;
  std::uint32_t loop_start = 0;
  std::uint32_t loop_end = 0;
  double cents = 0.0; // Its measured tuning against `key`.
  // The recording's peak before normalisation. Informational: the pack's
  // scale already turns each int16 back into the recording's level, so
  // layers keep their recorded dynamics.
  double peak_db = 0.0;
};

// The shortest loop accepted: the resampler reads this far around a frame.
inline constexpr std::uint32_t min_loop_frames = 2 * sample_stream::SincTable::taps;

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
  // The sample a (kind, key, layer) plays: its own when the library has it,
  // else the nearest key of the same kind and layer, else the nearest
  // layer, else the other kind. Keys must be in first_key..last_key.
  [[nodiscard]] std::uint32_t pick(Kind kind, int key, std::uint32_t layer) const noexcept {
    return picks_[static_cast<std::size_t>(kind)][static_cast<std::size_t>(key - first_key)][layer];
  }
  // True when the library has this cell itself (not a fallback).
  [[nodiscard]] bool has(Kind kind, int key, std::uint32_t layer) const noexcept;
  [[nodiscard]] const library::Status& status() const noexcept { return status_; }

private:
  Library() = default;

  // The sample index of each cell, no_sample where the library has none.
  using Cells =
      std::array<std::array<std::array<std::uint32_t, layer_count>, key_count>, kind_count>;
  void fill_picks() noexcept;

  std::vector<Sample> samples_;
  Cells cells_{};
  Cells picks_{};
  library::Status status_;
};

[[nodiscard]] sample_stream::Registry& registry();
// The library new and idle kernels use; null while missing. Callback safe.
[[nodiscard]] const Library* current() noexcept;

} // namespace oscillate::native_dsp::synth::wren::detail

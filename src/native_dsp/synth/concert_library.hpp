#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/concert.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

// Concert's sample library (ADR-0071). Private to the engine.
//
// The pack holds interleaved int16 stereo at library::pack_rate and is read
// through the shared streaming core (sample_stream, ADR-0088). A Library
// keeps each note sample's first library::resident_frames in memory and the
// release, resonance and pedal samples whole; the rest of a note streams
// into its voice's ring.
namespace oscillate::native_dsp::synth::concert::detail {

inline constexpr std::uint32_t pack_channels = 2;

enum class SampleKind : std::uint8_t { note, release, resonance, pedal_down, pedal_up };

// A sample's piano voicing; its place in the pack is Library::stream().
struct Sample {
  SampleKind kind = SampleKind::note;
  int first_key = 0;
  int last_key = 0;
  int root_key = 0;
  int low_velocity = 1;
  int high_velocity = 127;
  double peak_db = 0.0; // The recording's peak before normalisation.
  double gain_db = 0.0; // The library's level for the kind.
  double velocity_track = 0.0;
  double decay_db_per_second = 0.0; // Of the time the note was held.
};

class Library final : public sample_stream::Library {
public:
  ~Library() override = default;

  // Parses and checks the manifest against the pack (untrusted input),
  // then reads every sample's resident frames.
  [[nodiscard]] static foundation::Result<std::unique_ptr<Library>>
  open(const std::filesystem::path& directory);

  [[nodiscard]] std::span<const Sample> samples() const noexcept { return samples_; }
  [[nodiscard]] const Sample& sample(std::uint32_t index) const noexcept { return samples_[index]; }
  // The sample's place in the pack: frames, resident start and scale.
  [[nodiscard]] const sample_stream::Sample& stream(std::uint32_t index) const noexcept {
    return pack().sample(index);
  }
  [[nodiscard]] const std::int16_t* resident(std::uint32_t index) const noexcept {
    return pack().resident(pack().sample(index));
  }
  // A key's note layers, softest first.
  [[nodiscard]] std::span<const std::uint32_t> notes(int key) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> releases(int key) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> resonances(int key) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> pedal(bool down) const noexcept {
    return down ? pedal_down_ : pedal_up_;
  }
  [[nodiscard]] int last_damped_key() const noexcept { return last_damped_key_; }
  [[nodiscard]] const library::Status& status() const noexcept { return status_; }

  // Bounds for tail_frames, in pack frames: the longest note sample on an
  // undamped key and the longest release, resonance or pedal-up sample.
  [[nodiscard]] std::uint32_t longest_undamped_note() const noexcept {
    return longest_undamped_note_;
  }
  [[nodiscard]] std::uint32_t longest_one_shot() const noexcept { return longest_one_shot_; }

private:
  Library() = default;

  struct KeyIndex {
    std::vector<std::uint32_t> notes;
    std::vector<std::uint32_t> releases;
    std::vector<std::uint32_t> resonances;
  };

  std::vector<Sample> samples_;
  std::array<KeyIndex, key_count> keys_{};
  std::vector<std::uint32_t> pedal_down_;
  std::vector<std::uint32_t> pedal_up_;
  int last_damped_key_ = 0;
  std::uint32_t longest_undamped_note_ = 0;
  std::uint32_t longest_one_shot_ = 0;
  library::Status status_;
};

// Concert's libraries: one registry per process, shared by every Concert.
[[nodiscard]] sample_stream::Registry& registry();
// The library new and idle kernels use; null while missing. Callback safe.
[[nodiscard]] const Library* current() noexcept;
inline constexpr std::uint32_t max_libraries = 255;

} // namespace oscillate::native_dsp::synth::concert::detail

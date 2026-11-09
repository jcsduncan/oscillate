#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/bow.hpp"
#include "oscillate/native_dsp/synth/sample_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

// Bow's sample library (ADR-0111), on the shared sample core with every
// sample resident: a stereo pack (the VSCO sections and solo shorts) and a
// mono pack (the Iowa solo takes). Nothing streams. Private to the engine.
namespace oscillate::native_dsp::synth::bow::detail {

// What was recorded: Legato plays the sustains.
enum class Recorded : std::uint8_t { sustain, staccato, pizzicato, tremolo };
inline constexpr std::size_t recorded_count = 4;
inline constexpr std::size_t mode_count = 2;
inline constexpr std::size_t set_count = mode_count * instrument_count * recorded_count;

[[nodiscard]] constexpr bool looped(Recorded recorded) noexcept {
  return recorded == Recorded::sustain || recorded == Recorded::tremolo;
}

struct Sample {
  std::uint32_t channels = 1;
  Mode mode = Mode::section;
  Instrument instrument = Instrument::violin;
  Recorded recorded = Recorded::sustain;
  int key = 0;             // The key it was played as.
  std::uint32_t layer = 0; // 0 the softest.
  std::uint32_t round_robin = 0;
  // Frames from the sample's start: where the attack has settled (a legato
  // note starts here) and the loop, whose crossfade the installer baked in.
  // loop_end 0: a one-shot.
  std::uint32_t attack_end = 0;
  std::uint32_t loop_start = 0;
  std::uint32_t loop_end = 0;
  double cents = 0.0;    // Its measured tuning against `key`.
  double level_db = 0.0; // Its steady level as the pack scale plays it.
};

// The round robins of one note and layer.
struct Pick {
  std::array<std::uint32_t, max_round_robins> samples{};
  std::uint32_t count = 0;
};

// The longest sample accepted (a one-shot plays out this long at most).
inline constexpr std::uint32_t max_sample_seconds = 10;

// The shortest loop accepted: the resampler reads this far around a frame.
inline constexpr std::uint32_t min_loop_frames = 2 * sample_stream::SincTable::taps;

class Library final : public sample_stream::Library {
public:
  [[nodiscard]] static foundation::Result<std::unique_ptr<Library>>
  open(const std::filesystem::path& directory);

  [[nodiscard]] const Sample& sample(std::uint32_t index) const noexcept { return samples_[index]; }
  [[nodiscard]] const sample_stream::Sample& stream(std::uint32_t index) const noexcept {
    return pack_of(index).sample(local_[index]);
  }
  [[nodiscard]] const std::int16_t* resident(std::uint32_t index) const noexcept {
    const auto& pack = pack_of(index);
    return pack.resident(pack.sample(local_[index]));
  }
  // The takes a note plays: the set's own when it has any samples, else the
  // other mode's; the nearest key with the layer (ties to the lower key),
  // else the nearest layer. Empty when neither mode recorded it. `key` is
  // clamped to first_key..last_key and `layer` to the set's layers.
  [[nodiscard]] const Pick& pick(Mode mode, Instrument instrument, Recorded recorded, int key,
                                 std::uint32_t layer) const noexcept;
  // The recorded layers the pick plays from (0 when nothing was recorded).
  [[nodiscard]] std::uint32_t layers(Mode mode, Instrument instrument,
                                     Recorded recorded) const noexcept;
  [[nodiscard]] const library::Status& status() const noexcept { return status_; }

private:
  Library() = default;
  [[nodiscard]] const sample_stream::Pack& pack_of(std::uint32_t index) const noexcept {
    return samples_[index].channels == 1 ? mono_pack_ : pack_;
  }
  void fill_picks();

  std::vector<Sample> samples_;
  // Each sample's index in its own pack.
  std::vector<std::uint32_t> local_;
  sample_stream::Pack mono_pack_;
  // Per set, key and layer.
  std::vector<Pick> picks_;
  std::array<std::uint32_t, set_count> layers_{};
  library::Status status_;
};

[[nodiscard]] sample_stream::Registry& registry();
// The library new and idle kernels use; null while missing. Callback safe.
[[nodiscard]] const Library* current() noexcept;
// The library's state as last looked at (loading while the background read
// runs), for kernels without a library to report. Callback safe.
[[nodiscard]] library::State published_state() noexcept;

} // namespace oscillate::native_dsp::synth::bow::detail

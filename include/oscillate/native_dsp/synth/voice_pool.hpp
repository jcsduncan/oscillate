#pragma once

#include "oscillate/native_dsp/synth/shaping.hpp"

#include <cstddef>
#include <cstdint>

// The voice handling the polyphonic engines share: round-robin allocation
// with oldest-first stealing, and the declick that fades a stopped voice's
// last output instead of cutting it.
namespace oscillate::native_dsp::synth {

// A stolen or reset voice's last output fades out over this many frames.
inline constexpr std::uint32_t declick_frames = 64;

// The fade of stopped voices' last outputs: each capture carries what is
// still fading and restarts the linear fade from the sum.
class Declick {
public:
  constexpr Declick() noexcept = default;
  // `frames` is the fade length at the rate the declick runs (an engine
  // that declicks oversampled passes its oversampled frames).
  constexpr explicit Declick(std::uint32_t frames) noexcept : frames_(frames) {}

  void capture(StereoSample last) noexcept {
    const double carried = static_cast<double>(remaining_) / static_cast<double>(frames_);
    held_ = held_ * carried + last;
    remaining_ = frames_;
  }
  void capture(double left, double right) noexcept { capture(StereoSample{left, right}); }

  // Adds one frame of the fade, while one is running.
  void add(StereoSample& sample) noexcept {
    if (remaining_ > 0) {
      sample = sample + held_ * step();
    }
  }

private:
  [[nodiscard]] double step() noexcept {
    const double fade = static_cast<double>(remaining_) / static_cast<double>(frames_);
    --remaining_;
    return fade;
  }

  StereoSample held_{};
  std::uint32_t frames_ = declick_frames;
  std::uint32_t remaining_ = 0;
};

// A voice for a new note from `voices` (which have `active`, `held`, `age`
// and `slot`): the first free one round-robin from `next_slot`, so
// consecutive notes land on different voices; otherwise the oldest released
// voice, or failing that the oldest, after `steal(voice)` has captured it.
// The voice returned is inactive; `next_slot` moves past it.
template <class Voices, class Steal>
[[nodiscard]] auto& allocate_voice(Voices& voices, std::uint32_t& next_slot,
                                   Steal&& steal) noexcept {
  const auto count = static_cast<std::uint32_t>(voices.size());
  for (std::uint32_t step = 0; step < count; ++step) {
    auto& voice = voices[(next_slot + step) % count];
    if (!voice.active) {
      next_slot = (voice.slot + 1) % count;
      return voice;
    }
  }
  auto* chosen = &voices[0];
  bool released = false;
  for (auto& voice : voices) {
    if (!voice.held && (!released || voice.age < chosen->age)) {
      chosen = &voice;
      released = true;
    }
  }
  if (!released) {
    for (auto& voice : voices) {
      if (voice.age < chosen->age) {
        chosen = &voice;
      }
    }
  }
  steal(*chosen);
  chosen->active = false;
  // NOLINTNEXTLINE(clang-analyzer-core.DivideZero): a pool always has voices.
  next_slot = (chosen->slot + 1) % count;
  return *chosen;
}

} // namespace oscillate::native_dsp::synth

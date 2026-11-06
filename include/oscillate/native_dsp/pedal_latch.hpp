#pragma once

#include "oscillate/foundation/id.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp {

// Generic pedals for instruments that do not model them (ADR-0035):
// - sustain (down at 0.5 or more) defers note-offs and flushes them on lift;
// - sostenuto captures the notes held when it goes down and defers only
//   their note-offs until it lifts;
// - soft scales new notes' velocity by 0.75.
// A note re-struck while its earlier note-off is deferred releases the
// earlier one first, because loop repeats reuse note IDs. Fixed capacity
// and no allocation, for the callback; when a list is full, a note-off is
// delivered rather than lost.
class PedalLatch {
public:
  static constexpr std::size_t capacity = 256;
  static constexpr double soft_velocity_scale = 0.75;

  struct Note {
    foundation::IdBits clip;
    foundation::IdBits note;
    std::uint8_t key = 0;
    bool operator==(const Note& other) const noexcept {
      return clip == other.clip && note == other.note;
    }
  };

  void reset() noexcept {
    held_count_ = 0;
    deferred_count_ = 0;
    captured_count_ = 0;
    sustain_ = false;
    sostenuto_ = false;
    soft_ = false;
  }

  [[nodiscard]] static bool down(double value) noexcept { return value >= 0.5; }

  // A pedal change; `release` receives each note-off it frees.
  template <typename Release>
  void pedal(project::MidiPedal pedal, double value, Release&& release) {
    const bool now = down(value);
    switch (pedal) {
    case project::MidiPedal::sustain:
      if (sustain_ && !now) {
        sustain_ = false;
        flush(release);
      }
      sustain_ = now;
      break;
    case project::MidiPedal::sostenuto:
      if (!sostenuto_ && now) {
        captured_count_ = 0;
        for (std::size_t i = 0; i < held_count_ && captured_count_ < capacity; ++i) {
          captured_[captured_count_++] = held_[i];
        }
      } else if (sostenuto_ && !now) {
        sostenuto_ = false;
        captured_count_ = 0;
        flush(release);
      }
      sostenuto_ = now;
      break;
    case project::MidiPedal::soft:
      soft_ = now;
      break;
    }
  }

  // A note-on; returns the velocity to play it at. `release` receives the
  // deferred note-off of an earlier strike of the same note, if any.
  template <typename Release>
  [[nodiscard]] std::uint8_t note_on(const Note& note, std::uint8_t velocity, Release&& release) {
    for (std::size_t i = 0; i < deferred_count_; ++i) {
      if (deferred_[i] == note) {
        release(deferred_[i]);
        remove(deferred_, deferred_count_, i);
        break;
      }
    }
    if (held_count_ < capacity) {
      held_[held_count_++] = note;
    }
    if (!soft_) {
      return velocity;
    }
    const auto scaled = static_cast<int>(std::lround(velocity * soft_velocity_scale));
    return static_cast<std::uint8_t>(scaled < 1 ? 1 : scaled);
  }

  // A note-off: true when it should be delivered now, false when a pedal
  // holds it (it is delivered by a later lift).
  [[nodiscard]] bool note_off(const Note& note) noexcept {
    for (std::size_t i = 0; i < held_count_; ++i) {
      if (held_[i] == note) {
        remove(held_, held_count_, i);
        break;
      }
    }
    const bool hold = sustain_ || (sostenuto_ && captured(note));
    if (!hold || deferred_count_ == capacity) {
      return true;
    }
    deferred_[deferred_count_++] = note;
    return false;
  }

  [[nodiscard]] bool sustained() const noexcept { return deferred_count_ > 0; }

private:
  template <std::size_t N>
  static void remove(std::array<Note, N>& list, std::size_t& count, std::size_t index) noexcept {
    list[index] = list[--count];
  }

  [[nodiscard]] bool captured(const Note& note) const noexcept {
    for (std::size_t i = 0; i < captured_count_; ++i) {
      if (captured_[i] == note) {
        return true;
      }
    }
    return false;
  }

  // Delivers every deferred note-off no pedal still holds, in order.
  template <typename Release> void flush(Release& release) {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < deferred_count_; ++i) {
      const auto note = deferred_[i];
      if (sustain_ || (sostenuto_ && captured(note))) {
        deferred_[kept++] = note;
      } else {
        release(note);
      }
    }
    deferred_count_ = kept;
  }

  std::array<Note, capacity> held_{};
  std::array<Note, capacity> deferred_{};
  std::array<Note, capacity> captured_{};
  std::size_t held_count_ = 0;
  std::size_t deferred_count_ = 0;
  std::size_t captured_count_ = 0;
  bool sustain_ = false;
  bool sostenuto_ = false;
  bool soft_ = false;
};

} // namespace oscillate::native_dsp

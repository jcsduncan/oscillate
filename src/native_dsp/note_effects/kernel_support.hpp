#pragma once

#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "tables.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>

// Shared callback-side helpers of the note effect kernels: fixed capacity,
// no allocation, no locks.
namespace oscillate::native_dsp::note_effects::support {

using engine::BlockMidiEvent;
using engine::MidiEventKind;
using foundation::IdBits;

// The order instruments take events in: by offset, offs before ons, then
// note id; clip and key settle the rest, so the order never depends on how
// a block's events arrived (and so on the block split).
[[nodiscard]] inline bool event_less(const BlockMidiEvent& a, const BlockMidiEvent& b) noexcept {
  return std::tie(a.offset, a.kind, a.note_id_bits, a.clip_id_bits, a.key) <
         std::tie(b.offset, b.kind, b.note_id_bits, b.clip_id_bits, b.key);
}

// SplitMix64: a deterministic hash for seeded choices.
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t value) noexcept {
  return native_dsp::dsp::splitmix64(value);
}
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t a, std::uint64_t b) noexcept {
  return mix(a ^ mix(b));
}
// A uniform value in [0, 1) from a hash.
[[nodiscard]] constexpr double unit(std::uint64_t hash) noexcept {
  return native_dsp::dsp::unit_from_bits(hash);
}

// Writes a block's events, keeping note_off_reserve slots for note-offs.
class Emitter {
public:
  explicit Emitter(std::span<BlockMidiEvent> output) noexcept : output_(output) {}
  // False (nothing written) when the block is too crowded for a new note.
  bool note_on(std::uint32_t offset, IdBits clip, IdBits note, int key, int velocity) noexcept {
    if (key < 0 || key > 127 || count_ + note_off_reserve >= output_.size())
      return false;
    output_[count_++] = {offset,
                         clip,
                         note,
                         MidiEventKind::note_on,
                         static_cast<std::uint8_t>(key),
                         static_cast<std::uint8_t>(std::clamp(velocity, 1, 127)),
                         false};
    return true;
  }
  void note_off(std::uint32_t offset, IdBits clip, IdBits note, int key,
                bool boundary_crop = false) noexcept {
    if (key < 0 || key > 127 || count_ >= output_.size())
      return;
    output_[count_++] = {
        offset, clip,         note, MidiEventKind::note_off, static_cast<std::uint8_t>(key),
        0,      boundary_crop};
  }
  // An input event unchanged.
  void pass(const BlockMidiEvent& event) noexcept {
    if (event.kind == MidiEventKind::note_on && count_ + note_off_reserve >= output_.size())
      return;
    if (count_ < output_.size())
      output_[count_++] = event;
  }
  // Sorts what was written and returns how many.
  [[nodiscard]] std::size_t finish() noexcept {
    std::sort(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(count_), event_less);
    return count_;
  }

private:
  std::span<BlockMidiEvent> output_;
  std::size_t count_ = 0;
};

// A fixed-capacity map from an input note (clip and note ids) to what the
// kernel made of it.
template <typename Value, std::size_t Capacity> class NoteMap {
public:
  struct Entry {
    IdBits clip;
    IdBits note;
    Value value;
  };
  [[nodiscard]] Value* find(IdBits clip, IdBits note) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].clip == clip && entries_[i].note == note)
        return &entries_[i].value;
    }
    return nullptr;
  }
  // Null when full. A note already present is replaced.
  Value* insert(IdBits clip, IdBits note, const Value& value) noexcept {
    if (auto* existing = find(clip, note)) {
      *existing = value;
      return existing;
    }
    if (count_ == Capacity)
      return nullptr;
    entries_[count_] = {clip, note, value};
    return &entries_[count_++].value;
  }
  void erase(IdBits clip, IdBits note) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].clip == clip && entries_[i].note == note) {
        entries_[i] = entries_[--count_];
        return;
      }
    }
  }
  void clear() noexcept { count_ = 0; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::span<Entry> entries() noexcept { return std::span(entries_).first(count_); }

private:
  std::array<Entry, Capacity> entries_{};
  std::size_t count_ = 0;
};

// Notes a kernel will play at later ticks. fire() emits every one that
// falls in the block, at its frame.
template <std::size_t Capacity> class Pending {
public:
  struct Event {
    std::int64_t tick = 0;
    MidiEventKind kind = MidiEventKind::note_off;
    IdBits clip;
    IdBits note;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
  };
  // False when full; a dropped note-on simply never sounds.
  bool push(const Event& event) noexcept {
    if (count_ == Capacity)
      return false;
    events_[count_++] = event;
    return true;
  }
  void fire(const NoteClock& clock, Emitter& out) noexcept {
    for (std::size_t i = 0; i < count_;) {
      const auto& event = events_[i];
      if (event.tick > clock.last_tick()) {
        ++i;
        continue;
      }
      const auto offset = clock.offset_of(event.tick);
      if (event.kind == MidiEventKind::note_on)
        (void)out.note_on(offset, event.clip, event.note, event.key, event.velocity);
      else
        out.note_off(offset, event.clip, event.note, event.key);
      events_[i] = events_[--count_];
    }
  }
  // Every pending note-off now, at offset 0 (the effect was switched off).
  void flush_offs(Emitter& out) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (events_[i].kind == MidiEventKind::note_off)
        out.note_off(0, events_[i].clip, events_[i].note, events_[i].key);
    }
    count_ = 0;
  }
  void clear() noexcept { count_ = 0; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] bool full() const noexcept { return count_ == Capacity; }

private:
  std::array<Event, Capacity> events_{};
  std::size_t count_ = 0;
};

// Note ids for generated notes: unique, never nil, never reused.
class NoteIds {
public:
  [[nodiscard]] IdBits next() noexcept { return {0, ++serial_}; }

private:
  std::uint64_t serial_ = 0;
};

// Pitch class helpers over a scale mask (tables::scale_masks), rooted at
// `root` (0 = C).
struct Scale {
  std::uint16_t mask = 0x0FFF;
  int root = 0;
  [[nodiscard]] static Scale from(int root, int scale) noexcept {
    const auto index = static_cast<std::size_t>(
        std::clamp(scale, 0, static_cast<int>(tables::scale_masks.size()) - 1));
    return {tables::scale_masks[index], ((root % 12) + 12) % 12};
  }
  [[nodiscard]] bool contains(int key) const noexcept {
    const int pitch = (((key - root) % 12) + 12) % 12;
    return ((mask >> pitch) & 1U) != 0;
  }
  // The scale tone nearest `key` (ties go up), the next at or above, or
  // the next at or below.
  [[nodiscard]] int snap(int key, int direction) const noexcept {
    if (contains(key))
      return key;
    for (int distance = 1; distance < 12; ++distance) {
      const bool up = contains(key + distance);
      const bool down = contains(key - distance);
      if (direction == tables::scale::up && up)
        return key + distance;
      if (direction == tables::scale::down && down)
        return key - distance;
      if (direction == tables::scale::nearest && (up || down))
        return up ? key + distance : key - distance;
    }
    return key;
  }
  // `steps` scale tones above (or below) a scale tone.
  [[nodiscard]] int move(int key, int steps) const noexcept {
    while (steps > 0) {
      ++key;
      if (contains(key))
        --steps;
      if (key > 200)
        break;
    }
    while (steps < 0) {
      --key;
      if (contains(key))
        ++steps;
      if (key < -80)
        break;
    }
    return key;
  }
};

} // namespace oscillate::native_dsp::note_effects::support

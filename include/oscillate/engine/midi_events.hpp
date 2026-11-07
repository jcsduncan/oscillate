#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::engine {

inline constexpr std::uint32_t max_midi_block_frames = 8'192;
inline constexpr std::size_t max_midi_events_per_block = 1'024;
// The most notes of one instrument's plan sounding at once. The compiler
// refuses a plan past it, so HeldNotes can track every note it sends: as many
// as one block can start.
inline constexpr std::size_t max_concurrent_midi_notes = max_midi_events_per_block;

enum class MidiEventKind : std::uint8_t { note_off = 0, note_on = 1 };

// Stable ID bits avoid default-constructing an invalid persistent Id in the
// caller-owned callback buffer. They always come from validated project IDs.
struct BlockMidiEvent {
  std::uint32_t offset = 0;
  foundation::IdBits clip_id_bits;
  foundation::IdBits note_id_bits;
  MidiEventKind kind = MidiEventKind::note_off;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
  bool boundary_crop = false;
  // A note-on's frames until its note-off, when the plan knows it (notes in
  // clips); 0 when nobody knows yet (keys played live, note effects).
  std::uint32_t frames = 0;
};

struct ScheduledMidiEvent {
  timeline::SamplePosition sample;
  project::ClipId clip;
  project::NoteId note;
  MidiEventKind kind;
  std::uint8_t key;
  std::uint8_t velocity;
  bool boundary_crop;
  std::uint32_t frames = 0; // As BlockMidiEvent::frames.
};

// Pedals (ADR-0035) run in their own stream beside the notes, so consumers
// that only know notes are unaffected. At the same sample, consumers apply
// pedals before notes.
inline constexpr std::size_t max_pedal_events_per_block = 256;

struct BlockPedalEvent {
  std::uint32_t offset = 0;
  project::MidiPedal pedal = project::MidiPedal::sustain;
  float value = 0.0F;
};

struct ScheduledPedalEvent {
  timeline::SamplePosition sample;
  project::ClipId clip;
  project::MidiPedal pedal;
  double value;
  bool clip_end = false; // The release a clip's end adds; it sorts first.
};

// Each pedal's value, indexed by MidiPedal.
using PedalState = std::array<double, project::midi_pedal_count>;

enum class BlockMidiStatus : std::uint8_t { ready, invalid_range, insufficient_storage };
struct BlockMidiResult {
  BlockMidiStatus status;
  std::size_t count;
};

class InstrumentEvents {
public:
  InstrumentEvents(project::TrackId track, std::vector<ScheduledMidiEvent> events,
                   std::vector<ScheduledPedalEvent> pedals = {})
      : track_(track), events_(std::move(events)), pedals_(std::move(pedals)) {}

  [[nodiscard]] project::TrackId track() const noexcept { return track_; }
  [[nodiscard]] const std::vector<ScheduledMidiEvent>& events() const noexcept { return events_; }
  [[nodiscard]] const std::vector<ScheduledPedalEvent>& pedals() const noexcept { return pedals_; }

  // Callback safe: binary search and bounded copies into caller-owned storage.
  // The plan compiler guarantees 1024 slots suffice for any valid block.
  [[nodiscard]] BlockMidiResult copy_block(timeline::SamplePosition start, std::uint32_t frames,
                                           std::span<BlockMidiEvent> output) const noexcept;
  // The same for pedals; 256 slots suffice for any valid block.
  [[nodiscard]] BlockMidiResult copy_pedal_block(timeline::SamplePosition start,
                                                 std::uint32_t frames,
                                                 std::span<BlockPedalEvent> output) const noexcept;
  // Callback safe: copy_pedal_block, preceded at offset 0 by every pedal
  // held at `start` when `chase` is set: the first block after a seek or
  // loop, when the instrument starts with every pedal up. `output` needs
  // max_pedal_events_per_block + midi_pedal_count slots.
  [[nodiscard]] BlockMidiResult copy_pedals(timeline::SamplePosition start, std::uint32_t frames,
                                            bool chase,
                                            std::span<BlockPedalEvent> output) const noexcept;
  // Callback safe: each pedal's value from the last event before `sample`
  // (0 when none), for chasing pedals held across a seek or loop.
  [[nodiscard]] PedalState pedal_state_at(timeline::SamplePosition sample) const noexcept;
  // Control thread: the pedal events before `end`, plus a release at `end`
  // for every pedal still down there, for an export cut at `end`.
  [[nodiscard]] std::vector<ScheduledPedalEvent> pedals_until(std::int64_t end) const;

private:
  project::TrackId track_;
  std::vector<ScheduledMidiEvent> events_;
  std::vector<ScheduledPedalEvent> pedals_;
};

// The plan's notes still sounding between blocks. A planned loop wrap moves
// the playhead back without a new processing generation (the instrument keeps
// its voices for a seamless loop), so a note held at the loop end never
// reaches its note-off and the next pass would stack another voice on it.
// copy_block() releases every held note at offset 0 of a block that does not
// continue the previous one, as DAWs release held notes at the loop end; a
// new generation just forgets them, because the instrument resets anyway.
// Callback only; fixed capacity, no allocation. A compiled plan never holds
// more than `capacity` notes at once, so every held note is tracked.
class HeldNotes {
public:
  static constexpr std::size_t capacity = max_concurrent_midi_notes;
  // Room callers give copy_block(): the plan's events plus every release.
  static constexpr std::size_t block_capacity = max_midi_events_per_block + capacity;
  // What an instrument accepts per block: that, plus notes played live
  // (computer keyboard, piano-roll keys) merged in by the player.
  static constexpr std::size_t live_capacity = 128;
  static constexpr std::size_t instrument_capacity = block_capacity + live_capacity;

  // The plan's events for the block, preceded by releases when the block
  // jumps, sorted as instruments require (by offset, offs before ons, then
  // note id). `output` needs block_capacity slots.
  [[nodiscard]] BlockMidiResult copy_block(const InstrumentEvents& events,
                                           timeline::SamplePosition start, std::uint32_t frames,
                                           std::uint64_t generation,
                                           std::span<BlockMidiEvent> output) noexcept;
  void reset() noexcept {
    count_ = 0;
    started_ = false;
  }

private:
  std::array<BlockMidiEvent, capacity> held_{};
  std::size_t count_ = 0;
  std::int64_t next_sample_ = 0;
  std::uint64_t generation_ = 0;
  bool started_ = false;
};

// Pedal stream order: by sample, a clip-end release before any new value at
// the same sample, then by pedal and clip.
[[nodiscard]] bool pedal_event_less(const ScheduledPedalEvent& a,
                                    const ScheduledPedalEvent& b) noexcept;

class CompiledMidiPlan {
public:
  explicit CompiledMidiPlan(std::vector<InstrumentEvents> instruments)
      : instruments_(std::move(instruments)) {}
  [[nodiscard]] const std::vector<InstrumentEvents>& instruments() const noexcept {
    return instruments_;
  }

private:
  std::vector<InstrumentEvents> instruments_;
};

// Control thread only. Live playback and offline export must use this same
// compiler. A failed compile leaves the prior audible plan in place.
[[nodiscard]] foundation::Result<CompiledMidiPlan>
compile_midi_events(const project::Snapshot& snapshot);

} // namespace oscillate::engine

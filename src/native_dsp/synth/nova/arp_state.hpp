#pragma once

#include "oscillate/foundation/id.hpp"
#include "oscillate/native_dsp/synth/nova/arp.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// NoteGenerator's private state. Every container is fixed-capacity so the
// callback never allocates. The pipeline is played → sequencer → arpeggiator
// → voices; a stage that is off is skipped.
namespace oscillate::native_dsp::synth::nova {
namespace arp_detail {

inline constexpr std::size_t key_count = 128;
inline constexpr std::size_t sounding_capacity = max_generated_notes;
inline constexpr std::size_t held_capacity = 256;
inline constexpr std::size_t sequence_capacity = key_count * 4;
inline constexpr std::int64_t no_step = std::numeric_limits<std::int64_t>::min();
// The generator's own clip id on every minted note; note ids count from 1.
inline constexpr foundation::IdBits generated_clip{0x6e6f76612d617270ULL, 0x67656e6572617465ULL};

// One block on one musical axis: song beats under a running transport, or a
// free 120 BPM clock started by the first held note. An event at beat b
// lands on frame ceil((b - beat) / per_frame), so every block split puts
// it on the same absolute frame.
struct Axis {
  double beat = 0.0;
  double per_frame = 0.0;
  [[nodiscard]] std::int64_t raw_frame(double at) const noexcept; // May be negative.
  [[nodiscard]] std::int64_t due_frame(double at) const noexcept; // At least 0.
  [[nodiscard]] double beat_at(std::int64_t frame) const noexcept {
    return beat + per_frame * static_cast<double>(frame);
  }
};

// Output into a fixed buffer; push fails when it is full.
class Emitter {
public:
  explicit Emitter(std::span<GeneratedNote> buffer) noexcept : buffer_(buffer) {}
  bool push(const GeneratedNote& note) noexcept;
  // Removes an on of this note at this frame, for a zero-length played note
  // passed through; false when there is none.
  bool cancel_on(const NoteEvent& note, std::uint32_t offset) noexcept;
  // By frame, offs before ons, then key and id: one canonical order.
  void sort() noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::span<const GeneratedNote> notes() const noexcept {
    return buffer_.first(count_);
  }

private:
  std::span<GeneratedNote> buffer_;
  std::size_t count_ = 0;
};

struct IdMinter {
  std::uint64_t next = 1;
  [[nodiscard]] NoteEvent mint(std::uint8_t key, std::uint8_t velocity) noexcept {
    return {generated_clip, {0x6e6f7661ULL, next++}, key, velocity};
  }
};

// A generated note that is sounding, with the beat its off is due on.
struct Sounding {
  NoteEvent note{};
  double end_beat = 0.0;
  bool tied = false; // Ends at the next step, which may continue it.
  int clip = -1;     // The sequencer clip that played it.
};

class SoundingSet {
public:
  bool add(const Sounding& note) noexcept;
  void remove(std::size_t index) noexcept;
  void clear() noexcept { count_ = 0; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] Sounding& operator[](std::size_t index) noexcept { return notes_[index]; }
  // Offs for every note due before `limit` (or at it when inclusive); a note
  // whose off does not fit stays, overdue, for the next block's frame 0.
  void flush(const Axis& axis, std::int64_t limit, bool inclusive, Emitter& out) noexcept;
  // Offs for every note at `offset`, then forgets them.
  void release(std::uint32_t offset, Emitter& out) noexcept;

private:
  std::array<Sounding, sounding_capacity> notes_{};
  std::size_t count_ = 0;
};

// Keys held into a stage, counted so overlapping notes on one key pair up.
struct KeyPool {
  std::array<std::uint16_t, key_count> count{};
  std::array<bool, key_count> latched{};
  std::array<std::uint8_t, key_count> velocity{};
  std::array<std::uint64_t, key_count> stamp{};
  std::uint32_t active = 0; // Sum of count.
  std::uint64_t next_stamp = 1;

  [[nodiscard]] bool held(std::size_t key) const noexcept { return count[key] > 0 || latched[key]; }
  [[nodiscard]] bool empty() const noexcept;
  // The most recently pressed held key, or -1.
  [[nodiscard]] int newest() const noexcept;
  void press(std::uint8_t key, std::uint8_t velocity) noexcept;
  // True when the key was held and is now released.
  bool release(std::uint8_t key, bool latch) noexcept;
  void unlatch() noexcept { latched.fill(false); }
  void clear() noexcept;
};

// Played notes as they are physically held, in press order, for replay.
class HeldNotes {
public:
  void add(const NoteEvent& note) noexcept;
  void remove(const NoteEvent& note) noexcept;
  void clear() noexcept { count_ = 0; }
  [[nodiscard]] std::span<const NoteEvent> notes() const noexcept {
    return std::span(notes_).first(count_);
  }

private:
  std::array<NoteEvent, held_capacity> notes_{};
  std::size_t count_ = 0;
};

struct BlockContext {
  const Axis& axis;
  std::uint32_t frames;
  IdMinter& ids;
  double beats_per_bar = 4.0; // For variation bars.
};

// The most notes one step plays at a time: every held key (Chord, Strum),
// or one note and the chord lane's three.
inline constexpr std::size_t plan_capacity = key_count + 4;
// Order modes visit at most twice the sequence (Up/Down, Thumb, Pinky).
inline constexpr std::size_t order_capacity = 2 * sequence_capacity;

// One step's plan, made when the step starts: its notes, played as `hits`
// ratchet hits of `count` notes each (spread over `strum` beats of each hit
// in Strum). Its events (hit × count + note) may fall in later blocks; all
// of them land before the next step.
struct StepPlan {
  std::int64_t step = no_step;
  std::uint32_t events = 0; // hits × count
  std::uint32_t next = 0;   // The next event to play.
  std::uint32_t hits = 1;
  std::uint32_t count = 0;
  double start = 0.0;      // Beats.
  double next_start = 0.0; // The next step's start.
  double hit_beats = 0.0;  // One hit's share of the step.
  double hit_gate = 0.0;   // A hit's note length.
  double strum = 0.0;      // Beats a hit's notes spread over.
  bool strum_down = false;
  bool tie = false; // The last hit holds into the next step.
  float mod = 0.0F;
  bool accent = false;
  std::array<std::uint8_t, plan_capacity> keys{};
  std::array<std::uint8_t, plan_capacity> velocities{};
  [[nodiscard]] bool pending() const noexcept { return step != no_step && next < events; }
  [[nodiscard]] double time(std::uint32_t event) const noexcept;
  void clear() noexcept {
    step = no_step;
    events = 0;
    next = 0;
  }
};

class ArpStage {
public:
  void run(std::span<const GeneratedNote> in, const BlockContext& block,
           const ArpSettings& settings, Emitter& out) noexcept;
  // Offs for every sounding note at frame 0 and a fresh step count; the
  // held keys stay.
  void release(Emitter& out) noexcept;
  void forget() noexcept;   // Held keys too, with no offs.
  void restart() noexcept { // A new axis: count steps afresh.
    last_step_ = no_step;
    plan_.clear();
  }
  void unlatch() noexcept { pool_.unlatch(); }
  // A planned step still to finish (a ratchet's later hits, a strum's
  // later notes) keeps the stage busy even after the keys are released, so
  // the free clock never stops between hits at a split-dependent block edge.
  [[nodiscard]] bool idle() const noexcept {
    return pool_.empty() && sounding_.size() == 0 && !plan_.pending();
  }
  [[nodiscard]] int step() const noexcept { return step_; }
  [[nodiscard]] bool variation() const noexcept { return variation_; }

private:
  void apply(const GeneratedNote& note, const ArpSettings& settings) noexcept;
  void plan_step(std::int64_t n, std::int64_t frame, const BlockContext& block,
                 const ArpSettings& settings, Emitter& out) noexcept;
  void play_event(std::int64_t frame, const BlockContext& block, Emitter& out) noexcept;
  // The lane values that pick a step's notes.
  struct StepLanes {
    int note = 0;
    int octave = 0;
    ArpChord chord = ArpChord::off;
  };
  // The notes step k plays into plan_ (keys and played velocities); returns
  // how many.
  std::size_t choose(std::int64_t k, const StepLanes& lanes, const ArpSettings& settings) noexcept;
  std::size_t build_sequence(const ArpSettings& settings) noexcept;
  std::size_t build_order(std::size_t length, ArpMode mode) noexcept;
  [[nodiscard]] std::int64_t walk(std::int64_t k, std::size_t length, std::uint32_t seed) noexcept;
  [[nodiscard]] int shift_degrees(int key, int degrees, const ArpSettings& settings) const noexcept;

  KeyPool pool_;
  SoundingSet sounding_;
  StepPlan plan_;
  std::int64_t last_step_ = no_step;
  std::int64_t anchor_ = 0;
  bool anchor_pending_ = true;
  int step_ = -1;
  bool variation_ = false;
  // The held keys, ascending (or as played), before octaves.
  std::array<std::uint8_t, key_count> base_{};
  std::size_t base_count_ = 0;
  // base_ over the octaves: a position is a chord-tone degree.
  std::array<std::uint8_t, sequence_capacity> sequence_{};
  std::array<std::uint8_t, sequence_capacity> sequence_velocity_{};
  // The mode's visiting order, as positions in sequence_.
  std::array<std::uint16_t, order_capacity> order_{};
  // Random walk: the last position found, to step on from.
  std::int64_t walk_step_ = no_step;
  std::size_t walk_length_ = 0;
  std::uint32_t walk_seed_ = 0;
  std::int64_t walk_position_ = 0;
};

class SequencerStage {
public:
  void run(std::span<const GeneratedNote> in, const BlockContext& block,
           const SequencerSettings& settings, const SequencerClips& clips, bool transport,
           Emitter& out) noexcept;
  void release(Emitter& out) noexcept;
  // Note-triggered clips restart from the next 1/16 of a new axis; clips
  // that loop with the song stop when there is no song.
  void requantise(const Axis& axis, bool transport) noexcept;
  void forget() noexcept;
  [[nodiscard]] bool idle() const noexcept;
  [[nodiscard]] int playing_clip() const noexcept;
  [[nodiscard]] double playhead(const SequencerClips& clips, double beat) const noexcept;

private:
  struct Player {
    bool active = false;
    bool from_press = false; // Starts at origin; otherwise loops with the song.
    double origin = 0.0;
    std::uint64_t stamp = 0;
  };
  struct PendingOn {
    std::int64_t frame = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
    int clip = 0;
    double end_beat = 0.0;
  };
  void apply(const GeneratedNote& note, std::uint32_t frame, const BlockContext& block,
             const SequencerSettings& settings, bool transport, Emitter& out) noexcept;
  void start(std::size_t clip, double origin, bool from_press) noexcept;
  void stop(std::size_t clip, std::uint32_t frame, const Axis& axis, Emitter& out) noexcept;
  void play(std::uint32_t from, std::uint32_t to, const BlockContext& block,
            const SequencerSettings& settings, const SequencerClips& clips, Emitter& out) noexcept;

  std::array<Player, sequencer_clips> players_{};
  std::array<std::uint16_t, sequencer_clips> triggers_{};
  KeyPool keys_;                // Keys that transpose (follow on).
  std::uint32_t forwarded_ = 0; // Keys passed on (follow off) and held.
  std::uint64_t next_stamp_ = 1;
  SoundingSet sounding_;
  std::array<PendingOn, max_generated_notes> pending_{};
};

} // namespace arp_detail

struct NoteGenerator::State {
  double sample_rate = 48'000.0;
  ArpSettings arp{};
  SequencerSettings sequencer{};
  const SequencerClips* clips = nullptr;
  SequencerClips no_clips{};

  // Changes set() asks process() to apply at the next block start.
  bool rebuild = false;     // The pipeline changed: release all, replay held notes.
  bool release_arp = false; // The arp pattern changed: release its notes.
  bool unlatch = false;
  bool sequencer_into_arp = false; // The sequencer's notes feed the arp.

  bool have_axis = false;
  bool transport_axis = false;
  double expected_beat = 0.0;
  bool clock_running = false;
  std::int64_t clock_frame = 0;

  arp_detail::IdMinter ids;
  arp_detail::HeldNotes held;
  std::array<NoteEvent, arp_detail::sounding_capacity> passed{}; // Played notes sounding.
  std::size_t passed_count = 0;
  arp_detail::ArpStage arp_stage;
  arp_detail::SequencerStage sequencer_stage;
  std::array<GeneratedNote, max_generated_notes> sequencer_out{};
  std::array<GeneratedNote, arp_detail::held_capacity + 4 * max_generated_notes> input{};

  int playing_clip = -1;
  double playhead = 0.0;
  int arp_step = -1;
  bool arp_variation = false;

  void forget() noexcept;
  [[nodiscard]] bool idle() const noexcept;
  [[nodiscard]] std::span<const GeneratedNote> prepare_input(std::span<const GeneratedNote> played,
                                                             bool replay) noexcept;
  void track_held(std::span<const GeneratedNote> played) noexcept;
  void track_passed(std::span<const GeneratedNote> out) noexcept;
  void release_passed(arp_detail::Emitter& out) noexcept;
  [[nodiscard]] std::size_t process(std::span<const GeneratedNote> played,
                                    const Transport& transport, std::uint32_t frames,
                                    std::span<GeneratedNote, max_generated_notes> out) noexcept;
};

} // namespace oscillate::native_dsp::synth::nova

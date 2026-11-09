#pragma once

#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/foundation/id.hpp"
#include "oscillate/native_dsp/parameter_control.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// Note effects (ADR-0059): MIDI effects that run on a track's notes before
// its built-in instrument. Each engine is a versioned parameter table (the
// synths' ParameterDescriptor, so validation, automation domains and panels
// are shared) plus a kernel that turns a block's note events into new ones.
// Kernels are deterministic functions of the project frames they see: they
// time everything in project ticks through the shared tempo map, so any
// block split, live playback and export produce the same notes.
namespace oscillate::native_dsp::note_effects {
// Events a stage may receive and return per block: what an instrument takes.
inline constexpr std::size_t block_event_capacity = engine::HeldNotes::instrument_capacity;
// Room a kernel keeps for the note-offs of the notes it is sounding, so a
// crowded block drops new notes rather than strand old ones.
inline constexpr std::size_t note_off_reserve = 256;

// The project tick of a project frame: the host's conversion through the
// shared tempo map (native_inserts::TempoTicks), so note effects never
// evaluate a tempo map themselves. Callback safe; false when the frame
// cannot be placed.
class FrameTicks {
public:
  virtual ~FrameTicks() = default;
  [[nodiscard]] virtual bool tick_of(std::uint64_t frame, std::int64_t& tick) const noexcept = 0;
};

// The musical clock of one block. A frame's tick is the shared tempo map's
// exact conversion of its project frame, so a step lands on the same frame
// however the blocks are split. Callback safe.
class NoteClock {
public:
  NoteClock(const FrameTicks& ticks, std::uint64_t first_frame, std::uint32_t frames) noexcept;
  // False when the tempo map could not place the block (the block fails).
  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] std::uint32_t frames() const noexcept { return frames_; }
  [[nodiscard]] std::uint64_t first_frame() const noexcept { return first_frame_; }
  [[nodiscard]] const FrameTicks& ticks() const noexcept { return *ticks_; }
  // The tick of a frame of the block.
  [[nodiscard]] std::int64_t tick_at(std::uint32_t offset) const noexcept;
  // The tick of the frame before the block (-1 at the song's start): events
  // at or before it belonged to an earlier block.
  [[nodiscard]] std::int64_t previous_tick() const noexcept { return previous_tick_; }
  [[nodiscard]] std::int64_t last_tick() const noexcept { return last_tick_; }
  // True when `tick` falls in this block: after previous_tick(), at or
  // before last_tick().
  [[nodiscard]] bool holds(std::int64_t tick) const noexcept {
    return tick > previous_tick_ && tick <= last_tick_;
  }
  // The first frame of the block whose tick reaches `tick` (0 for an
  // earlier tick, frames() for a later one).
  [[nodiscard]] std::uint32_t offset_of(std::int64_t tick) const noexcept;

private:
  const FrameTicks* ticks_;
  std::uint64_t first_frame_;
  std::uint32_t frames_;
  bool valid_ = false;
  std::int64_t first_tick_ = 0;
  std::int64_t previous_tick_ = -1;
  std::int64_t last_tick_ = 0;
};

// The track's own notes just after a block (ADR-0084), for a kernel that
// acts before a note arrives: Comp pushes a chord change early. An event
// carries the tick of the frame before it: it joins the first step whose tick
// passes that, exactly as it does when it arrives as input in its own block.
struct AheadEvent {
  std::int64_t previous_tick = 0;
  foundation::IdBits clip_id_bits;
  foundation::IdBits note_id_bits;
  engine::MidiEventKind kind = engine::MidiEventKind::note_off;
  std::uint8_t key = 0;
};
// How far past a block kernels see: a dotted 8th, Comp's longest push.
inline constexpr std::int64_t note_lookahead_ticks = timeline::ticks_per_quarter * 3 / 4;
// Comp's grid and the phrases its Motion is measured in (comp.phrase's
// choices), shared with the panels that set them.
inline constexpr int comp_steps_per_bar = 16;
inline constexpr std::array<int, 3> comp_phrase_bars{4, 8, 16};
// Events a block sees ahead; a crowd beyond it is not seen.
inline constexpr std::size_t ahead_event_capacity = 256;
// Callback safe. The events of `events` (sorted by sample, as a compiled
// plan holds them) after the block `clock` covers whose previous_tick is
// before last_tick() + note_lookahead_ticks, in order, up to output.size();
// returns how many. Song time runs straight on: the lookahead does not know
// where a transport loop wraps, so near a loop end it sees the notes after
// the loop end, not those at the loop start.
[[nodiscard]] std::size_t copy_ahead(std::span<const engine::ScheduledMidiEvent> events,
                                     const NoteClock& clock, std::span<AheadEvent> output) noexcept;

// A parameter's value at a frame of the block: its lane's when automated,
// else the block's value. Also carries the track's own notes ahead of the
// block (empty when none are known, as for notes played live).
class NoteParameters {
public:
  NoteParameters(std::span<const double> values, std::span<const ParameterLane> lanes,
                 std::span<const AheadEvent> ahead = {}) noexcept
      : values_(values), lanes_(lanes), ahead_(ahead) {}
  [[nodiscard]] std::span<const AheadEvent> ahead() const noexcept { return ahead_; }
  [[nodiscard]] double at(std::size_t parameter, std::uint32_t offset) const noexcept;
  // A stepped or choice parameter, rounded.
  [[nodiscard]] int index_at(std::size_t parameter, std::uint32_t offset) const noexcept;

private:
  std::span<const double> values_;
  std::span<const ParameterLane> lanes_;
  std::span<const AheadEvent> ahead_;
};

// Engine-specific note processing. Created on control; every other member
// runs only on the one callback that owns the chain.
class NoteEffectKernel {
public:
  virtual ~NoteEffectKernel() = default;
  // `input` is sorted as instruments require (by offset, offs before ons,
  // then note id). Writes the block's events into `output`, sorted alike,
  // and returns how many; `output` holds block_event_capacity events.
  [[nodiscard]] virtual std::size_t process(std::span<const engine::BlockMidiEvent> input,
                                            std::span<engine::BlockMidiEvent> output,
                                            const NoteClock& clock,
                                            const NoteParameters& parameters) noexcept = 0;
  // A seek, or a loop that starts a new transport generation: the
  // instrument drops every voice, so the kernel forgets every note.
  virtual void reset() noexcept = 0;
  // The stage was switched off: writes a note-off at offset 0 for every
  // note the kernel made that may still sound, returns how many, and
  // forgets every note. Input notes it held pass through from now on, and
  // a note-off it does not know always passes through.
  [[nodiscard]] virtual std::size_t flush(std::span<engine::BlockMidiEvent> output) noexcept = 0;
};

struct NoteEffectEngine {
  const EngineDescriptor* descriptor = nullptr;
  // Control only. Notes the kernel makes sound under `generated_clip`, with
  // its own note ids, so they never pair with the project's notes.
  std::unique_ptr<NoteEffectKernel> (*create_kernel)(foundation::IdBits generated_clip) = nullptr;
  // Control only: quarter-note beats a note can keep sounding after its
  // input note ends (echo repeats), for the instrument's tail.
  double (*tail_beats)(std::span<const double> values) = nullptr;
};

[[nodiscard]] std::span<const NoteEffectEngine* const> engines() noexcept;
[[nodiscard]] const NoteEffectEngine* find_engine(std::string_view id) noexcept;

// Project binding (control only). Unknown engines, newer versions, unknown
// ids and out-of-range values fail without guessing.
struct ResolvedNoteEffect {
  const NoteEffectEngine* engine = nullptr;
  std::vector<double> values; // Dense, in the engine's table order.
};
[[nodiscard]] foundation::Result<ResolvedNoteEffect>
resolve_note_effect(const project::NoteEffectInsert& insert);
[[nodiscard]] foundation::Result<ResolvedNoteEffect>
resolve_note_effect(std::string_view engine, std::uint32_t engine_version,
                    std::span<const project::NativeInstrumentParameter> parameters);
// Dense values -> the saved sparse form (non-default values, table order).
[[nodiscard]] std::vector<project::NativeInstrumentParameter>
sparse_note_effect_parameters(const NoteEffectEngine& engine, std::span<const double> values);
[[nodiscard]] foundation::Result<std::size_t>
note_effect_parameter_index(const NoteEffectEngine& engine, std::string_view parameter_id);
[[nodiscard]] foundation::Result<project::AutomationDomain>
note_effect_automation_domain(const NoteEffectEngine& engine, std::string_view parameter_id);

// One stage's controls for a block: whether it runs, its values in table
// order and its automated parameters' lanes.
struct StageControls {
  bool enabled = true;
  std::span<const double> values;
  std::span<const ParameterLane> lanes;
};

// A track's note effects in chain order, disabled ones included (switching
// one on or off is a live edit). Created on control; process and reset run
// on the one callback that owns the chain.
class NoteEffectChain {
public:
  [[nodiscard]] static foundation::Result<std::unique_ptr<NoteEffectChain>>
  create(std::span<const project::NoteEffectInsert> inserts);

  [[nodiscard]] std::size_t size() const noexcept { return stages_.size(); }
  [[nodiscard]] const NoteEffectEngine& engine(std::size_t stage) const noexcept {
    return *stages_[stage].engine;
  }
  [[nodiscard]] project::InsertSlotId slot(std::size_t stage) const noexcept {
    return stages_[stage].slot;
  }
  // True when the inserts have the chain's slots and engines in its order.
  [[nodiscard]] bool same_layout(std::span<const project::NoteEffectInsert> inserts) const noexcept;

  // Callback only. Runs each enabled stage over `events` (sorted, `count`
  // of them) and returns the final events, which live in `events` or
  // `scratch`; both hold block_event_capacity events. `controls` has one
  // entry per stage. Every stage sees `ahead`, the track's own notes after
  // the block (copy_ahead), as they are before any note effect. Empty when
  // the clock is invalid.
  [[nodiscard]] std::span<const engine::BlockMidiEvent>
  process(std::span<engine::BlockMidiEvent> events, std::size_t count,
          std::span<engine::BlockMidiEvent> scratch, const NoteClock& clock,
          std::span<const StageControls> controls, std::span<const AheadEvent> ahead = {}) noexcept;
  void reset() noexcept;

private:
  struct Stage {
    project::InsertSlotId slot;
    const NoteEffectEngine* engine = nullptr;
    std::unique_ptr<NoteEffectKernel> kernel;
    bool enabled = true; // Callback only: on at the last block.
  };
  std::vector<Stage> stages_;
};

} // namespace oscillate::native_dsp::note_effects

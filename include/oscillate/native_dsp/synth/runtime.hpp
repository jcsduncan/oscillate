#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/engine/synth_media.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/document_state.hpp"
#include "oscillate/native_dsp/parameter_control.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/pedal_latch.hpp"
#include "oscillate/native_dsp/synth/lock.hpp"
#include "oscillate/native_dsp/telemetry.hpp"
#include "oscillate/native_dsp/transport.hpp"
#include "oscillate/project/model.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::engine {
class AudioWorkerPool;
} // namespace oscillate::engine

namespace oscillate::native_dsp::synth {

struct NoteEvent {
  foundation::IdBits clip;
  foundation::IdBits note;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
  // A note-on's frames until its note-off when known in advance (a note in
  // a clip), else 0 (engine::BlockMidiEvent::frames).
  std::uint32_t frames = 0;
};

struct KernelStatus {
  std::uint32_t active_voices = 0;
  std::uint32_t voice_steals = 0; // Cumulative.
  // Sounding notes still using next-note values that differ from current.
  bool deferred_changes = false;
};

// Decoded managed media (ADR-0057); see engine/synth_media.hpp.
using engine::SynthMedia;
using engine::SynthMediaAsset;
using engine::SynthMediaEntry;

// Engine-specific DSP. Created and destroyed on control; every other member
// runs only on the one callback that owns the runtime.
class SynthKernel {
public:
  virtual ~SynthKernel() = default;

  // At each control tick. values are effective plain values in table order;
  // frames_to_next_tick lets the kernel ramp its per-frame state across the
  // interval. transport is evaluated at the tick frame.
  virtual void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
                       const Transport& transport) noexcept = 0;
  virtual void note_on(const NoteEvent& event) noexcept = 0;
  virtual void note_off(const NoteEvent& event) noexcept = 0;
  // Additive. frames never spans a control tick or a note event.
  virtual void render(std::span<engine::StereoFrame> output) noexcept = 0;
  // Seek or loop: silence every voice and restart free modulation. The
  // kernel declicks its own last output.
  virtual void reset() noexcept = 0;
  [[nodiscard]] virtual KernelStatus status() const noexcept = 0;
  // Callback only, after each block: writes up to out.size() values of the
  // engine's telemetry layout and returns how many. Engines without
  // telemetry write nothing.
  [[nodiscard]] virtual std::size_t telemetry(std::span<float> out) const noexcept {
    (void)out;
    return 0;
  }
  // At a block start (and once on control before the first block): the
  // engine's own compiled document, valid until the next adoption. Engines
  // without a document never see this.
  virtual void adopt_document(const DocumentState& state) noexcept { (void)state; }
  // A pedal's new value, 0..1, for engines that model their pedals
  // (SynthEngine::models_pedals). Others get generic pedals from the
  // runtime and never see this.
  virtual void pedal(project::MidiPedal pedal, double value) noexcept {
    (void)pedal;
    (void)value;
  }
  // Offline renders only (export, bounce, tests), on the rendering thread
  // between blocks: return once the content the next blocks read is in
  // memory (a sampled instrument streaming from disk). A realtime callback
  // never calls it. Engines without streamed content do nothing.
  virtual void await_content() noexcept {}
  // Before each note and render: the project frame they start at. Engines
  // that read followed notes (adopt_lock) place themselves with it.
  virtual void locate(std::uint64_t project_frame) noexcept { (void)project_frame; }
  // Kick & Bass Lock (ADR-0072), at a block start for engines that follow
  // another track (SynthEngine::lock_follows): the followed notes, valid
  // until the next adoption. Empty when nothing is followed.
  virtual void adopt_lock(const LockState& state) noexcept { (void)state; }
  // At a block start whenever it changes: the project's key, or nullopt when
  // none is set (the starting state). Engines that tune to it (Knock's
  // pads) read it; a function of the snapshot, so export equals playback.
  virtual void project_key(std::optional<project::MusicalKey> key) noexcept { (void)key; }
  // Before each block: audio helpers (ADR-0085) the kernel may hand
  // independent work to, such as its voices, with
  // AudioWorkerPool::run_nested; null for none. What it renders must not
  // depend on them. Engines that do all their work alone ignore it.
  virtual void use_workers(engine::AudioWorkerPool* workers) noexcept { (void)workers; }
};

struct SynthEngine {
  const EngineDescriptor* descriptor = nullptr;
  // Control only. Allocates everything the kernel needs at this rate.
  std::unique_ptr<SynthKernel> (*create_kernel)(double sample_rate) = nullptr;
  // Control only: frames a released note can keep sounding with these
  // values, including effect tails. Offline export bounds its render with it.
  std::uint64_t (*tail_frames)(std::span<const double> values, double sample_rate) = nullptr;
  // True when the kernel models pedals itself (a piano's dampers and una
  // corda); otherwise the runtime applies generic sustain, sostenuto and
  // soft pedals around it (ADR-0035).
  bool models_pedals = false;
  // Structured state (ADR-0057). Null for engines whose state is only their
  // parameter table: they accept only an empty document.
  // Control only: is this document's meaning valid for the engine? Media
  // slots are checked by id and kind, not decoded.
  foundation::Result<void> (*validate_document)(const project::NativeSynthDocument& document) =
      nullptr;
  // Control only, for a validated document: compile it with its managed
  // media decoded. The result must not allocate when its kernel reads it.
  // `previous` is the state last compiled for the same runtime (null for the
  // first): reuse what has not changed, such as built tables or effect
  // instances whose sound must carry on. The kernel may still be reading
  // `previous` until it adopts the result.
  foundation::Result<std::shared_ptr<const DocumentState>> (*prepare_document)(
      const project::NativeSynthDocument& document, const SynthMedia& media, double sample_rate,
      const DocumentState* previous) = nullptr;
  // The document a factory preset, or the defaults (nullopt), start with;
  // empty when null.
  project::NativeSynthDocument (*preset_document)(std::optional<std::size_t> preset) = nullptr;
  // Kick & Bass Lock (ADR-0072). An engine with lock_follows reads the notes
  // of the tracks playing that engine while lock_enabled holds on both
  // sides; a followed engine may give each hit's amplitude envelope
  // (lock_envelope_frames apart, peak 1) for a velocity, a note's length in
  // frames (0 when open-ended) and the tempo at the hit. Control only.
  std::string_view lock_follows{};
  bool (*lock_enabled)(std::span<const double> values) = nullptr;
  std::vector<float> (*lock_envelope)(std::span<const double> values,
                                      const project::NativeSynthDocument& document,
                                      std::uint8_t velocity, std::uint32_t note_frames, double bpm,
                                      double sample_rate) = nullptr;
};

enum class SynthProcessStatus : std::uint8_t {
  rendered,
  invalid_block,
  invalid_events,
  invalid_lanes
};

struct SynthProcessReport {
  SynthProcessStatus status = SynthProcessStatus::rendered;
  KernelStatus kernel{};
};

// One serialized control producer, one audio callback consumer: create on
// control, process on exactly one callback.
class NativeSynthRuntime {
public:
  // values must already be resolved against the engine table, and the
  // document validated by the engine; media holds its decoded managed slots.
  [[nodiscard]] static foundation::Result<std::unique_ptr<NativeSynthRuntime>>
  create(const SynthEngine& engine, std::span<const double> values,
         const project::NativeSynthDocument& document, SynthMedia media, double sample_rate);
  // With an empty document.
  [[nodiscard]] static foundation::Result<std::unique_ptr<NativeSynthRuntime>>
  create(const SynthEngine& engine, std::span<const double> values, double sample_rate);

  // Callback only. Output is additive. Events must be sorted and in range;
  // lanes must name live, automatable parameters with one finite in-range
  // value per frame. A rejected block leaves output and state unchanged.
  [[nodiscard]] SynthProcessReport
  process(std::span<engine::StereoFrame> output, std::span<const engine::BlockMidiEvent> events,
          std::uint64_t project_frame, std::uint64_t transport_generation,
          const Transport& transport, std::span<const ParameterLane> lanes) noexcept;
  // With pedals (ADR-0035): sorted, in range, values 0..1. At the same frame
  // pedals act before notes. A transport-generation change resets the
  // pedals with the voices; the caller chases held pedals at offset 0.
  [[nodiscard]] SynthProcessReport
  process(std::span<engine::StereoFrame> output, std::span<const engine::BlockMidiEvent> events,
          std::span<const engine::BlockPedalEvent> pedals, std::uint64_t project_frame,
          std::uint64_t transport_generation, const Transport& transport,
          std::span<const ParameterLane> lanes) noexcept;

  // Control only. The latest accepted offer is adopted at the next block;
  // panel jumps glide over each parameter's glide time. Fails when the
  // mailbox is full, the revision does not increase or a value is invalid.
  [[nodiscard]] bool offer_values(std::span<const double> values, std::uint64_t revision) noexcept;
  // Control only: a panel control mid-drag, with no saved revision
  // (ParameterControl::offer_live).
  [[nodiscard]] bool offer_live_values(std::span<const double> values) noexcept {
    return control_.offer_live(values);
  }
  [[nodiscard]] std::uint64_t applied_revision() const noexcept;
  // Control only: compile a validated document against this runtime's media
  // and offer it, before offer_values with the same revision; the callback
  // adopts both at one block start, so applied_revision() covers both.
  // Revisions must not decrease (a retry may repeat one). False, with
  // nothing offered, when the document's managed media differ from those
  // this runtime was prepared with: the caller rebuilds the source.
  [[nodiscard]] foundation::Result<bool>
  offer_document(const project::NativeSynthDocument& document, std::uint64_t revision);
  // Control only: the document last offered (or prepared with).
  [[nodiscard]] const project::NativeSynthDocument& document() const noexcept { return document_; }
  // Control-side view of the last rendered block, for honest panel status.
  // Relaxed: it is a display hint, never a synchronisation point.
  [[nodiscard]] KernelStatus observed_status() const noexcept {
    return {observed_voices_.load(std::memory_order_relaxed),
            observed_steals_.load(std::memory_order_relaxed),
            observed_deferred_.load(std::memory_order_relaxed)};
  }

  [[nodiscard]] const SynthEngine& engine() const noexcept { return engine_; }
  // Callback only, before process: the helpers this block's kernel may use
  // (SynthKernel::use_workers), or null.
  void use_workers(engine::AudioWorkerPool* workers) noexcept { kernel_->use_workers(workers); }

  // Offline renders only, on the thread that calls process, between blocks:
  // waits for streamed content (SynthKernel::await_content), so an offline
  // render never outruns the disk.
  void await_content() noexcept { kernel_->await_content(); }
  // Control only: the notes this runtime's engine follows (Kick & Bass
  // Lock), adopted at the next block start. False for a null state.
  [[nodiscard]] bool offer_lock(std::shared_ptr<const LockState> state);
  // Control only: the project's key (SynthKernel::project_key), adopted at
  // the next block start. Lock-free; offering the same key again is free.
  void offer_project_key(std::optional<project::MusicalKey> key) noexcept;

  // Control only: the newest complete telemetry frame the callback has
  // published, copied into `out`. False until the first frame. Never sees a
  // frame mid-write (a lock-free triple buffer).
  [[nodiscard]] bool read_telemetry(std::vector<float>& out);

  // This instance's number in performance traces (its engine-named spans).
  [[nodiscard]] std::uint64_t trace_instance() const noexcept { return trace_instance_; }

private:
  void publish_telemetry() noexcept;
  void adopt_document() noexcept;
  NativeSynthRuntime(const SynthEngine& engine, std::unique_ptr<SynthKernel> kernel,
                     std::span<const double> values, double sample_rate);

  // Compiled documents in offer order. Control owns them and retires those
  // older than the callback's adopted revision; the callback reads the one
  // it adopted until the next adoption.
  struct DocumentOffer {
    std::uint64_t revision = 0;
    std::shared_ptr<const DocumentState> state;
  };
  struct DocumentMailbox {
    std::atomic<const DocumentOffer*> offered{nullptr};
    std::atomic<std::uint64_t> applied{0};
  };
  void adopt_lock() noexcept;
  void adopt_project_key() noexcept;
  void apply_pedal(const engine::BlockPedalEvent& event) noexcept;
  void apply_note(const engine::BlockMidiEvent& event) noexcept;
  void tick(std::uint32_t frames_to_next_tick, std::size_t block_offset, const Transport& transport,
            std::span<const ParameterLane> lanes) noexcept;

  SynthEngine engine_;
  std::unique_ptr<SynthKernel> kernel_;
  std::size_t count_;
  double sample_rate_;
  ParameterControl control_;
  std::unique_ptr<TelemetryBuffer> telemetry_ = std::make_unique<TelemetryBuffer>();
  std::unique_ptr<PedalLatch> pedals_ = std::make_unique<PedalLatch>();
  project::NativeSynthDocument document_;                 // Control only.
  SynthMedia media_;                                      // Control only.
  std::vector<std::unique_ptr<DocumentOffer>> documents_; // Control only.
  std::unique_ptr<DocumentMailbox> document_mailbox_ = std::make_unique<DocumentMailbox>();
  // Lock states in offer order, retired like documents once the callback
  // has adopted a later one.
  struct LockOffer {
    std::uint64_t serial = 0;
    std::shared_ptr<const LockState> state;
  };
  struct LockMailbox {
    std::atomic<const LockOffer*> offered{nullptr};
    std::atomic<std::uint64_t> applied{0};
  };
  std::vector<std::unique_ptr<LockOffer>> locks_; // Control only.
  std::uint64_t lock_serial_ = 0;                 // Control only.
  std::unique_ptr<LockMailbox> lock_mailbox_ = std::make_unique<LockMailbox>();
  // The offered project key, packed by pack_key (0 for none); the callback
  // tells the kernel when it differs from the one it last adopted.
  std::atomic<std::uint16_t> offered_key_{0};
  std::uint16_t adopted_key_ = 0; // Callback only.
  std::uint64_t generation_ = 0;
  std::uint64_t trace_instance_ = 0;
  bool started_ = false;
  std::atomic<std::uint32_t> observed_voices_{0};
  std::atomic<std::uint32_t> observed_steals_{0};
  std::atomic<bool> observed_deferred_{false};
};

} // namespace oscillate::native_dsp::synth

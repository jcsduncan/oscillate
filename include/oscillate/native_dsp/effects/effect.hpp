#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/synth_media.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/document_state.hpp"
#include "oscillate/native_dsp/parameter_control.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/telemetry.hpp"
#include "oscillate/native_dsp/transport.hpp"
#include "oscillate/project/model.hpp"

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The native effect platform (ADR-0040): every effect is a versioned
// parameter table plus a DSP kernel, run by one generic runtime on the
// neutral native_dsp parameter control, stepped lanes, telemetry and
// transport that synths use too.
namespace oscillate::native_dsp::effects {

// A note-on of a track keying an effect (EffectEngine::key_notes), at its
// project frame.
struct KeyNote {
  std::int64_t frame = 0;
  std::uint8_t key = 0;
  std::uint8_t velocity = 0;
  auto operator<=>(const KeyNote&) const = default;
};

// Where a kernel's telemetry is computed (ADR-0125, ADR-0129).
enum class TelemetryAnalysis : std::uint8_t {
  // On the callback, after each block (EffectKernel::telemetry).
  callback,
  // From what the callback captures, analysed off it by the thread that
  // reads the telemetry, so an analysis nobody reads costs the callback a
  // copy. A capture left unread for too long is dropped and the analysis
  // starts afresh: for displays that settle within a second or two
  // (spectra, held peaks).
  on_read,
  // As on_read, and also drained a few times a second by the platform's
  // analysis worker, so readings that accumulate (integrated loudness, the
  // highest true peak) see every frame whether or not anyone reads them.
  continuous,
};

// Engine-specific DSP. Created and destroyed on control; every other member
// runs only on the one callback that owns the runtime, except
// adopt_document's offers (see NativeEffectRuntime) and, for a kernel that
// analyses off the callback, analyse and analysed_telemetry. Such a kernel
// splits its state: what the callback writes and the analysis reads is
// shared through lock-free structures only (its CaptureQueue, and values
// as atomics); everything else belongs to one side.
class EffectKernel {
public:
  virtual ~EffectKernel() = default;
  // At each control tick: effective plain values in table order, frames to
  // the next tick, and the musical position at the tick.
  virtual void control(std::span<const double> values, std::uint32_t frames_to_next_tick,
                       const Transport& transport) noexcept = 0;
  // In place. frames never span a control tick. sidechain is empty unless
  // the engine takes one and the route supplies it.
  virtual void process(std::span<engine::StereoFrame> audio,
                       std::span<const engine::StereoFrame> sidechain) noexcept = 0;
  // Seek or loop: clear history (delay lines, filters) without a click; the
  // kernel declicks its own last output.
  virtual void reset() noexcept = 0;
  // Callback only, after each block: the engine's telemetry layout.
  [[nodiscard]] virtual std::size_t telemetry(std::span<float> out) const noexcept {
    (void)out;
    return 0;
  }
  // Fixed for the kernel's life.
  [[nodiscard]] virtual TelemetryAnalysis analysis() const noexcept {
    return TelemetryAnalysis::callback;
  }
  // A kernel that analyses off the callback: brings the analysis up to
  // what the callback captured. Runs while the callback does, on the
  // thread that reads the telemetry or on the analysis worker, never two at
  // once.
  virtual void analyse() noexcept {}
  // The reader, right after analyse(): `out` holds the newest telemetry the
  // callback published (its first `published` values; 0 before the first),
  // and the kernel completes it with the analysis's values. Returns the
  // telemetry's size, 0 while there is none.
  [[nodiscard]] virtual std::size_t analysed_telemetry(std::span<float> out,
                                                       std::size_t published) noexcept {
    (void)out;
    return published;
  }
  // Callback only, for a continuous kernel: true when its capture holds
  // more than half its room. A host that is not on a realtime thread (an
  // offline render, a test) then analyses it before going on, so nothing is
  // dropped however fast it runs.
  [[nodiscard]] virtual bool analysis_backlogged() const noexcept { return false; }
  // At a block start (and once on control before the first block): the
  // engine's compiled document, valid until the next adoption. Effects
  // without a document never see this.
  virtual void adopt_document(const DocumentState& state) noexcept { (void)state; }
  // Engines with EffectEngine::key_notes, at a block start: the note-ons
  // of the tracks keying this effect (sorted by frame, then key; unique),
  // valid until the next adoption, so the kernel sees them ahead.
  virtual void adopt_key_notes(std::span<const KeyNote> notes) noexcept { (void)notes; }
  // Before each process call: the project frame of its first frame.
  virtual void locate(std::uint64_t project_frame) noexcept { (void)project_frame; }
};

// What an engine compiles its document with: the effect's values in table
// order, its decoded managed media (EffectEngine::media) and the rate.
// `previous` is the state last compiled for the same runtime (null for the
// first) and may still be in use; an engine may share its parts.
struct DocumentInputs {
  std::span<const double> values;
  const engine::SynthMedia* media = nullptr;
  double sample_rate = 0.0;
  const DocumentState* previous = nullptr;
};

struct EffectEngine {
  const EngineDescriptor* descriptor = nullptr;
  // Control only. Allocates everything the kernel needs for these initial
  // values at this rate (they fix the latency); null when they cannot run.
  std::unique_ptr<EffectKernel> (*create_kernel)(std::span<const double> values,
                                                 double sample_rate) = nullptr;
  // Control only: frames the effect keeps sounding after its input stops.
  std::uint64_t (*tail_frames)(std::span<const double> values, double sample_rate) = nullptr;
  // Control only: constant processing delay for these values. A change of
  // latency needs a route rebuild; every other edit is live.
  std::uint32_t (*latency_frames)(std::span<const double> values, double sample_rate) = nullptr;
  // True when the effect reads a sidechain input.
  bool sidechain = false;
  // True when the effect sounds with no input (a test generator). Hosts
  // keep it processing while it is in the chain; its tail is only the fade
  // after it is switched off, and silence in is not silence out.
  bool generator = false;
  // Structured state (ADR-0058): drawn curves. Null for effects whose state
  // is only their parameter table: they accept only an empty document.
  // Control only: is this document's meaning valid for the engine?
  foundation::Result<void> (*validate_document)(const project::NativeSynthDocument& document) =
      nullptr;
  // Control only, for a validated document: compile it. The kernel reads
  // the result without allocating.
  foundation::Result<std::shared_ptr<const DocumentState>> (*prepare_document)(
      const project::NativeSynthDocument& document, const DocumentInputs& inputs) = nullptr;
  // The document a factory preset, or the defaults (nullopt), start with;
  // empty when null.
  project::NativeSynthDocument (*preset_document)(std::optional<std::size_t> preset) = nullptr;
  // True when the kernel reads its keying tracks' notes (adopt_key_notes):
  // stages compile them from the project and refresh them on every edit.
  bool key_notes = false;
  // True when the document may hold managed media slots (ADR-0115): stages
  // decode them off the callback and pass them to prepare_document. A
  // change of managed media rebuilds the stage.
  bool media = false;
  // Parameters prepare_document reads (indices in table order; never
  // automatable). A change of any of them compiles the document again on
  // control, adopted at a block start like a document edit.
  std::span<const std::size_t> document_parameters{};
  // Control only, for engines whose tail depends on their document (an
  // IR's length): frames the effect keeps sounding after its input stops
  // with this document. Null: tail_frames.
  std::uint64_t (*document_tail_frames)(const project::NativeSynthDocument& document,
                                        std::span<const double> values,
                                        double sample_rate) = nullptr;
};

[[nodiscard]] std::span<const EffectEngine* const> engines() noexcept;
[[nodiscard]] const EffectEngine* find_engine(std::string_view id) noexcept;

enum class EffectProcessStatus : std::uint8_t {
  rendered,
  invalid_block,
  invalid_sidechain,
  invalid_lanes
};

// One serialized control producer, one audio callback consumer: create on
// control, process on exactly one callback.
struct AnalysisSlot;

class NativeEffectRuntime {
  struct DocumentOffer;

public:
  NativeEffectRuntime(const NativeEffectRuntime&) = delete;
  NativeEffectRuntime& operator=(const NativeEffectRuntime&) = delete;
  NativeEffectRuntime(NativeEffectRuntime&&) = delete;
  NativeEffectRuntime& operator=(NativeEffectRuntime&&) = delete;
  // Control only, after the callback's last block.
  ~NativeEffectRuntime();

  [[nodiscard]] static foundation::Result<std::unique_ptr<NativeEffectRuntime>>
  create(const EffectEngine& engine, std::span<const double> values, double sample_rate);
  // With a document the engine validated and its decoded managed media
  // (EffectEngine::media; empty for every other engine).
  [[nodiscard]] static foundation::Result<std::unique_ptr<NativeEffectRuntime>>
  create(const EffectEngine& engine, std::span<const double> values,
         const project::NativeSynthDocument& document, double sample_rate,
         engine::SynthMedia media = {});

  // Callback only. In place. A transport-generation change resets the
  // kernel. Lanes name live, automatable parameters with one value per
  // frame. A rejected block leaves audio and state unchanged.
  [[nodiscard]] EffectProcessStatus
  process(std::span<engine::StereoFrame> audio, std::span<const engine::StereoFrame> sidechain,
          std::uint64_t project_frame, std::uint64_t transport_generation,
          const Transport& transport, std::span<const ParameterLane> lanes) noexcept;

  // Control only (ADR-0027 semantics): adopted at the next block, panel
  // jumps glide over each parameter's glide time.
  [[nodiscard]] bool offer_values(std::span<const double> values, std::uint64_t revision) noexcept;
  // Control only: a panel control mid-drag, with no saved revision. A
  // change of a document parameter compiles the document first; false when
  // that fails or the mailbox is full.
  [[nodiscard]] bool offer_live_values(std::span<const double> values) noexcept;
  [[nodiscard]] std::uint64_t applied_revision() const noexcept {
    return control_.applied_revision();
  }
  // Control only: compile a validated document and offer it, before
  // offer_values with the same revision; the callback adopts both at one
  // block start. Revisions must not decrease (a retry may repeat one).
  [[nodiscard]] foundation::Result<void>
  offer_document(const project::NativeSynthDocument& document, std::uint64_t revision);

  // One saved edit's values, and its document when `document` is set,
  // checked and compiled as offer_values and offer_document would take
  // them, with nothing offered yet.
  struct PreparedOffer {
    std::unique_ptr<DocumentOffer> document_offer;
    project::NativeSynthDocument document;
    std::vector<double> values;
    std::uint64_t revision = 0;
  };
  // Control only. Fails as those offers would (a full mailbox, a revision
  // that does not increase, a document that does not compile).
  [[nodiscard]] foundation::Result<PreparedOffer>
  prepare_offer(std::span<const double> values, const project::NativeSynthDocument* document,
                std::uint64_t revision);
  // Control only: offers a prepared edit. With no other offer to this
  // runtime since it was prepared, it is always taken, so several runtimes
  // take one edit together or none does (ADR-0058's linked effects). False
  // only when that was broken.
  [[nodiscard]] bool commit_offer(PreparedOffer&& offer) noexcept;
  // Control only: the keying tracks' note-ons, sorted and unique, adopted
  // at the next block start. False when they are the ones offered last
  // (nothing to do).
  bool offer_key_notes(std::vector<KeyNote> notes);
  // Control only: the document last offered (or created with).
  [[nodiscard]] const project::NativeSynthDocument& document() const noexcept { return document_; }
  // Control only, from one thread: the newest telemetry, false until the
  // first. For a kernel that analyses off the callback, this runs its
  // analysis (EffectKernel::analysis).
  [[nodiscard]] bool read_telemetry(std::vector<float>& out);
  [[nodiscard]] const EffectEngine& engine() const noexcept { return engine_; }
  // Fixed at creation (see EffectEngine::latency_frames).
  [[nodiscard]] std::uint32_t latency_frames() const noexcept { return latency_; }
  // This instance's number in performance traces (its engine-named spans);
  // label it with foundation::trace::label(engine().descriptor->name.data(), ...).
  [[nodiscard]] std::uint64_t trace_instance() const noexcept { return trace_instance_; }

private:
  NativeEffectRuntime(const EffectEngine& engine, std::unique_ptr<EffectKernel> kernel,
                      std::span<const double> values, double sample_rate);
  void adopt_document() noexcept;
  // Control only: the document compiled with `values` as an offer at
  // `revision` (null for an engine without one), and published once
  // compiled.
  [[nodiscard]] foundation::Result<std::unique_ptr<DocumentOffer>>
  compile_document(const project::NativeSynthDocument& document, std::span<const double> values,
                   std::uint64_t revision) const;
  // Control only: true when `values` differ from the last compiled ones in
  // a document parameter.
  [[nodiscard]] bool document_values_changed(std::span<const double> values) const noexcept;
  // Control only: compiles and publishes the current document with these
  // values when a document parameter changed (at the newest revision).
  [[nodiscard]] bool recompile_for_values(std::span<const double> values,
                                          std::uint64_t revision) noexcept;
  void publish_document(std::unique_ptr<DocumentOffer> offer) noexcept;
  void adopt_key_notes() noexcept;
  // Brings an off-callback analysis up to its capture unless the reader or
  // the worker is already doing so; never waits.
  void analyse_now() noexcept;

  // Compiled documents in offer order. Control owns them and retires those
  // published before the one the callback adopted, and any it never took;
  // the callback reads the one it adopted until the next adoption.
  struct DocumentOffer {
    std::uint64_t revision = 0;
    std::shared_ptr<const DocumentState> state;
  };
  struct DocumentMailbox {
    std::atomic<const DocumentOffer*> offered{nullptr};
    std::atomic<const DocumentOffer*> applied{nullptr};
  };

  EffectEngine engine_;
  std::unique_ptr<EffectKernel> kernel_;
  TelemetryAnalysis analysis_ = TelemetryAnalysis::callback;
  // For a kernel that analyses off the callback: the lock its reader and
  // the analysis worker share (ADR-0129).
  std::shared_ptr<AnalysisSlot> analysis_slot_;
  ParameterControl control_;
  std::unique_ptr<TelemetryBuffer> telemetry_ = std::make_unique<TelemetryBuffer>();
  project::NativeSynthDocument document_;                 // Control only.
  engine::SynthMedia media_;                              // Control only.
  std::vector<double> compiled_values_;                   // Control only.
  std::vector<std::unique_ptr<DocumentOffer>> documents_; // Control only.
  std::unique_ptr<DocumentMailbox> document_mailbox_ = std::make_unique<DocumentMailbox>();
  // Key notes in offer order (control owns them): the callback reads the
  // one it adopted until it adopts the newest.
  struct KeyNotesMailbox {
    std::atomic<const std::vector<KeyNote>*> offered{nullptr};
    std::atomic<const std::vector<KeyNote>*> applied{nullptr};
  };
  std::vector<std::unique_ptr<const std::vector<KeyNote>>> key_notes_; // Control only.
  std::unique_ptr<KeyNotesMailbox> key_notes_mailbox_ = std::make_unique<KeyNotesMailbox>();
  double sample_rate_ = 48'000.0;
  std::uint32_t latency_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t trace_instance_ = 0;
  bool started_ = false;
};

} // namespace oscillate::native_dsp::effects

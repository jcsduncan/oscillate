#pragma once

#include "oscillate/plugins/vst3_scan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oscillate::plugins {

class Vst3EditorView;

enum class Vst3HostKind : std::uint8_t { effect, instrument };
enum class Vst3ProcessingMode : std::uint8_t { realtime, offline };

// Hard ceiling for Vst3EffectConfig::max_parameter_events. A host reserves one
// queue and one editor-capture slot per admitted point at creation, so this
// bounds per-host parameter memory whatever a project requests.
inline constexpr std::uint32_t max_vst3_parameter_events = 4096;
// Capacity loaded hosts use when no automation budget is requested: the
// largest static-value set plus one manual event.
inline constexpr std::uint32_t default_vst3_parameter_events = 257;

// Per-process-call point budget for a project runtime: each automated lane
// may change on every frame, plus one slot that keeps manual-event headroom.
// Returns nullopt when the budget would exceed max_vst3_parameter_events.
[[nodiscard]] constexpr std::optional<std::uint32_t>
vst3_automation_event_budget(std::size_t lanes, std::uint32_t max_block_frames) noexcept {
  if (max_block_frames == 0 || lanes > (max_vst3_parameter_events - 1U) / max_block_frames)
    return std::nullopt;
  return static_cast<std::uint32_t>(lanes) * max_block_frames + 1U;
}

struct Vst3EffectConfig {
  Vst3Class identity;
  double sample_rate{48000.0};
  std::uint32_t max_block_frames{128};
  std::size_t max_state_bytes{4UL * 1024UL * 1024UL};
  std::uint32_t max_parameter_events{256}; // 1..max_vst3_parameter_events
  Vst3HostKind kind{Vst3HostKind::effect};
  std::uint32_t max_note_events{256};
  Vst3ProcessingMode processing_mode{Vst3ProcessingMode::realtime};
  // Effect only. Requires exactly one compatible stereo auxiliary input bus.
  // The caller must supply its aligned detector span on every process block.
  bool routed_sidechain{false};
};

struct Vst3ParameterInfo {
  std::uint32_t id{0};
  std::string title;
  std::string short_title;
  std::string units;
  std::int32_t step_count{0};
  double default_normalized{0.0};
  bool automatable{false};
  bool read_only{false};
};

struct Vst3ParameterEvent {
  std::uint32_t id{0};
  std::uint32_t sample_offset{0};
  double normalized_value{0.0};
};

enum class Vst3ParameterPrecision : std::uint8_t {
  unavailable,
  control_only,
  sample_offsets_forwarded,
};

enum class Vst3LatencyCheckStatus : std::uint8_t {
  stable,
  changed,
  restart_requested,
  unavailable,
};

struct Vst3LatencyCheck {
  Vst3LatencyCheckStatus status{Vst3LatencyCheckStatus::unavailable};
  std::uint32_t prepared_samples{0};
  std::optional<std::uint32_t> observed_samples;
};

enum class Vst3ControllerStateSync : std::uint8_t {
  not_attempted,
  synchronized,
  not_implemented,
};

// The controller chunk is independent of the processor chunk. nullopt means
// the controller did not provide getState; an engaged empty vector is a real
// zero-byte chunk and must survive project save/reopen unchanged.
struct Vst3StatePair {
  std::vector<std::uint8_t> component_state;
  std::optional<std::vector<std::uint8_t>> controller_state;
};

struct Vst3AudioBusLimits {
  std::uint32_t silent_auxiliary_inputs = 0;
  std::uint32_t discarded_secondary_outputs = 0;
};

enum class Vst3EditorPlatform : std::uint8_t { x11_xembed, native_wayland };
enum class Vst3EditorProbe : std::uint8_t {
  x11_candidate,
  no_controller,
  no_view,
  unsupported_platform,
  wayland_bridge_unavailable,
  host_unavailable,
  plugin_exception,
};

struct Vst3StereoBlock {
  std::span<float> input_left;
  std::span<float> input_right;
  std::span<float> output_left;
  std::span<float> output_right;
  // The graph owner supplies these at the block boundary. Defaults support
  // isolated host probes; project renderers must use the authoritative map.
  std::int64_t project_sample = 0;
  double quarter_position = 0.0;
  double tempo_bpm = 120.0;
  bool playing = true;
  std::span<const float> sidechain_left;
  std::span<const float> sidechain_right;
};

enum class Vst3NoteKind : std::uint8_t { on, off };
struct Vst3NoteEvent {
  Vst3NoteKind kind{Vst3NoteKind::on};
  std::uint32_t sample_offset{0};
  std::uint8_t channel{0};
  std::uint8_t pitch{60};
  float velocity{1.0F};
  std::int32_t note_id{-1};
};

struct Vst3InstrumentBlock {
  std::span<float> output_left;
  std::span<float> output_right;
};

// A block-boundary snapshot computed by the shared timeline owner. The host
// only forwards these values; it never derives tempo or musical position.
struct Vst3InstrumentTransport {
  std::int64_t project_sample{0};
  double quarter_position{0.0};
  double tempo_bpm{120.0};
  std::uint64_t generation{0};
  bool playing{true};
};

enum class Vst3ProcessStatus : std::uint8_t {
  ok,
  inactive,
  invalid_block,
  invalid_parameter_events,
  parameter_capacity_exceeded,
  parameter_unavailable,
  invalid_note_events,
  note_capacity_exceeded,
  invalid_transport,
  plugin_error,
};

// One exclusive render owner. SDK initialization, controller connection and
// teardown belong on the caller's UI thread; the host does not dispatch them.
// Activation, suspension and state operations run off the audio callback.
// process() may run on the callback after activation. Never destroy a live
// instance from the callback or concurrently with processing.
class Vst3EffectHost final {
public:
  struct Impl; // SDK types are confined to the implementation.
  explicit Vst3EffectHost(std::unique_ptr<Impl> implementation);
  ~Vst3EffectHost();
  Vst3EffectHost(const Vst3EffectHost&) = delete;
  Vst3EffectHost& operator=(const Vst3EffectHost&) = delete;
  Vst3EffectHost(Vst3EffectHost&&) = delete;
  Vst3EffectHost& operator=(Vst3EffectHost&&) = delete;

  [[nodiscard]] bool activate(std::string& error);
  void suspend() noexcept;
  // The stereo API exposes the primary bus. With routed_sidechain enabled,
  // the first stereo auxiliary input receives the supplied detector spans;
  // other auxiliary inputs receive silence. Secondary outputs are discarded.
  [[nodiscard]] Vst3ProcessStatus process(Vst3StereoBlock block,
                                          std::span<const Vst3ParameterEvent> events = {}) noexcept;
  // The caller supplies offset-sorted events; ties retain caller order. Exactly
  // one main event input bus with 1..16 channels and one stereo output are
  // required. A generation change (seek/graph replacement) or stopped block
  // flushes tracked notes before new events. Stop blocks reject new notes.
  // The caller advances the generation at each timeline discontinuity.
  // No allocation or sorting occurs on the audio callback.
  [[nodiscard]] Vst3ProcessStatus
  process_instrument(Vst3InstrumentBlock block, std::span<const Vst3NoteEvent> notes,
                     Vst3InstrumentTransport transport,
                     std::span<const Vst3ParameterEvent> parameters = {}) noexcept;
  [[nodiscard]] std::uint32_t latency_samples() const noexcept;
  // Re-query only with exclusive control of this instance, never on the audio
  // callback or concurrently with process(). A latency restart request is
  // sticky for this instance even after take_restart_requests() drains its
  // diagnostic mask. This check never updates the graph's prepared latency.
  [[nodiscard]] Vst3LatencyCheck check_prepared_latency() const noexcept;
  // Observed controller restart flags since the last drain. This is an atomic,
  // bounded bit mask, safe to drain off the callback. No requested restart is
  // performed by this host; a nonzero result closes live-edit admission until
  // the owner has rebuilt/revalidated the instance and render graph.
  [[nodiscard]] std::uint32_t take_restart_requests() noexcept;
  // Query only off the processing callback. nullopt represents the VST3
  // infinite-tail sentinel or an unavailable/failed query; export must not
  // silently truncate either case.
  [[nodiscard]] std::optional<std::uint32_t> finite_tail_samples() const noexcept;
  [[nodiscard]] const Vst3Class& identity() const noexcept;
  [[nodiscard]] const std::vector<Vst3ParameterInfo>& parameters() const noexcept;
  // Reports input-queue delivery, not the plug-in's actual interpolation precision.
  [[nodiscard]] Vst3ParameterPrecision parameter_precision(std::uint32_t id) const noexcept;
  // Control only, never concurrently with a process call (for example while
  // preparing a runtime, before its callback runs): the parameter the plugin's
  // IMidiMapping assigns to MIDI controller `controller` on event bus 0,
  // channel 0, or nullopt when it has none. A mapped parameter is then
  // accepted in process_instrument's parameter events even when it is
  // hidden or not automatable, as MIDI-learn targets often are. Pedals
  // (ADR-0035) reach instruments this way: CC64 sustain, CC66 sostenuto,
  // CC67 soft.
  [[nodiscard]] std::optional<std::uint32_t> map_midi_controller(std::int16_t controller);
  // UI/control-thread capability probe only. An X11 candidate still requires
  // a native XEmbed window, Linux run loop, attach/detach and compatibility
  // testing. Native Wayland has no host bridge yet and always falls back.
  // Do not call concurrently with lifecycle/state operations or from audio.
  [[nodiscard]] Vst3EditorProbe probe_editor(Vst3EditorPlatform platform) noexcept;
  // UI-thread lease for a single native editor. The host and its loaded module
  // must outlive the lease. Close the editor before destroying the host; no
  // lifecycle/state operation may race view creation or destruction.
  [[nodiscard]] std::unique_ptr<Vst3EditorView>
  create_editor_view(Vst3EditorProbe* status = nullptr) noexcept;
  // UI-thread edit callbacks observed while a native view is attached.
  // Bounded by max_parameter_events; overflow makes the result unavailable.
  void begin_editor_parameter_capture() noexcept;
  [[nodiscard]] std::optional<std::vector<Vst3ParameterEvent>> take_editor_parameter_edits();
  // Latest values from gestures still in progress. Draining leaves the
  // completed-gesture and final-state capture intact. UI thread only.
  [[nodiscard]] std::optional<std::vector<Vst3ParameterEvent>> take_editor_parameter_previews();
  // Completed native gestures, one final value per endEdit. Draining does not
  // stop the final-state capture. Call only on the UI thread.
  [[nodiscard]] std::optional<std::vector<Vst3ParameterEvent>> take_completed_editor_gestures();
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> save_component_state(std::string& error);
  [[nodiscard]] bool restore_component_state(std::span<const std::uint8_t> state,
                                             std::string& error);
  // UI/control thread only, with processing suspended and no editor lifecycle
  // operation in flight. Both chunks are captured from one instance. Restore
  // uses the VST3 preset order: component, controller's component view, then
  // controller's independent state. A failed restore faults this host.
  [[nodiscard]] std::optional<Vst3StatePair> save_state_pair(std::string& error);
  [[nodiscard]] bool restore_state_pair(const Vst3StatePair& state, std::string& error);
  // Component DSP state can restore when a separate controller does not
  // implement setComponentState. Native editor state then needs separate review.
  [[nodiscard]] Vst3ControllerStateSync controller_state_sync() const noexcept;
  // Counts auxiliary buses still unconnected by this host instance.
  [[nodiscard]] Vst3AudioBusLimits audio_bus_limits() const noexcept;
  [[nodiscard]] bool routed_sidechain_enabled() const noexcept;
  // Host-level contract only: the caller owns thread dispatch and must not
  // call these methods concurrently or from the audio callback. Before
  // activation, the owner applies a bounded, sorted, unique set of values to
  // a connected controller, then activates and flushes them before graph
  // publication or the first audible frame. Project binding performs these
  // steps on its preparation side before transferring the host. No audible
  // process() call succeeds while a flush is pending or after either stage
  // fails. static_parameter_values_ready() describes only static-value
  // delivery, not graph state.
  [[nodiscard]] bool stage_static_parameter_values(std::span<const Vst3ParameterEvent> values,
                                                   std::string& error);
  [[nodiscard]] bool flush_static_parameter_values(std::string& error);
  [[nodiscard]] bool static_parameter_values_ready() const noexcept;

private:
  std::unique_ptr<Impl> implementation_;
};

} // namespace oscillate::plugins

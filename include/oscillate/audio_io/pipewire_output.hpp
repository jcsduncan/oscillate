#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include <oscillate/audio_io/callback_timing.hpp>
#include <oscillate/audio_io/device_contract.hpp>
#include <oscillate/audio_io/stereo_block.hpp>
#include <oscillate/foundation/error.hpp>

namespace oscillate::audio_io {

struct OutputStatus {
  OutputState state = OutputState::stopped;
  std::uint32_t requested_client_rate = 0;
  std::optional<std::uint64_t> selected_target_serial;
  std::optional<std::uint32_t> stream_node_id;
  std::optional<std::uint32_t> negotiated_client_rate;
  // True only after a client format is observed at a rate different from the
  // project's requested rate. Graph/server-rate conversion is separate.
  bool client_rate_mismatch = false;
  std::optional<std::uint32_t> observed_graph_rate;
  std::optional<std::uint32_t> observed_block_frames;
  std::uint64_t render_failures = 0;
  std::uint64_t oversized_blocks = 0;
  std::uint64_t invalid_buffers = 0;
  // Wall time spent inside the process callback, measured only for valid
  // output blocks. These are processing-budget observations, not PipeWire
  // driver xruns or stream underflows. Empty until a block is observed.
  std::optional<std::uint64_t> measured_callback_blocks;
  std::optional<std::uint64_t> callback_over_period_blocks;
  std::optional<std::uint64_t> max_callback_duration_ns;
  std::optional<CallbackTimingDistribution> callback_timing_distribution;
  std::optional<CallbackLoad> callback_load;
  // Recovery notices addressed to this node by the PipeWire graph, sampled
  // once per process callback from SPA_IO_Position. Unknown if any process
  // callback lacked that IO area. This is not a device-wide xrun count.
  std::optional<std::uint64_t> graph_xrun_recovery_events;
  // JACK client xrun callbacks observed since this output started. These
  // notifications do not identify a particular output port or prove that
  // this DAW's rendering caused the server xrun. Unknown for PipeWire.
  std::optional<std::uint64_t> client_xrun_notifications;
  // Audio worker threads running at the callback's real-time priority
  // (ADR-0048). Zero means every block renders on the callback thread alone.
  std::size_t realtime_workers = 0;
  // Xruns the audio server reported to this client (DeviceTelemetry::server_xrun).
  // Unknown for PipeWire, whose stream API exposes no trustworthy per-stream
  // count; its graph recoveries are graph_xrun_recovery_events.
  std::optional<std::uint64_t> xruns;
  // Frames between the next frame written and the DAC, as the device driver
  // reports it. Only the direct ALSA output measures it (ADR-0130).
  std::optional<std::uint32_t> output_latency_frames;
};

// Event-loop-owned matching of a selected object serial to its current
// PipeWire registry global. A removed selection stays lost until select() is
// called for a new stream. Numeric node IDs may be reused; serials may not.
struct RegistryNodeIdentity {
  std::uint32_t global_id;
  std::uint64_t serial;
};

class SelectedTargetRegistry {
public:
  void select(std::optional<std::uint64_t> serial) noexcept;
  [[nodiscard]] bool observe_node(RegistryNodeIdentity node) noexcept;
  [[nodiscard]] bool remove_node(std::uint32_t global_id) noexcept;
  [[nodiscard]] bool present() const noexcept { return global_id_.has_value(); }
  [[nodiscard]] bool lost() const noexcept { return lost_; }

private:
  std::optional<std::uint64_t> serial_;
  std::optional<std::uint32_t> global_id_;
  bool lost_ = false;
};

// Atomic observations shared by a device backend's event and RT callback
// threads (PipeWire and JACK alike): the single source of callback timing,
// load, failure and xrun counts. The numeric rate is zero until it has
// actually been observed.
//
// Callback-side methods never allocate, lock or format text. Failures and
// xruns are also published to foundation::realtime_diagnostics() as fixed
// records, for a control thread to log (foundation::log::drain_diagnostics).
// callback_duration() feeds the trace's audio.load counter and
// audio.over_budget instant while a trace records.
class DeviceTelemetry {
public:
  using CallbackTimingSample = CallbackHeadroomTelemetry::Sample;
  static constexpr std::int64_t unknown_sample = -1;

  // Control, while the callback is stopped.
  void start(std::uint32_t requested_rate,
             std::optional<std::uint64_t> target_serial = std::nullopt) noexcept;
  void state(OutputState value) noexcept;
  void stream_node(std::uint32_t value) noexcept;
  void client_rate(std::uint32_t value) noexcept;
  void invalidate_format() noexcept;
  void invalidate_connection() noexcept;
  void graph_rate(std::uint32_t value) noexcept;
  void block_frames(std::uint32_t value) noexcept;
  // The callback output silence for a block it should have rendered.
  void render_failure(std::uint32_t frames = 0, std::int64_t sample = unknown_sample) noexcept;
  void oversized_block(std::uint32_t frames = 0) noexcept;
  void invalid_buffer() noexcept;
  void graph_xrun_recovery(std::optional<bool> flagged) noexcept;
  // Control, after start(): this backend reports xruns to its client (JACK),
  // so zero is a known count rather than unknown.
  void reports_server_xruns() noexcept;
  // An xrun the server reported to this client (a JACK xrun callback).
  void server_xrun() noexcept;
  void callback_duration(CallbackTimingSample sample) noexcept;
  [[nodiscard]] OutputStatus snapshot() const noexcept;

private:
  std::atomic<OutputState> state_{OutputState::stopped};
  std::atomic<std::uint32_t> requested_rate_{0};
  std::atomic<std::uint64_t> target_serial_{0}; // Zero means no explicit selection.
  std::atomic<std::uint64_t> stream_node_{0};   // Stored as node ID plus one.
  std::atomic<std::uint32_t> client_rate_{0};
  std::atomic<std::uint32_t> graph_rate_{0};
  std::atomic<std::uint32_t> block_frames_{0};
  std::atomic<std::uint64_t> render_failures_{0};
  std::atomic<std::uint64_t> oversized_blocks_{0};
  std::atomic<std::uint64_t> invalid_buffers_{0};
  std::atomic<std::uint64_t> graph_recovery_events_{0};
  std::atomic<std::uint64_t> graph_recovery_observations_{0};
  std::atomic<bool> graph_recovery_observation_gap_{false};
  std::atomic<std::uint64_t> measured_callback_blocks_{0};
  std::atomic<std::uint64_t> callback_over_period_blocks_{0};
  std::atomic<std::uint64_t> max_callback_duration_ns_{0};
  std::atomic<std::uint64_t> server_xruns_{0};
  std::atomic<bool> server_xruns_observed_{false};
  // CallbackLoad's fields as bits: a reader may see one a block newer than
  // the other, never a torn value.
  std::atomic<std::uint64_t> load_average_bits_{0};
  std::atomic<std::uint64_t> load_peak_bits_{0};
  // Callback-owned smoothing state, reset by start().
  double load_average_ = 0.0;
  double load_peak_ = 0.0;
  bool graph_recovering_ = false;
  CallbackHeadroomTelemetry callback_headroom_;
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

// Control-thread lifecycle. stop() requests callback silence, disconnects the
// stream, then joins PipeWire's loop before releasing its callback-owned state.
// It has no hard completion deadline: an in-flight renderer or PipeWire teardown
// may stall. The render source and transport must remain alive until it returns.
// This adapter owns no project state.
class PipeWireOutput {
public:
  PipeWireOutput(StereoRenderSource source, TransportCursor& transport,
                 TransportPermission permission = {});
  PipeWireOutput(const PipeWireOutput&) = delete;
  PipeWireOutput& operator=(const PipeWireOutput&) = delete;
  ~PipeWireOutput();

  // Requested latency is a PipeWire hint; observed block size may differ.
  // The render source and transport cursor must outlive this output.
  [[nodiscard]] foundation::Result<void> start(std::uint32_t project_rate,
                                               std::uint32_t requested_latency_frames,
                                               std::optional<std::uint64_t> target_serial = {});
  void stop() noexcept;
  [[nodiscard]] OutputStatus status() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oscillate::audio_io

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

[[nodiscard]] bool jack_server_available() noexcept;

struct JackOutputStatus {
  OutputState state = OutputState::stopped;
  std::uint32_t requested_project_rate = 0;
  std::uint32_t requested_buffer_frames = 0;
  std::optional<std::uint32_t> server_rate;
  std::optional<std::uint32_t> server_buffer_frames;
  bool rate_mismatch = false;
  bool exact_buffer_size_required = false;
  bool buffer_size_mismatch = false;
  std::optional<std::uint32_t> mismatched_buffer_frames;
  std::uint64_t render_failures = 0;
  std::uint64_t oversized_blocks = 0;
  std::uint64_t invalid_buffers = 0;
  // Audio worker threads at the client's real-time priority (ADR-0048).
  std::size_t realtime_workers = 0;
  // JACK notifications are client callbacks, not a proven per-output underrun count.
  std::uint64_t xrun_notifications = 0;
  int physical_connections = 0;
  // From the same DeviceTelemetry as PipeWire: empty until a block is measured.
  std::optional<std::uint64_t> measured_callback_blocks;
  std::optional<std::uint64_t> callback_over_period_blocks;
  std::optional<std::uint64_t> max_callback_duration_ns;
  std::optional<CallbackTimingDistribution> callback_timing_distribution;
  std::optional<CallbackLoad> callback_load;
};

enum class JackBufferPolicy : std::uint8_t { flexible, exact };

struct JackStartConfig {
  std::uint32_t project_rate;
  std::uint32_t requested_buffer_frames;
  // Exact mode treats requested_buffer_frames as the already prepared graph
  // size. Any different callback size invalidates this run until stop/start.
  JackBufferPolicy buffer_policy = JackBufferPolicy::flexible;
};

// JACK functions are injected so callback, rate-change and shutdown behaviour
// can be exercised without a running server. The table and its context must
// outlive the output. Null means the system JACK client library.
struct JackApi {
  using Process = int (*)(std::uint32_t, void*) noexcept;
  using Change = int (*)(std::uint32_t, void*) noexcept;
  using Event = int (*)(void*) noexcept;
  using Shutdown = void (*)(void*) noexcept;

  void* context = nullptr;
  void* (*open)(void*, const char*) noexcept = nullptr;
  int (*close)(void*, void*) noexcept = nullptr;
  int (*activate)(void*, void*) noexcept = nullptr;
  int (*deactivate)(void*, void*) noexcept = nullptr;
  std::uint32_t (*sample_rate)(void*, void*) noexcept = nullptr;
  std::uint32_t (*buffer_size)(void*, void*) noexcept = nullptr;
  void* (*register_output)(void*, void*, const char*) noexcept = nullptr;
  float* (*port_buffer)(void*, void*, std::uint32_t) noexcept = nullptr;
  int (*set_process)(void*, void*, Process, void*) noexcept = nullptr;
  int (*set_sample_rate)(void*, void*, Change, void*) noexcept = nullptr;
  int (*set_buffer_size)(void*, void*, Change, void*) noexcept = nullptr;
  int (*set_xrun)(void*, void*, Event, void*) noexcept = nullptr;
  void (*set_shutdown)(void*, void*, Shutdown, void*) noexcept = nullptr;
  // Control-thread convenience connection. Returns the number of physical
  // playback ports connected; zero leaves routing to the JACK patchbay.
  int (*connect_playback_ports)(void*, void*, void*, void*) noexcept = nullptr;
  // Control-thread audio workers (ADR-0048): one thread at the client's
  // real-time priority, or false when the server is not real-time. Optional.
  bool (*start_realtime_thread)(void*, void*, void* (*)(void*), void*,
                                std::uint64_t*) noexcept = nullptr;
  void (*join_thread)(void*, std::uint64_t) noexcept = nullptr;
};

class JackOutput {
public:
  JackOutput(StereoRenderSource source, TransportCursor& transport,
             TransportPermission permission = {}, const JackApi* api = nullptr);
  JackOutput(const JackOutput&) = delete;
  JackOutput& operator=(const JackOutput&) = delete;
  ~JackOutput();

  [[nodiscard]] foundation::Result<void> start(JackStartConfig config);
  void stop() noexcept;
  [[nodiscard]] JackOutputStatus status() const noexcept;

private:
  void stop_client() noexcept;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oscillate::audio_io

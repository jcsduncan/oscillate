#include <oscillate/audio_io/jack_output.hpp>

#include <oscillate/audio_io/pipewire_output.hpp>
#include <oscillate/foundation/log.hpp>
#include <oscillate/foundation/trace.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

#include <pthread.h>

#include <jack/jack.h>
#include <jack/thread.h>

namespace oscillate::audio_io {
namespace {
constexpr std::size_t max_frames = 8192;

bool supported_rate(std::uint32_t rate) noexcept {
  return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000;
}

void silence_port(float* port, std::uint32_t frames) noexcept {
  if (port != nullptr) {
    // JACK owns a frames-long buffer. Clearing all frames is required even
    // when the server requests more than our preallocated render capacity.
    std::fill_n(port, frames, 0.0F);
  }
}

void* real_open(void*, const char* name) noexcept {
  jack_status_t status = JackFailure;
  return jack_client_open(name, JackNoStartServer, &status);
}
int real_close(void*, void* client) noexcept {
  return jack_client_close(static_cast<jack_client_t*>(client));
}
int real_activate(void*, void* client) noexcept {
  return jack_activate(static_cast<jack_client_t*>(client));
}
int real_deactivate(void*, void* client) noexcept {
  return jack_deactivate(static_cast<jack_client_t*>(client));
}
std::uint32_t real_sample_rate(void*, void* client) noexcept {
  return jack_get_sample_rate(static_cast<jack_client_t*>(client));
}
std::uint32_t real_buffer_size(void*, void* client) noexcept {
  return jack_get_buffer_size(static_cast<jack_client_t*>(client));
}
void* real_register_output(void*, void* client, const char* name) noexcept {
  return jack_port_register(static_cast<jack_client_t*>(client), name, JACK_DEFAULT_AUDIO_TYPE,
                            JackPortIsOutput, 0);
}
float* real_port_buffer(void*, void* port, std::uint32_t frames) noexcept {
  return static_cast<float*>(jack_port_get_buffer(static_cast<jack_port_t*>(port), frames));
}
int real_set_process(void*, void* client, JackApi::Process callback, void* data) noexcept {
  return jack_set_process_callback(static_cast<jack_client_t*>(client), callback, data);
}
int real_set_sample_rate(void*, void* client, JackApi::Change callback, void* data) noexcept {
  return jack_set_sample_rate_callback(static_cast<jack_client_t*>(client), callback, data);
}
int real_set_buffer_size(void*, void* client, JackApi::Change callback, void* data) noexcept {
  return jack_set_buffer_size_callback(static_cast<jack_client_t*>(client), callback, data);
}
int real_set_xrun(void*, void* client, JackApi::Event callback, void* data) noexcept {
  return jack_set_xrun_callback(static_cast<jack_client_t*>(client), callback, data);
}
void real_set_shutdown(void*, void* client, JackApi::Shutdown callback, void* data) noexcept {
  jack_on_shutdown(static_cast<jack_client_t*>(client), callback, data);
}
int real_connect_playback_ports(void*, void* client, void* left, void* right) noexcept {
  auto* jack_client = static_cast<jack_client_t*>(client);
  const auto** ports = jack_get_ports(jack_client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                      JackPortIsPhysical | JackPortIsInput);
  if (ports == nullptr)
    return 0;
  const char* outputs[2] = {jack_port_name(static_cast<jack_port_t*>(left)),
                            jack_port_name(static_cast<jack_port_t*>(right))};
  int connected = 0;
  for (int channel = 0; channel < 2 && ports[channel] != nullptr; ++channel) {
    const int result = jack_connect(jack_client, outputs[channel], ports[channel]);
    if (result == 0 || result == EEXIST)
      ++connected;
  }
  jack_free(ports);
  return connected;
}

bool real_start_realtime_thread(void*, void* client, void* (*entry)(void*), void* argument,
                                std::uint64_t* handle) noexcept {
  auto* jack_client = static_cast<jack_client_t*>(client);
  if (jack_is_realtime(jack_client) == 0)
    return false;
  jack_native_thread_t thread{};
  if (jack_client_create_thread(jack_client, &thread, jack_client_real_time_priority(jack_client),
                                1, entry, argument) != 0)
    return false;
  *handle = static_cast<std::uint64_t>(thread);
  return true;
}
void real_join_thread(void*, std::uint64_t handle) noexcept {
  (void)pthread_join(static_cast<pthread_t>(handle), nullptr);
}

void* worker_entry(void* pool) noexcept {
  static_cast<engine::AudioWorkerPool*>(pool)->run_worker();
  return nullptr;
}

const JackApi real_api{nullptr,
                       real_open,
                       real_close,
                       real_activate,
                       real_deactivate,
                       real_sample_rate,
                       real_buffer_size,
                       real_register_output,
                       real_port_buffer,
                       real_set_process,
                       real_set_sample_rate,
                       real_set_buffer_size,
                       real_set_xrun,
                       real_set_shutdown,
                       real_connect_playback_ports,
                       real_start_realtime_thread,
                       real_join_thread};
} // namespace

struct JackOutput::Impl {
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

  Impl(StereoRenderSource render_source, TransportCursor& cursor,
       TransportPermission playback_permission, const JackApi& calls)
      : source(render_source), transport(cursor), permission(playback_permission), api(calls) {}

  void invalidate_block_size(std::uint32_t frames) noexcept {
    mismatched_buffer_frames.store(frames, std::memory_order_release);
    buffer_size_mismatch.store(true, std::memory_order_release);
    invalidated.store(true, std::memory_order_release);
    callback_gate.store(false, std::memory_order_release);
    state.store(OutputState::error, std::memory_order_release);
  }

  static int process(std::uint32_t frames, void* data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (frames == 0) {
      if (self.buffer_policy == JackBufferPolicy::exact)
        self.invalidate_block_size(frames);
      return 0;
    }
    const auto callback_start = std::chrono::steady_clock::now();
    // Scoped: a driver may run the callback on a thread it borrowed.
    const foundation::RealtimeThreadScope realtime;
    thread_local const bool named = (foundation::trace::name_thread("audio callback"), true);
    (void)named;
    auto* left = self.api.port_buffer(self.api.context, self.left, frames);
    auto* right = self.api.port_buffer(self.api.context, self.right, frames);
    if (left == nullptr || right == nullptr) {
      self.telemetry.invalid_buffer();
      self.invalidated.store(true, std::memory_order_release);
      self.callback_gate.store(false, std::memory_order_release);
      self.state.store(OutputState::error, std::memory_order_release);
      silence_port(left, frames);
      silence_port(right, frames);
      return 0;
    }
    if (frames > max_frames) {
      self.telemetry.oversized_block(frames);
      self.invalidated.store(true, std::memory_order_release);
      self.callback_gate.store(false, std::memory_order_release);
      self.state.store(OutputState::error, std::memory_order_release);
      silence_port(left, frames);
      silence_port(right, frames);
      return 0;
    }
    if (self.buffer_policy == JackBufferPolicy::exact && frames != self.requested_buffer_frames) {
      self.invalidate_block_size(frames);
      silence_port(left, frames);
      silence_port(right, frames);
      return 0;
    }
    const bool permitted =
        self.permission.enabled == nullptr || self.permission.enabled(self.permission.context);
    const bool ready = self.callback_gate.load(std::memory_order_acquire) &&
                       !self.invalidated.load(std::memory_order_acquire) && permitted;
    auto output = std::span<float>(self.interleaved.data(), static_cast<std::size_t>(frames) * 2);
    // When this block is heard (ADR-0086), approximately: JACK plays what
    // this cycle writes during the next one; port latency is not added.
    const auto rate = self.server_rate.load(std::memory_order_relaxed);
    BlockTiming timing;
    if (rate != 0) {
      constexpr double nanoseconds = 1e9;
      timing = {
          std::chrono::duration_cast<std::chrono::nanoseconds>(callback_start.time_since_epoch())
                  .count() +
              static_cast<std::int64_t>(frames * nanoseconds / rate),
          rate};
    }
    self.telemetry.block_frames(frames);
    const auto result = [&] {
      OSCILLATE_TRACE_SPAN("audio.render", frames);
      return render_transport_block(self.source, self.transport, ready, self.stereo_scratch, output,
                                    timing);
    }();
    if (result != BlockResult::rendered && ready) {
      self.telemetry.render_failure(
          frames, static_cast<std::int64_t>(self.transport.completed_snapshot().next_sample));
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
      left[frame] = output[frame * 2];
      right[frame] = output[frame * 2 + 1];
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - callback_start);
    // Also the trace's audio.load counter and the UI's load meter.
    self.telemetry.callback_duration({static_cast<std::uint64_t>(elapsed.count()), frames,
                                      self.server_rate.load(std::memory_order_relaxed)});
    return 0;
  }

  static int sample_rate_changed(std::uint32_t rate, void* data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    self.server_rate.store(rate, std::memory_order_release);
    if (rate != self.project_rate) {
      self.invalidated.store(true, std::memory_order_release);
      self.callback_gate.store(false, std::memory_order_release);
      self.rate_mismatch.store(true, std::memory_order_release);
      self.state.store(OutputState::error, std::memory_order_release);
    }
    return 0;
  }

  static int buffer_size_changed(std::uint32_t frames, void* data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    self.server_buffer_frames.store(frames, std::memory_order_release);
    if (self.buffer_policy == JackBufferPolicy::exact && frames != self.requested_buffer_frames) {
      self.invalidate_block_size(frames);
    } else if (frames == 0 || frames > max_frames) {
      self.invalidated.store(true, std::memory_order_release);
      self.callback_gate.store(false, std::memory_order_release);
      self.state.store(OutputState::error, std::memory_order_release);
    }
    return 0;
  }

  static int xrun(void* data) noexcept {
    static_cast<Impl*>(data)->telemetry.server_xrun();
    return 0;
  }

  static void shutdown(void* data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    self.server_shutdown.store(true, std::memory_order_release);
    self.invalidated.store(true, std::memory_order_release);
    self.callback_gate.store(false, std::memory_order_release);
    self.state.store(OutputState::disconnected, std::memory_order_release);
  }

  // Control side, after activation: workers join only at real-time priority.
  void start_workers() noexcept {
    auto* pool = source.workers;
    if (pool == nullptr || pool->wanted_workers() == 0 || api.start_realtime_thread == nullptr ||
        api.join_thread == nullptr)
      return;
    try {
      worker_threads.reserve(pool->wanted_workers());
    } catch (const std::bad_alloc&) {
      return;
    }
    for (std::size_t index = 0; index < pool->wanted_workers(); ++index) {
      std::uint64_t handle = 0;
      if (!api.start_realtime_thread(api.context, client, &worker_entry, pool, &handle))
        break;
      worker_threads.push_back(handle);
    }
    realtime_workers.store(worker_threads.size(), std::memory_order_release);
  }
  void stop_workers() noexcept {
    if (worker_threads.empty())
      return;
    source.workers->request_stop();
    for (const auto handle : worker_threads)
      api.join_thread(api.context, handle);
    worker_threads.clear();
    source.workers->reset_after_join();
    realtime_workers.store(0, std::memory_order_release);
  }

  StereoRenderSource source;
  std::vector<std::uint64_t> worker_threads; // control-owned
  std::atomic<std::size_t> realtime_workers{0};
  TransportCursor& transport;
  TransportPermission permission;
  const JackApi& api;
  std::array<engine::StereoFrame, max_frames> stereo_scratch{};
  std::array<float, max_frames * 2> interleaved{};
  void* client = nullptr;
  void* left = nullptr;
  void* right = nullptr;
  std::uint32_t project_rate = 0;
  std::uint32_t requested_buffer_frames = 0;
  JackBufferPolicy buffer_policy = JackBufferPolicy::flexible;
  bool active = false;
  int physical_connections = 0;
  std::atomic<bool> callback_gate{false};
  std::atomic<bool> invalidated{false};
  std::atomic<bool> server_shutdown{false};
  std::atomic<bool> rate_mismatch{false};
  std::atomic<bool> buffer_size_mismatch{false};
  std::atomic<std::uint32_t> mismatched_buffer_frames{0};
  std::atomic<OutputState> state{OutputState::stopped};
  std::atomic<std::uint32_t> server_rate{0};
  std::atomic<std::uint32_t> server_buffer_frames{0};
  // Failures, xruns and callback timing, as for PipeWire.
  DeviceTelemetry telemetry;
};

JackOutput::JackOutput(StereoRenderSource source, TransportCursor& transport,
                       TransportPermission permission, const JackApi* api)
    : impl_(std::make_unique<Impl>(source, transport, permission, api ? *api : real_api)) {}

JackOutput::~JackOutput() { stop(); }

bool jack_server_available() noexcept {
  jack_status_t status = JackFailure;
  auto* client = jack_client_open("Oscillate probe", JackNoStartServer, &status);
  if (client == nullptr)
    return false;
  (void)jack_client_close(client);
  return true;
}

foundation::Result<void> JackOutput::start(JackStartConfig config) {
  if (impl_->client != nullptr) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "JACK output is already started"});
  }
  if (!supported_rate(config.project_rate) || config.requested_buffer_frames == 0 ||
      config.requested_buffer_frames > max_frames || impl_->source.render == nullptr) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid JACK output configuration"});
  }
  if (config.buffer_policy != JackBufferPolicy::flexible &&
      config.buffer_policy != JackBufferPolicy::exact) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid JACK buffer policy"});
  }
  impl_->project_rate = config.project_rate;
  impl_->requested_buffer_frames = config.requested_buffer_frames;
  impl_->buffer_policy = config.buffer_policy;
  impl_->state.store(OutputState::connecting, std::memory_order_release);
  impl_->rate_mismatch.store(false, std::memory_order_release);
  impl_->buffer_size_mismatch.store(false, std::memory_order_release);
  impl_->mismatched_buffer_frames.store(0, std::memory_order_release);
  impl_->invalidated.store(false, std::memory_order_release);
  impl_->server_shutdown.store(false, std::memory_order_release);
  impl_->server_rate.store(0, std::memory_order_release);
  impl_->server_buffer_frames.store(0, std::memory_order_release);
  impl_->physical_connections = 0;
  impl_->telemetry.start(config.project_rate);
  impl_->telemetry.reports_server_xruns();
  // Resolve the clock path on the control thread before its first RT use.
  (void)std::chrono::steady_clock::now();
  impl_->client = impl_->api.open(impl_->api.context, "Oscillate");
  if (impl_->client == nullptr) {
    impl_->state.store(OutputState::error, std::memory_order_release);
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unavailable, "Cannot open JACK client"});
  }
  const auto rate = impl_->api.sample_rate(impl_->api.context, impl_->client);
  const auto buffer_frames = impl_->api.buffer_size(impl_->api.context, impl_->client);
  impl_->server_rate.store(rate, std::memory_order_release);
  impl_->server_buffer_frames.store(buffer_frames, std::memory_order_release);
  if (rate != config.project_rate) {
    impl_->rate_mismatch.store(true, std::memory_order_release);
    impl_->state.store(OutputState::error, std::memory_order_release);
    stop_client();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unsupported,
         "JACK server rate differs from project rate; restart JACK at the project rate or change "
         "the project rate"});
  }
  if (buffer_frames == 0 || buffer_frames > max_frames) {
    impl_->state.store(OutputState::error, std::memory_order_release);
    stop_client();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unsupported, "JACK server buffer size exceeds adapter limit"});
  }
  if (config.buffer_policy == JackBufferPolicy::exact &&
      buffer_frames != config.requested_buffer_frames) {
    impl_->invalidate_block_size(buffer_frames);
    stop_client();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unsupported,
         "JACK server buffer size differs from prepared graph size; reprepare after changing "
         "the server buffer size"});
  }
  auto& api = impl_->api;
  void* ctx = api.context;
  void* client = impl_->client;
  impl_->left = api.register_output(ctx, client, "left");
  impl_->right = api.register_output(ctx, client, "right");
  if (impl_->left == nullptr || impl_->right == nullptr ||
      api.set_process(ctx, client, &Impl::process, impl_.get()) != 0 ||
      api.set_sample_rate(ctx, client, &Impl::sample_rate_changed, impl_.get()) != 0 ||
      api.set_buffer_size(ctx, client, &Impl::buffer_size_changed, impl_.get()) != 0 ||
      api.set_xrun(ctx, client, &Impl::xrun, impl_.get()) != 0) {
    impl_->state.store(OutputState::error, std::memory_order_release);
    stop_client();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unavailable, "Cannot configure JACK callbacks or output ports"});
  }
  api.set_shutdown(ctx, client, &Impl::shutdown, impl_.get());
  impl_->callback_gate.store(true, std::memory_order_release);
  if (api.activate(ctx, client) != 0) {
    impl_->state.store(OutputState::error, std::memory_order_release);
    stop_client();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unavailable, "Cannot activate JACK client"});
  }
  impl_->active = true;
  auto expected = OutputState::connecting;
  const bool became_streaming = impl_->state.compare_exchange_strong(
      expected, OutputState::streaming, std::memory_order_acq_rel);
  if (!became_streaming || impl_->invalidated.load(std::memory_order_acquire)) {
    stop_client();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unavailable, "JACK format or server changed during activation"});
  }
  if (api.connect_playback_ports != nullptr)
    impl_->physical_connections =
        api.connect_playback_ports(ctx, client, impl_->left, impl_->right);
  impl_->start_workers();
  foundation::log::info(foundation::DiagnosticCategory::device,
                        "JACK output streaming: {} Hz, {} frames, {} physical ports, {} workers",
                        rate, buffer_frames, impl_->physical_connections,
                        impl_->realtime_workers.load(std::memory_order_acquire));
  return foundation::Result<void>::success();
}

void JackOutput::stop_client() noexcept {
  impl_->callback_gate.store(false, std::memory_order_release);
  if (impl_->client != nullptr) {
    if (impl_->active) {
      if (!impl_->server_shutdown.load(std::memory_order_acquire))
        (void)impl_->api.deactivate(impl_->api.context, impl_->client);
      impl_->active = false;
    }
    impl_->stop_workers();
    (void)impl_->api.close(impl_->api.context, impl_->client);
    impl_->client = nullptr;
    impl_->left = nullptr;
    impl_->right = nullptr;
  }
}

void JackOutput::stop() noexcept {
  const bool was_running = impl_->client != nullptr;
  stop_client();
  impl_->state.store(OutputState::stopped, std::memory_order_release);
  if (!was_running)
    return;
  const auto status = impl_->telemetry.snapshot();
  foundation::log::info(foundation::DiagnosticCategory::device,
                        "JACK output stopped after {} blocks: {} render failures, {} xruns, "
                        "{} over their period",
                        status.measured_callback_blocks.value_or(0), status.render_failures,
                        status.xruns.value_or(0), status.callback_over_period_blocks.value_or(0));
}

JackOutputStatus JackOutput::status() const noexcept {
  JackOutputStatus result{};
  result.state = impl_->state.load(std::memory_order_acquire);
  result.requested_project_rate = impl_->project_rate;
  result.requested_buffer_frames = impl_->requested_buffer_frames;
  const auto rate = impl_->server_rate.load(std::memory_order_acquire);
  if (rate != 0) {
    result.server_rate = rate;
  }
  const auto frames = impl_->server_buffer_frames.load(std::memory_order_acquire);
  if (frames != 0) {
    result.server_buffer_frames = frames;
  }
  result.rate_mismatch = impl_->rate_mismatch.load(std::memory_order_acquire);
  result.exact_buffer_size_required = impl_->buffer_policy == JackBufferPolicy::exact;
  result.buffer_size_mismatch = impl_->buffer_size_mismatch.load(std::memory_order_acquire);
  if (result.buffer_size_mismatch) {
    result.mismatched_buffer_frames =
        impl_->mismatched_buffer_frames.load(std::memory_order_acquire);
  }
  const auto device = impl_->telemetry.snapshot();
  result.render_failures = device.render_failures;
  result.oversized_blocks = device.oversized_blocks;
  result.realtime_workers = impl_->realtime_workers.load(std::memory_order_acquire);
  result.invalid_buffers = device.invalid_buffers;
  result.xrun_notifications = device.xruns.value_or(0);
  result.physical_connections = impl_->physical_connections;
  result.measured_callback_blocks = device.measured_callback_blocks;
  result.callback_over_period_blocks = device.callback_over_period_blocks;
  result.max_callback_duration_ns = device.max_callback_duration_ns;
  result.callback_timing_distribution = device.callback_timing_distribution;
  result.callback_load = device.callback_load;
  return result;
}

} // namespace oscillate::audio_io

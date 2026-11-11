#include <oscillate/audio_io/pipewire_output.hpp>
#include <oscillate/audio_io/render_ahead.hpp>

#include <oscillate/foundation/log.hpp>
#include <oscillate/foundation/trace.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <pipewire/pipewire.h>
#include <pipewire/thread.h>
#include <spa/node/io.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/support/thread.h>

namespace oscillate::audio_io {
namespace {
constexpr std::size_t max_frames = 8192;
constexpr std::uint32_t channels = 2;
constexpr std::size_t bytes_per_frame = sizeof(float) * channels;
// Blocks between graph-rate telemetry samples.
constexpr std::uint32_t graph_rate_probe_blocks = 32;

bool supported_rate(std::uint32_t rate) noexcept {
  return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000;
}

std::optional<std::uint64_t> parse_object_serial(const char* text) noexcept {
  if (text == nullptr) {
    return std::nullopt;
  }
  std::size_t length = 0;
  while (length <= 20 && text[length] != '\0') {
    ++length;
  }
  if (length == 0 || length > 20) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text, text + length, value);
  if (parsed.ec != std::errc{} || parsed.ptr != text + length || value == 0) {
    return std::nullopt;
  }
  return value;
}
} // namespace

struct PipeWireOutput::Impl {
  Impl(StereoRenderSource render_source, TransportCursor& render_transport,
       TransportPermission playback_permission)
      : transport(render_transport), source(render_source), permission(playback_permission) {}

  static void on_global(void* data, std::uint32_t id, std::uint32_t, const char* type,
                        std::uint32_t, const spa_dict* props) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (!self.target_serial || type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 ||
        props == nullptr) {
      return;
    }
    const auto* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (media_class == nullptr || std::strcmp(media_class, "Audio/Sink") != 0) {
      return;
    }
    const auto serial = parse_object_serial(spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL));
    if (serial && self.target_registry.observe_node({id, *serial})) {
      self.target_present.store(true, std::memory_order_release);
    }
  }

  static void on_global_remove(void* data, std::uint32_t id) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (!self.target_registry.remove_node(id)) {
      return;
    }
    self.target_present.store(false, std::memory_order_release);
    self.target_lost.store(true, std::memory_order_release);
    self.format_ok.store(false, std::memory_order_release);
    self.telemetry.invalidate_connection();
    self.telemetry.state(OutputState::disconnected);
  }

  static void on_state(void* data, pw_stream_state, pw_stream_state next, const char*) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (self.target_lost.load(std::memory_order_acquire)) {
      return;
    }
    switch (next) {
    case PW_STREAM_STATE_CONNECTING:
      self.telemetry.state(OutputState::connecting);
      break;
    case PW_STREAM_STATE_PAUSED:
      self.was_connected.store(true, std::memory_order_relaxed);
      if (self.stream != nullptr && pw_stream_get_node_id(self.stream) != PW_ID_ANY) {
        self.telemetry.stream_node(pw_stream_get_node_id(self.stream));
      }
      self.telemetry.state(self.client_rate_mismatch.load(std::memory_order_acquire)
                               ? OutputState::error
                               : OutputState::paused);
      break;
    case PW_STREAM_STATE_STREAMING:
      self.was_connected.store(true, std::memory_order_relaxed);
      if (self.stream != nullptr && pw_stream_get_node_id(self.stream) != PW_ID_ANY) {
        self.telemetry.stream_node(pw_stream_get_node_id(self.stream));
      }
      self.telemetry.state(self.client_rate_mismatch.load(std::memory_order_acquire)
                               ? OutputState::error
                               : OutputState::streaming);
      break;
    case PW_STREAM_STATE_UNCONNECTED:
      self.format_ok.store(false, std::memory_order_release);
      self.client_rate_mismatch.store(false, std::memory_order_release);
      self.telemetry.invalidate_connection();
      self.telemetry.state(self.was_connected.load(std::memory_order_relaxed)
                               ? OutputState::disconnected
                               : OutputState::connecting);
      break;
    case PW_STREAM_STATE_ERROR:
      self.format_ok.store(false, std::memory_order_release);
      self.client_rate_mismatch.store(false, std::memory_order_release);
      self.telemetry.invalidate_connection();
      self.telemetry.state(OutputState::error);
      break;
    }
  }

  static void on_format(void* data, std::uint32_t id, const spa_pod* parameter) noexcept {
    auto& self = *static_cast<Impl*>(data);
    if (id != SPA_PARAM_Format || self.target_lost.load(std::memory_order_acquire)) {
      return;
    }
    self.format_ok.store(false, std::memory_order_release);
    self.client_rate_mismatch.store(false, std::memory_order_release);
    self.telemetry.invalidate_format();
    if (parameter == nullptr) {
      return;
    }
    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(parameter, &info) < 0) {
      return;
    }
    self.telemetry.client_rate(info.rate);
    const bool rate_mismatch = info.rate != self.project_rate;
    self.client_rate_mismatch.store(rate_mismatch, std::memory_order_release);
    self.format_ok.store(info.format == SPA_AUDIO_FORMAT_F32 && info.channels == channels &&
                             !rate_mismatch,
                         std::memory_order_release);
    if (rate_mismatch)
      self.telemetry.state(OutputState::error);
  }

  static void on_io_changed(void* data, std::uint32_t id, void* area, std::uint32_t size) noexcept {
    if (id != SPA_IO_Position)
      return;
    auto& self = *static_cast<Impl*>(data);
    const auto* position = area != nullptr && size >= sizeof(spa_io_position)
                               ? static_cast<const spa_io_position*>(area)
                               : nullptr;
    self.graph_position.store(position, std::memory_order_release);
  }

  static void on_process(void* data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    const auto callback_start = std::chrono::steady_clock::now();
    // Scoped: a driver may run the callback on a thread it borrowed.
    const foundation::RealtimeThreadScope realtime;
    thread_local const bool named = (foundation::trace::name_thread("audio callback"), true);
    (void)named;
    const auto* position = self.graph_position.load(std::memory_order_acquire);
    const bool recovering =
        position != nullptr && (position->clock.flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER) != 0;
    if (recovering)
      foundation::trace::instant("audio.xrun_recovery");
    self.telemetry.graph_xrun_recovery(position == nullptr ? std::optional<bool>{}
                                                           : std::optional<bool>{recovering});
    pw_buffer* queued = pw_stream_dequeue_buffer(self.stream);
    if (queued == nullptr) {
      self.telemetry.invalid_buffer();
      return;
    }
    spa_buffer* buffer = queued->buffer;
    if (buffer == nullptr || buffer->n_datas == 0 || buffer->datas[0].chunk == nullptr ||
        buffer->datas[0].data == nullptr) {
      self.telemetry.invalid_buffer();
      if (buffer != nullptr && buffer->n_datas != 0 && buffer->datas[0].chunk != nullptr) {
        buffer->datas[0].chunk->size = 0;
      }
      (void)pw_stream_queue_buffer(self.stream, queued);
      return;
    }

    spa_data& plane = buffer->datas[0];
    spa_chunk& chunk = *plane.chunk;
    const auto capacity_frames = static_cast<std::size_t>(plane.maxsize) / bytes_per_frame;
    const auto frames =
        queued->requested != 0 ? static_cast<std::size_t>(queued->requested) : capacity_frames;
    if (frames > max_frames || frames > capacity_frames) {
      self.telemetry.oversized_block(static_cast<std::uint32_t>(frames));
      chunk.offset = 0;
      chunk.size = 0;
      chunk.stride = static_cast<std::int32_t>(bytes_per_frame);
      (void)pw_stream_queue_buffer(self.stream, queued);
      return;
    }

    self.telemetry.block_frames(static_cast<std::uint32_t>(frames));
    // When this block is heard (ADR-0086): the graph cycle's time, plus the
    // delay to the device, plus what is queued ahead of it in this stream.
    BlockTiming timing;
    pw_time time{};
    if (pw_stream_get_time_n(self.stream, &time, sizeof(time)) >= 0 && time.rate.denom != 0 &&
        time.now > 0) {
      if (++self.graph_rate_probe == graph_rate_probe_blocks) {
        self.graph_rate_probe = 0;
        if (time.rate.num == 1)
          self.telemetry.graph_rate(time.rate.denom);
      }
      constexpr double nanoseconds = 1e9;
      const double delay_ns =
          static_cast<double>(time.delay) * nanoseconds * time.rate.num / time.rate.denom;
      const double ahead_frames =
          static_cast<double>(time.queued) / bytes_per_frame + static_cast<double>(time.buffered);
      timing = {time.now + static_cast<std::int64_t>(delay_ns + ahead_frames * nanoseconds /
                                                                    self.project_rate),
                self.project_rate};
    }

    auto* samples = static_cast<float*>(plane.data);
    auto output = std::span<float>(samples, frames * channels);
    const bool permitted =
        !self.stop_gate.requested() &&
        (self.permission.enabled == nullptr || self.permission.enabled(self.permission.context));
    const bool format_ready = self.format_ok.load(std::memory_order_acquire) &&
                              self.target_present.load(std::memory_order_acquire) &&
                              !self.target_lost.load(std::memory_order_acquire);
    if (permitted && self.client_rate_mismatch.load(std::memory_order_acquire))
      self.telemetry.state(OutputState::error);
    const auto outcome = [&] {
      OSCILLATE_TRACE_SPAN("audio.render", frames);
      return render_device_block(self.source, self.transport, permitted && format_ready,
                                 self.stereo_scratch, output, timing);
    }();
    if (outcome != BlockResult::rendered && permitted && format_ready) {
      self.telemetry.render_failure(
          static_cast<std::uint32_t>(frames),
          static_cast<std::int64_t>(self.transport.completed_snapshot().next_sample));
    }
    chunk.offset = 0;
    chunk.size = static_cast<std::uint32_t>(frames * bytes_per_frame);
    chunk.stride = static_cast<std::int32_t>(bytes_per_frame);
    if (pw_stream_queue_buffer(self.stream, queued) < 0) {
      self.telemetry.invalid_buffer();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - callback_start);
    // Also the trace's audio.load counter and the UI's load meter.
    self.telemetry.callback_duration({static_cast<std::uint64_t>(elapsed.count()),
                                      static_cast<std::uint32_t>(frames), self.project_rate});
  }

  static void* worker_entry(void* pool) noexcept {
    static_cast<engine::AudioWorkerPool*>(pool)->run_worker();
    return nullptr;
  }

  // Control side, under the loop lock, after the stream connects. Workers share the callback's
  // real-time priority through PipeWire's thread utilities (module-rt: RTKit or the realtime
  // portal). A worker that cannot become real-time is not kept: the callback would otherwise wait
  // on a thread the desktop can preempt (ADR-0048).
  void start_workers() noexcept {
    auto* pool = source.workers;
    if ((pool == nullptr || pool->wanted_workers() == 0) && source.ahead == nullptr)
      return;
    auto* core = pw_stream_get_core(stream);
    auto* context = core == nullptr ? nullptr : pw_core_get_context(core);
    thread_utils = context == nullptr ? nullptr
                                      : static_cast<spa_thread_utils*>(pw_context_get_object(
                                            context, SPA_TYPE_INTERFACE_ThreadUtils));
    if (thread_utils == nullptr)
      thread_utils = pw_thread_utils_get();
    if (thread_utils == nullptr)
      return;
    start_render_ahead();
    if (pool == nullptr || pool->wanted_workers() == 0)
      return;
    try {
      worker_threads.reserve(pool->wanted_workers());
    } catch (const std::bad_alloc&) {
      return;
    }
    for (std::size_t index = 0; index < pool->wanted_workers(); ++index) {
      auto* thread = spa_thread_utils_create(thread_utils, nullptr, &Impl::worker_entry, pool);
      if (thread == nullptr)
        break;
      worker_threads.push_back(thread);
      // module-rt answers asynchronously with a positive sequence number.
      if (spa_thread_utils_acquire_rt(thread_utils, thread, -1) < 0) {
        stop_workers();
        return;
      }
    }
    realtime_workers.store(worker_threads.size(), std::memory_order_release);
  }

  // Control side: render-ahead's producer at the callback's priority
  // (ADR-0128). Without it the callback renders every block itself.
  void start_render_ahead() noexcept {
    if (source.ahead == nullptr || thread_utils == nullptr)
      return;
    auto* thread =
        spa_thread_utils_create(thread_utils, nullptr, &render_ahead_entry, source.ahead);
    if (thread == nullptr)
      return;
    render_ahead_thread = thread;
    if (spa_thread_utils_acquire_rt(thread_utils, thread, -1) < 0)
      stop_render_ahead();
  }
  void stop_render_ahead() noexcept {
    if (render_ahead_thread == nullptr)
      return;
    source.ahead->request_stop();
    (void)spa_thread_utils_join(thread_utils, render_ahead_thread, nullptr);
    render_ahead_thread = nullptr;
    source.ahead->reset_after_join();
  }

  // Control side. Callbacks may still run: the pool finishes any batch
  // inline once its workers have left.
  void stop_workers() noexcept {
    stop_render_ahead();
    if (worker_threads.empty())
      return;
    source.workers->request_stop();
    for (auto* thread : worker_threads)
      (void)spa_thread_utils_join(thread_utils, thread, nullptr);
    worker_threads.clear();
    source.workers->reset_after_join();
    realtime_workers.store(0, std::memory_order_release);
  }

  alignas(16) std::array<std::uint8_t, 1024> format_storage{};
  TransportCursor& transport;
  std::array<const spa_pod*, 1> format_parameters{};
  pw_thread_loop* loop = nullptr;
  pw_stream* stream = nullptr;
  pw_registry* registry = nullptr;
  StereoRenderSource source;
  TransportPermission permission;
  CallbackStopGate stop_gate;
  std::optional<std::uint64_t> target_serial;
  pw_registry_events registry_events{};
  SelectedTargetRegistry target_registry;
  spa_hook registry_listener{};
  DeviceTelemetry telemetry;
  pw_stream_events events{};
  std::uint32_t project_rate = 0;
  std::uint32_t graph_rate_probe = 0; // callback-owned
  std::atomic<const spa_io_position*> graph_position{nullptr};
  std::array<engine::StereoFrame, max_frames> stereo_scratch{};
  bool loop_started = false;            // control-owned
  bool registry_listener_added = false; // control/event-loop owned
  std::atomic<bool> format_ok{false};
  std::atomic<bool> client_rate_mismatch{false};
  std::atomic<bool> was_connected{false};
  std::atomic<bool> target_present{true};
  std::atomic<bool> target_lost{false};
  spa_thread_utils* thread_utils = nullptr;  // control-owned
  spa_thread* render_ahead_thread = nullptr; // control-owned
  std::vector<spa_thread*> worker_threads;   // control-owned
  std::atomic<std::size_t> realtime_workers{0};
};

PipeWireOutput::PipeWireOutput(StereoRenderSource source, TransportCursor& transport,
                               TransportPermission permission)
    : impl_(std::make_unique<Impl>(source, transport, permission)) {}
PipeWireOutput::~PipeWireOutput() { stop(); }

foundation::Result<void> PipeWireOutput::start(std::uint32_t project_rate,
                                               std::uint32_t requested_latency_frames,
                                               std::optional<std::uint64_t> target_serial) {
  if (impl_->loop != nullptr) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::conflict, "PipeWire output is already started"});
  }
  if (!supported_rate(project_rate) || (target_serial && *target_serial == 0) ||
      requested_latency_frames == 0 || requested_latency_frames > max_frames ||
      impl_->source.render == nullptr) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid PipeWire output configuration"});
  }
  static std::once_flag init_once;
  std::call_once(init_once, [] { pw_init(nullptr, nullptr); });

  impl_->project_rate = project_rate;
  impl_->stop_gate.reset_after_quiesce();
  impl_->target_serial = target_serial;
  impl_->target_registry.select(target_serial);
  impl_->target_present.store(!target_serial, std::memory_order_relaxed);
  impl_->target_lost.store(false, std::memory_order_relaxed);
  impl_->format_ok.store(false, std::memory_order_relaxed);
  impl_->client_rate_mismatch.store(false, std::memory_order_relaxed);
  impl_->was_connected.store(false, std::memory_order_relaxed);
  impl_->graph_position.store(nullptr, std::memory_order_relaxed);
  impl_->telemetry.start(project_rate, target_serial);
  // Resolve the clock path on the control thread before its first RT use.
  (void)std::chrono::steady_clock::now();
  impl_->loop = pw_thread_loop_new("Oscillate PipeWire", nullptr);
  if (impl_->loop == nullptr) {
    impl_->telemetry.state(OutputState::error);
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unavailable, "Cannot create PipeWire event loop"});
  }
  if (pw_thread_loop_start(impl_->loop) < 0) {
    stop();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unavailable, "Cannot start PipeWire event loop"});
  }
  impl_->loop_started = true;

  const std::string latency =
      std::to_string(requested_latency_frames) + "/" + std::to_string(project_rate);
  pw_thread_loop_lock(impl_->loop);
  pw_properties* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                                                "Playback", PW_KEY_MEDIA_ROLE, "Production",
                                                PW_KEY_NODE_LATENCY, latency.c_str(), nullptr);
  if (properties == nullptr) {
    pw_thread_loop_unlock(impl_->loop);
    stop();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::resource_exhausted, "Cannot allocate PipeWire properties"});
  }
  if (target_serial) {
    const std::string serial = std::to_string(*target_serial);
    if (pw_properties_set(properties, PW_KEY_TARGET_OBJECT, serial.c_str()) < 0 ||
        pw_properties_set(properties, PW_KEY_NODE_DONT_RECONNECT, "true") < 0) {
      pw_properties_free(properties);
      pw_thread_loop_unlock(impl_->loop);
      stop();
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::resource_exhausted, "Cannot select PipeWire target"});
    }
  }
  impl_->events = {};
  impl_->events.version = PW_VERSION_STREAM_EVENTS;
  impl_->events.state_changed = &Impl::on_state;
  impl_->events.io_changed = &Impl::on_io_changed;
  impl_->events.param_changed = &Impl::on_format;
  impl_->events.process = &Impl::on_process;
  impl_->stream = pw_stream_new_simple(pw_thread_loop_get_loop(impl_->loop), "Oscillate output",
                                       properties, &impl_->events, impl_.get());
  if (impl_->stream == nullptr) {
    pw_thread_loop_unlock(impl_->loop);
    stop();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unavailable, "Cannot create PipeWire stream"});
  }

  spa_pod_builder builder{};
  spa_pod_builder_init(&builder, impl_->format_storage.data(),
                       static_cast<std::uint32_t>(impl_->format_storage.size()));
  spa_audio_info_raw format{};
  format.format = SPA_AUDIO_FORMAT_F32;
  format.rate = project_rate;
  format.channels = channels;
  format.position[0] = SPA_AUDIO_CHANNEL_FL;
  format.position[1] = SPA_AUDIO_CHANNEL_FR;
  impl_->format_parameters[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
  // The audio callback must retain real-time scheduling in development too.
  // Normal scheduling missed deadlines under compiler load even when the
  // callback used less than one third of its 128-frame period. Built-in synth
  // kernels are optimized in the interactive Debug build to stay within the
  // RTKit CPU allowance.
  constexpr auto process_flag = static_cast<std::uint32_t>(PW_STREAM_FLAG_RT_PROCESS);
  // PipeWire intentionally accepts an OR of enum flags, which Clang's enum
  // analyzer treats as outside the enumerator set.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto flags = static_cast<pw_stream_flags>(
      static_cast<std::uint32_t>(PW_STREAM_FLAG_AUTOCONNECT) |
      static_cast<std::uint32_t>(PW_STREAM_FLAG_MAP_BUFFERS) | process_flag |
      (target_serial ? static_cast<std::uint32_t>(PW_STREAM_FLAG_DONT_RECONNECT) : 0U));
  const int connected = impl_->format_parameters[0] == nullptr
                            ? -1
                            : pw_stream_connect(impl_->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                                flags, impl_->format_parameters.data(), 1);
  if (connected < 0) {
    pw_thread_loop_unlock(impl_->loop);
    stop();
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::unavailable, "Cannot connect PipeWire output stream"});
  }
  // A simple stream has its core, and so its context's thread utilities,
  // only once connected.
  impl_->start_workers();
  if (target_serial) {
    auto* core = pw_stream_get_core(impl_->stream);
    impl_->registry =
        core == nullptr ? nullptr : pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (impl_->registry == nullptr) {
      pw_thread_loop_unlock(impl_->loop);
      stop();
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::unavailable, "Cannot watch selected PipeWire target"});
    }
    impl_->registry_events = {};
    impl_->registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    impl_->registry_events.global = &Impl::on_global;
    impl_->registry_events.global_remove = &Impl::on_global_remove;
    if (pw_registry_add_listener(impl_->registry, &impl_->registry_listener,
                                 &impl_->registry_events, impl_.get()) < 0) {
      pw_thread_loop_unlock(impl_->loop);
      stop();
      return foundation::Result<void>::failure(
          {foundation::ErrorCode::unavailable, "Cannot listen for selected PipeWire target"});
    }
    impl_->registry_listener_added = true;
  }
  pw_thread_loop_unlock(impl_->loop);
  foundation::log::info(foundation::DiagnosticCategory::device,
                        "PipeWire output connecting: {} Hz, {} frames requested, {} workers{}",
                        project_rate, requested_latency_frames,
                        impl_->realtime_workers.load(std::memory_order_acquire),
                        target_serial ? " to serial " + std::to_string(*target_serial) : "");
  return foundation::Result<void>::success();
}

void PipeWireOutput::stop() noexcept {
  if (impl_->loop == nullptr) {
    return;
  }
  // Suppress new render calls before waiting for PipeWire's loop lock or join.
  // A render that already passed its gate still has to finish before teardown.
  impl_->stop_gate.request();
  if (impl_->loop_started) {
    pw_thread_loop_lock(impl_->loop);
    if (impl_->registry != nullptr) {
      if (impl_->registry_listener_added) {
        spa_hook_remove(&impl_->registry_listener);
        impl_->registry_listener_added = false;
      }
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(impl_->registry));
      impl_->registry = nullptr;
    }
    if (impl_->stream != nullptr) {
      (void)pw_stream_set_active(impl_->stream, false);
      (void)pw_stream_disconnect(impl_->stream);
      // The stream owns the context whose thread utilities created them.
      impl_->stop_workers();
      pw_stream_destroy(impl_->stream);
      impl_->stream = nullptr;
    }
    pw_thread_loop_unlock(impl_->loop);
    pw_thread_loop_stop(impl_->loop);
  }
  pw_thread_loop_destroy(impl_->loop);
  impl_->loop = nullptr;
  impl_->loop_started = false;
  impl_->target_serial.reset();
  impl_->target_registry.select(std::nullopt);
  impl_->target_present.store(true, std::memory_order_relaxed);
  impl_->target_lost.store(false, std::memory_order_relaxed);
  impl_->graph_position.store(nullptr, std::memory_order_relaxed);
  impl_->telemetry.invalidate_connection();
  impl_->telemetry.state(OutputState::stopped);
  const auto status = impl_->telemetry.snapshot();
  foundation::log::info(
      foundation::DiagnosticCategory::device,
      "PipeWire output stopped after {} blocks: {} render failures, {} oversized, "
      "{} invalid buffers, {} over their period",
      status.measured_callback_blocks.value_or(0), status.render_failures, status.oversized_blocks,
      status.invalid_buffers, status.callback_over_period_blocks.value_or(0));
}

OutputStatus PipeWireOutput::status() const noexcept {
  auto result = impl_->telemetry.snapshot();
  result.realtime_workers = impl_->realtime_workers.load(std::memory_order_acquire);
  if (result.selected_target_serial) {
    if (impl_->target_lost.load(std::memory_order_acquire)) {
      result.state = OutputState::disconnected;
      result.stream_node_id.reset();
      result.negotiated_client_rate.reset();
      result.observed_graph_rate.reset();
      result.observed_block_frames.reset();
    } else if (!impl_->target_present.load(std::memory_order_acquire) &&
               (result.state == OutputState::streaming || result.state == OutputState::paused)) {
      result.state = OutputState::connecting;
    }
  }
  return result;
}

} // namespace oscillate::audio_io

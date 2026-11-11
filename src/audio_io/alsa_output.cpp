#include <oscillate/audio_io/alsa_output.hpp>

#include <oscillate/audio_io/pipewire_output.hpp>
#include <oscillate/audio_io/render_ahead.hpp>
#include <oscillate/foundation/log.hpp>
#include <oscillate/foundation/trace.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>
#include <pipewire/thread.h>
#include <spa/support/thread.h>

namespace oscillate::audio_io {
namespace {
constexpr std::size_t max_frames = 8192;
constexpr std::uint32_t stereo_channels = 2;
// A device that consumes nothing for this long has stalled.
constexpr int wait_slice_ms = 50;
constexpr std::uint32_t stall_slices = 20;
// The sound server closes the card after it gives up the reservation; the
// PCM may still be busy for a moment.
constexpr int open_attempts = 10;
constexpr auto open_retry_interval = std::chrono::milliseconds(100);
// Writes of silence before the stream is declared unrecoverable.
constexpr int prefill_attempts = 3;
constexpr double nanoseconds_per_second = 1e9;

constexpr double s32_full_scale = 2147483647.0;
constexpr double s24_full_scale = 8388607.0;
constexpr double s16_full_scale = 32767.0;
constexpr unsigned bits_per_byte = 8;
constexpr std::uint32_t byte_mask = 0xFFU;

bool supported_rate(std::uint32_t rate) noexcept {
  return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000;
}

snd_pcm_format_t to_alsa(AlsaSampleFormat format) noexcept {
  switch (format) {
  case AlsaSampleFormat::s32_le:
    return SND_PCM_FORMAT_S32_LE;
  case AlsaSampleFormat::s24_3le:
    return SND_PCM_FORMAT_S24_3LE;
  case AlsaSampleFormat::s16_le:
    return SND_PCM_FORMAT_S16_LE;
  }
  return SND_PCM_FORMAT_UNKNOWN;
}

double full_scale(AlsaSampleFormat format) noexcept {
  switch (format) {
  case AlsaSampleFormat::s32_le:
    return s32_full_scale;
  case AlsaSampleFormat::s24_3le:
    return s24_full_scale;
  case AlsaSampleFormat::s16_le:
    return s16_full_scale;
  }
  return 0.0;
}

std::string alsa_error(int error) { return snd_strerror(error); }

std::int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::optional<int> parse_index(std::string_view text) noexcept {
  int value = -1;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0)
    return std::nullopt;
  return value;
}

// Threads at the PipeWire output's real-time priority: the thread utilities
// module-rt installs in a PipeWire context (RTKit or the realtime portal),
// read from the same client configuration (ADR-0048). Without them, plain
// threads.
class RealtimeThreads {
public:
  RealtimeThreads() = default;
  RealtimeThreads(const RealtimeThreads&) = delete;
  RealtimeThreads& operator=(const RealtimeThreads&) = delete;
  ~RealtimeThreads() { close(); }

  void open() noexcept {
    static std::once_flag init_once;
    std::call_once(init_once, [] { pw_init(nullptr, nullptr); });
    loop_ = pw_loop_new(nullptr);
    if (loop_ != nullptr)
      context_ = pw_context_new(loop_, nullptr, 0);
    if (context_ != nullptr)
      utils_ = static_cast<spa_thread_utils*>(
          pw_context_get_object(context_, SPA_TYPE_INTERFACE_ThreadUtils));
    if (utils_ == nullptr)
      utils_ = pw_thread_utils_get();
  }
  [[nodiscard]] spa_thread* create(void* (*entry)(void*), void* argument) noexcept {
    return utils_ == nullptr ? nullptr : spa_thread_utils_create(utils_, nullptr, entry, argument);
  }
  [[nodiscard]] bool acquire_rt(spa_thread* thread) noexcept {
    // module-rt answers asynchronously with a positive sequence number.
    return spa_thread_utils_acquire_rt(utils_, thread, -1) >= 0;
  }
  void join(spa_thread* thread) noexcept { (void)spa_thread_utils_join(utils_, thread, nullptr); }
  void close() noexcept {
    utils_ = nullptr;
    if (context_ != nullptr)
      pw_context_destroy(context_);
    context_ = nullptr;
    if (loop_ != nullptr)
      pw_loop_destroy(loop_);
    loop_ = nullptr;
  }

private:
  pw_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  spa_thread_utils* utils_ = nullptr;
};

struct HwParamsDeleter {
  void operator()(snd_pcm_hw_params_t* params) const noexcept { snd_pcm_hw_params_free(params); }
};
struct SwParamsDeleter {
  void operator()(snd_pcm_sw_params_t* params) const noexcept { snd_pcm_sw_params_free(params); }
};
struct CtlDeleter {
  void operator()(snd_ctl_t* control) const noexcept { (void)snd_ctl_close(control); }
};
struct CardInfoDeleter {
  void operator()(snd_ctl_card_info_t* info) const noexcept { snd_ctl_card_info_free(info); }
};
struct PcmInfoDeleter {
  void operator()(snd_pcm_info_t* info) const noexcept { snd_pcm_info_free(info); }
};

void* worker_entry(void* pool) noexcept {
  static_cast<engine::AudioWorkerPool*>(pool)->run_worker();
  return nullptr;
}
} // namespace

std::size_t alsa_sample_bytes(AlsaSampleFormat format) noexcept {
  switch (format) {
  case AlsaSampleFormat::s32_le:
    return 4;
  case AlsaSampleFormat::s24_3le:
    return 3;
  case AlsaSampleFormat::s16_le:
    return 2;
  }
  return 0;
}

std::string_view alsa_format_name(AlsaSampleFormat format) noexcept {
  switch (format) {
  case AlsaSampleFormat::s32_le:
    return "S32_LE";
  case AlsaSampleFormat::s24_3le:
    return "S24_3LE";
  case AlsaSampleFormat::s16_le:
    return "S16_LE";
  }
  return "unknown";
}

void convert_stereo_to_device(std::span<const float> stereo, AlsaSampleFormat format,
                              std::uint32_t channels, std::span<std::byte> device) noexcept {
  const auto bytes = alsa_sample_bytes(format);
  const auto frames = stereo.size() / stereo_channels;
  if (channels < stereo_channels || device.size() < frames * channels * bytes)
    return;
  const double scale = full_scale(format);
  std::fill_n(device.begin(), frames * channels * bytes, std::byte{0});
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (std::uint32_t channel = 0; channel < stereo_channels; ++channel) {
      const double sample =
          std::clamp(static_cast<double>(stereo[frame * stereo_channels + channel]), -1.0, 1.0);
      // Two's complement, written little-endian byte by byte.
      const auto value =
          static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lrint(sample * scale)));
      auto* out = device.data() + (frame * channels + channel) * bytes;
      for (std::size_t index = 0; index < bytes; ++index)
        out[index] = static_cast<std::byte>((value >> (bits_per_byte * index)) & byte_mask);
    }
  }
}

std::optional<int> alsa_card_of(std::string_view pcm) {
  constexpr std::string_view hw = "hw:";
  constexpr std::string_view plughw = "plughw:";
  constexpr std::string_view card_key = "CARD=";
  std::string_view rest;
  if (pcm.starts_with(hw))
    rest = pcm.substr(hw.size());
  else if (pcm.starts_with(plughw))
    rest = pcm.substr(plughw.size());
  else
    return std::nullopt;
  if (rest.starts_with(card_key))
    rest.remove_prefix(card_key.size());
  const auto card = rest.substr(0, rest.find(','));
  if (card.empty())
    return std::nullopt;
  if (const auto index = parse_index(card))
    return index;
  const int index = snd_card_get_index(std::string(card).c_str());
  return index < 0 ? std::nullopt : std::optional<int>(index);
}

foundation::Result<std::vector<AlsaDevice>> list_alsa_playback_devices() {
  using R = foundation::Result<std::vector<AlsaDevice>>;
  std::vector<AlsaDevice> devices;
  snd_ctl_card_info_t* card_info_raw = nullptr;
  snd_pcm_info_t* pcm_info_raw = nullptr;
  if (snd_ctl_card_info_malloc(&card_info_raw) < 0 || snd_pcm_info_malloc(&pcm_info_raw) < 0) {
    snd_ctl_card_info_free(card_info_raw);
    return R::failure({foundation::ErrorCode::resource_exhausted, "Cannot list ALSA cards"});
  }
  const std::unique_ptr<snd_ctl_card_info_t, CardInfoDeleter> card_info(card_info_raw);
  const std::unique_ptr<snd_pcm_info_t, PcmInfoDeleter> pcm_info(pcm_info_raw);
  int card = -1;
  while (true) {
    if (const int error = snd_card_next(&card); error < 0)
      return R::failure(
          {foundation::ErrorCode::unavailable, "Cannot list ALSA cards: " + alsa_error(error)});
    if (card < 0)
      break;
    snd_ctl_t* control_raw = nullptr;
    if (snd_ctl_open(&control_raw, ("hw:" + std::to_string(card)).c_str(), 0) < 0)
      continue;
    const std::unique_ptr<snd_ctl_t, CtlDeleter> control(control_raw);
    if (snd_ctl_card_info(control.get(), card_info.get()) < 0)
      continue;
    const std::string card_id = snd_ctl_card_info_get_id(card_info.get());
    const std::string card_name = snd_ctl_card_info_get_name(card_info.get());
    int device = -1;
    while (snd_ctl_pcm_next_device(control.get(), &device) == 0 && device >= 0) {
      snd_pcm_info_set_device(pcm_info.get(), static_cast<unsigned>(device));
      snd_pcm_info_set_subdevice(pcm_info.get(), 0);
      snd_pcm_info_set_stream(pcm_info.get(), SND_PCM_STREAM_PLAYBACK);
      if (snd_ctl_pcm_info(control.get(), pcm_info.get()) < 0)
        continue; // capture only
      devices.push_back({"hw:CARD=" + card_id + ",DEV=" + std::to_string(device),
                         card_name + ": " + snd_pcm_info_get_name(pcm_info.get()), card, device});
    }
  }
  return R::success(std::move(devices));
}

struct AlsaOutput::Impl {
  Impl(StereoRenderSource render_source, TransportCursor& cursor,
       TransportPermission playback_permission, std::shared_ptr<DeviceReservation> held,
       const AlsaFaults* injected)
      : source(render_source), transport(cursor), permission(playback_permission),
        reservation(std::move(held)), faults(injected) {}

  // I/O thread.
  static void* io_entry(void* data) noexcept {
    static_cast<Impl*>(data)->run();
    return nullptr;
  }

  void fail(OutputState state) noexcept {
    callback_gate.store(false, std::memory_order_release);
    telemetry.state(state);
  }

  // Waits for the device for at most one slice; false after the device has
  // stalled or failed.
  [[nodiscard]] bool wait_for_room() noexcept {
    const int ready = snd_pcm_wait(pcm, wait_slice_ms);
    if (ready < 0)
      return recover(ready);
    if (ready == 0) {
      if (++idle_slices > stall_slices) {
        fail(OutputState::disconnected);
        return false;
      }
      return true;
    }
    idle_slices = 0;
    return true;
  }

  // Fills the device buffer with silence so it starts with a full buffer of
  // headroom (the start threshold is the buffer).
  [[nodiscard]] bool prefill() noexcept {
    auto remaining = static_cast<snd_pcm_uframes_t>(buffer_frames);
    int attempts = 0;
    while (remaining > 0 && !stop_requested.load(std::memory_order_acquire)) {
      const auto chunk = std::min<snd_pcm_uframes_t>(remaining, period_frames);
      const auto written = snd_pcm_writei(pcm, silence.data(), chunk);
      if (written == -EAGAIN) {
        if (!wait_for_room())
          return false;
        continue;
      }
      if (written < 0) {
        if (++attempts > prefill_attempts ||
            snd_pcm_recover(pcm, static_cast<int>(written), 1) < 0) {
          fail(OutputState::error);
          return false;
        }
        continue;
      }
      remaining -= static_cast<snd_pcm_uframes_t>(written);
    }
    return true;
  }

  // After a failed wait or write: true when the stream runs again.
  [[nodiscard]] bool recover(int error) noexcept {
    if (error == -ESTRPIPE) {
      // Suspended. snd_pcm_recover would sleep until resume; poll instead.
      const int resumed = snd_pcm_resume(pcm);
      if (resumed == -EAGAIN) {
        (void)snd_pcm_wait(pcm, wait_slice_ms);
        return true;
      }
      if (resumed < 0 && snd_pcm_prepare(pcm) < 0) {
        fail(OutputState::disconnected);
        return false;
      }
      return prefill();
    }
    if (error == -EPIPE)
      telemetry.server_xrun();
    if (snd_pcm_recover(pcm, error, 1) < 0) {
      fail(error == -ENODEV || error == -EBADFD ? OutputState::disconnected : OutputState::error);
      return false;
    }
    return error == -EPIPE ? prefill() : true;
  }

  // Renders one period, converts it and writes it.
  [[nodiscard]] bool process_period() noexcept {
    const auto start = now_ns();
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(pcm, &delay) == 0 && delay >= 0)
      delay_frames.store(static_cast<std::int64_t>(delay), std::memory_order_relaxed);
    const bool permitted = permission.enabled == nullptr || permission.enabled(permission.context);
    const bool ready = callback_gate.load(std::memory_order_acquire) && permitted;
    telemetry.block_frames(period_frames);
    {
      OSCILLATE_TRACE_SPAN("audio.render", period_frames);
      for (std::size_t offset = 0; offset < period_frames; offset += render_frames) {
        const auto frames = std::min<std::size_t>(render_frames, period_frames - offset);
        const auto heard =
            start + static_cast<std::int64_t>(
                        static_cast<double>(delay + static_cast<snd_pcm_sframes_t>(offset)) *
                        nanoseconds_per_second / rate);
        const auto result = render_device_block(
            source, transport, ready, std::span(stereo_scratch).first(frames),
            std::span(interleaved).subspan(offset * stereo_channels, frames * stereo_channels),
            {heard, rate});
        if (result != BlockResult::rendered && ready)
          telemetry.render_failure(
              static_cast<std::uint32_t>(frames),
              static_cast<std::int64_t>(transport.completed_snapshot().next_sample));
      }
    }
    convert_stereo_to_device(interleaved, format, channels, device_bytes);
    const auto elapsed = std::max<std::int64_t>(now_ns() - start, 0);
    // Also the trace's audio.load counter and the UI's load meter.
    telemetry.callback_duration({static_cast<std::uint64_t>(elapsed), period_frames, rate});
    const auto period = periods_written++;
    if (faults != nullptr && faults->before_write != nullptr) {
      if (const int injected = faults->before_write(faults->context, period); injected < 0)
        return recover(injected);
    }
    std::size_t written = 0;
    while (written < period_frames && !stop_requested.load(std::memory_order_acquire)) {
      const auto result =
          snd_pcm_writei(pcm, device_bytes.data() + written * frame_bytes, period_frames - written);
      if (result == -EAGAIN) {
        if (!wait_for_room())
          return false;
        continue;
      }
      if (result < 0)
        return recover(static_cast<int>(result)); // the rest of this period is dropped
      written += static_cast<std::size_t>(result);
    }
    return true;
  }

  void run() noexcept {
    const foundation::RealtimeThreadScope realtime_scope;
    foundation::trace::name_thread("audio callback");
    if (!prefill())
      return;
    while (!stop_requested.load(std::memory_order_acquire)) {
      const auto available = snd_pcm_avail_update(pcm);
      if (available < 0) {
        if (!recover(static_cast<int>(available)))
          return;
        continue;
      }
      if (static_cast<std::size_t>(available) < period_frames) {
        if (!wait_for_room())
          return;
        continue;
      }
      if (!process_period())
        return;
    }
  }

  [[nodiscard]] foundation::Result<void> configure(const AlsaStartConfig& config) {
    using R = foundation::Result<void>;
    snd_pcm_hw_params_t* hw_raw = nullptr;
    snd_pcm_sw_params_t* sw_raw = nullptr;
    if (snd_pcm_hw_params_malloc(&hw_raw) < 0 || snd_pcm_sw_params_malloc(&sw_raw) < 0) {
      snd_pcm_hw_params_free(hw_raw);
      return R::failure({foundation::ErrorCode::resource_exhausted, "Cannot allocate ALSA params"});
    }
    const std::unique_ptr<snd_pcm_hw_params_t, HwParamsDeleter> hw(hw_raw);
    const std::unique_ptr<snd_pcm_sw_params_t, SwParamsDeleter> sw(sw_raw);
    const auto refuse = [&config](std::string what) {
      return R::failure({foundation::ErrorCode::unsupported, config.device + " " + what});
    };
    if (snd_pcm_hw_params_any(pcm, hw.get()) < 0 ||
        snd_pcm_hw_params_set_access(pcm, hw.get(), SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
      return refuse("does not take interleaved writes");
    std::optional<AlsaSampleFormat> chosen;
    if (config.format) {
      if (snd_pcm_hw_params_test_format(pcm, hw.get(), to_alsa(*config.format)) == 0)
        chosen = config.format;
    } else {
      for (const auto candidate : alsa_preferred_formats)
        if (snd_pcm_hw_params_test_format(pcm, hw.get(), to_alsa(candidate)) == 0) {
          chosen = candidate;
          break;
        }
    }
    if (!chosen || snd_pcm_hw_params_set_format(pcm, hw.get(), to_alsa(*chosen)) < 0)
      return refuse("takes none of S32_LE, S24_3LE and S16_LE");
    unsigned min_channels = 0;
    unsigned max_channels = 0;
    (void)snd_pcm_hw_params_get_channels_min(hw.get(), &min_channels);
    (void)snd_pcm_hw_params_get_channels_max(hw.get(), &max_channels);
    const unsigned wanted_channels =
        config.channels.value_or(std::max<unsigned>(stereo_channels, min_channels));
    if (wanted_channels < stereo_channels || wanted_channels > max_channels ||
        snd_pcm_hw_params_set_channels(pcm, hw.get(), wanted_channels) < 0)
      return refuse("has no stereo playback channel layout");
    if (snd_pcm_hw_params_set_rate_resample(pcm, hw.get(), 0) < 0 ||
        snd_pcm_hw_params_set_rate(pcm, hw.get(), config.project_rate, 0) < 0)
      return refuse("does not run at " + std::to_string(config.project_rate) + " Hz");
    snd_pcm_uframes_t period = config.period_frames;
    unsigned period_count = config.periods;
    int direction = 0;
    if (snd_pcm_hw_params_set_period_size_near(pcm, hw.get(), &period, &direction) < 0 ||
        snd_pcm_hw_params_set_periods_near(pcm, hw.get(), &period_count, &direction) < 0)
      return refuse("cannot use " + std::to_string(config.periods) + " periods of " +
                    std::to_string(config.period_frames) + " frames");
    if (const int error = snd_pcm_hw_params(pcm, hw.get()); error < 0)
      return refuse("rejected its settings: " + alsa_error(error));
    snd_pcm_uframes_t buffer = 0;
    (void)snd_pcm_hw_params_get_period_size(hw.get(), &period, &direction);
    (void)snd_pcm_hw_params_get_periods(hw.get(), &period_count, &direction);
    (void)snd_pcm_hw_params_get_buffer_size(hw.get(), &buffer);
    if (period == 0 || period > max_frames || buffer < period)
      return refuse("chose a period of " + std::to_string(period) + " frames");
    if (snd_pcm_sw_params_current(pcm, sw.get()) < 0 ||
        snd_pcm_sw_params_set_start_threshold(pcm, sw.get(), buffer) < 0 ||
        snd_pcm_sw_params_set_avail_min(pcm, sw.get(), period) < 0 ||
        snd_pcm_sw_params(pcm, sw.get()) < 0)
      return refuse("rejected its start threshold");
    format = *chosen;
    channels = wanted_channels;
    rate = config.project_rate;
    period_frames = static_cast<std::uint32_t>(period);
    periods = period_count;
    buffer_frames = static_cast<std::uint32_t>(buffer);
    render_frames = std::min<std::size_t>(config.period_frames, period_frames);
    frame_bytes = channels * alsa_sample_bytes(format);
    return R::success();
  }

  // Control side, after the I/O thread starts: helpers at its priority.
  void start_helpers() noexcept {
    if (source.ahead != nullptr) {
      render_ahead_thread = threads.create(&render_ahead_entry, source.ahead);
      if (render_ahead_thread != nullptr && !threads.acquire_rt(render_ahead_thread))
        stop_render_ahead();
    }
    auto* pool = source.workers;
    if (pool == nullptr || pool->wanted_workers() == 0)
      return;
    try {
      worker_threads.reserve(pool->wanted_workers());
    } catch (const std::bad_alloc&) {
      return;
    }
    for (std::size_t index = 0; index < pool->wanted_workers(); ++index) {
      auto* thread = threads.create(&worker_entry, pool);
      if (thread == nullptr)
        break;
      worker_threads.push_back(thread);
      // A worker the desktop can preempt would stall the I/O thread.
      if (!threads.acquire_rt(thread)) {
        stop_workers();
        return;
      }
    }
    realtime_workers.store(worker_threads.size(), std::memory_order_release);
  }
  void stop_render_ahead() noexcept {
    if (render_ahead_thread == nullptr)
      return;
    source.ahead->request_stop();
    threads.join(render_ahead_thread);
    render_ahead_thread = nullptr;
    source.ahead->reset_after_join();
  }
  void stop_workers() noexcept {
    if (worker_threads.empty())
      return;
    source.workers->request_stop();
    for (auto* thread : worker_threads)
      threads.join(thread);
    worker_threads.clear();
    source.workers->reset_after_join();
    realtime_workers.store(0, std::memory_order_release);
  }

  StereoRenderSource source;
  TransportCursor& transport;
  TransportPermission permission;
  std::shared_ptr<DeviceReservation> reservation;
  const AlsaFaults* faults;
  // Control-owned while stopped; the I/O thread reads them while it runs.
  snd_pcm_t* pcm = nullptr;
  std::string device;
  AlsaSampleFormat format = AlsaSampleFormat::s32_le;
  std::uint32_t channels = stereo_channels;
  std::uint32_t rate = 0;
  std::uint32_t period_frames = 0;
  std::uint32_t periods = 0;
  std::uint32_t buffer_frames = 0;
  std::size_t render_frames = 0;
  std::size_t frame_bytes = 0;
  std::vector<engine::StereoFrame> stereo_scratch;
  std::vector<float> interleaved;
  std::vector<std::byte> device_bytes;
  std::vector<std::byte> silence;
  bool reservation_held = false;
  bool reserved = false;
  RealtimeThreads threads;
  spa_thread* io_thread = nullptr;
  spa_thread* render_ahead_thread = nullptr;
  std::vector<spa_thread*> worker_threads;
  // I/O-thread-owned.
  std::uint32_t idle_slices = 0;
  std::uint64_t periods_written = 0;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> callback_gate{false};
  std::atomic<bool> realtime{false};
  std::atomic<std::size_t> realtime_workers{0};
  std::atomic<std::int64_t> delay_frames{-1};
  DeviceTelemetry telemetry;
};

AlsaOutput::AlsaOutput(StereoRenderSource source, TransportCursor& transport,
                       TransportPermission permission,
                       std::shared_ptr<DeviceReservation> reservation, const AlsaFaults* faults)
    : impl_(std::make_unique<Impl>(source, transport, permission, std::move(reservation), faults)) {
}

AlsaOutput::~AlsaOutput() { stop(); }

foundation::Result<void> AlsaOutput::start(AlsaStartConfig config) {
  using R = foundation::Result<void>;
  auto& self = *impl_;
  if (self.pcm != nullptr)
    return R::failure({foundation::ErrorCode::conflict, "ALSA output is already started"});
  if (config.device.empty() || !supported_rate(config.project_rate) || config.period_frames == 0 ||
      config.period_frames > max_frames || config.periods < alsa_min_periods ||
      config.periods > alsa_max_periods ||
      (config.channels && *config.channels < stereo_channels) || self.source.render == nullptr)
    return R::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid ALSA output configuration"});
  self.telemetry.start(config.project_rate);
  self.telemetry.reports_server_xruns();
  self.device = config.device;
  self.delay_frames.store(-1, std::memory_order_relaxed);
  self.reserved = false;
  const auto failed = [&self](foundation::ErrorCode code, std::string message) {
    if (self.pcm != nullptr)
      (void)snd_pcm_close(self.pcm);
    self.pcm = nullptr;
    if (self.reservation_held)
      self.reservation->release();
    self.reservation_held = false;
    self.reserved = false;
    self.telemetry.state(OutputState::error);
    return R::failure({code, std::move(message)});
  };
  if (const auto card = alsa_card_of(config.device)) {
    if (!self.reservation)
      self.reservation = std::make_shared<DbusDeviceReservation>();
    auto acquired = self.reservation->acquire(*card);
    if (!acquired)
      return failed(acquired.error().code,
                    "Cannot take " + config.device + ": " + acquired.error().message);
    self.reservation_held = true;
    self.reserved = acquired.value() == DeviceReservation::Outcome::reserved;
  }
  int opened = 0;
  for (int attempt = 0; attempt < open_attempts; ++attempt) {
    opened =
        snd_pcm_open(&self.pcm, config.device.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (opened != -EBUSY)
      break;
    self.pcm = nullptr;
    std::this_thread::sleep_for(open_retry_interval);
  }
  if (opened < 0) {
    self.pcm = nullptr;
    return failed(foundation::ErrorCode::unavailable,
                  "Cannot open " + config.device + ": " + alsa_error(opened));
  }
  if (auto configured = self.configure(config); !configured)
    return failed(configured.error().code, configured.error().message);
  try {
    self.stereo_scratch.assign(self.render_frames, engine::StereoFrame{});
    self.interleaved.assign(static_cast<std::size_t>(self.period_frames) * stereo_channels, 0.0F);
    self.device_bytes.assign(self.period_frames * self.frame_bytes, std::byte{0});
    self.silence.assign(self.period_frames * self.frame_bytes, std::byte{0});
  } catch (const std::bad_alloc&) {
    return failed(foundation::ErrorCode::resource_exhausted, "Cannot allocate ALSA buffers");
  }
  self.idle_slices = 0;
  self.periods_written = 0;
  self.stop_requested.store(false, std::memory_order_release);
  self.callback_gate.store(true, std::memory_order_release);
  self.telemetry.client_rate(self.rate);
  self.telemetry.graph_rate(self.rate);
  self.telemetry.block_frames(self.period_frames);
  self.telemetry.state(OutputState::streaming);
  // Resolve the clock path on the control thread before its first RT use.
  (void)std::chrono::steady_clock::now();
  self.threads.open();
  self.io_thread = self.threads.create(&Impl::io_entry, impl_.get());
  if (self.io_thread == nullptr) {
    self.threads.close();
    return failed(foundation::ErrorCode::unavailable, "Cannot start the ALSA I/O thread");
  }
  self.realtime.store(self.threads.acquire_rt(self.io_thread), std::memory_order_release);
  self.start_helpers();
  foundation::log::info(
      foundation::DiagnosticCategory::device,
      "ALSA output streaming: {} at {} Hz, {} x {} frames, {} channels {}, {}, {}, {} workers",
      self.device, self.rate, self.periods, self.period_frames, self.channels,
      alsa_format_name(self.format), self.reserved ? "reserved" : "not reserved",
      self.realtime.load(std::memory_order_acquire) ? "real-time" : "not real-time",
      self.realtime_workers.load(std::memory_order_acquire));
  return R::success();
}

void AlsaOutput::stop() noexcept {
  auto& self = *impl_;
  if (self.pcm == nullptr)
    return;
  self.stop_requested.store(true, std::memory_order_release);
  self.callback_gate.store(false, std::memory_order_release);
  if (self.io_thread != nullptr) {
    self.threads.join(self.io_thread);
    self.io_thread = nullptr;
  }
  self.stop_render_ahead();
  self.stop_workers();
  self.threads.close();
  (void)snd_pcm_drop(self.pcm);
  (void)snd_pcm_close(self.pcm);
  self.pcm = nullptr;
  // The card goes back to the sound server only once it is closed.
  if (self.reservation_held)
    self.reservation->release();
  self.reservation_held = false;
  self.reserved = false;
  self.telemetry.state(OutputState::stopped);
  const auto status = self.telemetry.snapshot();
  foundation::log::info(foundation::DiagnosticCategory::device,
                        "ALSA output stopped after {} periods: {} render failures, {} xruns, "
                        "{} over their period",
                        status.measured_callback_blocks.value_or(0), status.render_failures,
                        status.xruns.value_or(0), status.callback_over_period_blocks.value_or(0));
}

AlsaOutputStatus AlsaOutput::status() const {
  const auto& self = *impl_;
  AlsaOutputStatus result;
  const auto device = self.telemetry.snapshot();
  result.state = device.state;
  if (self.pcm != nullptr) {
    result.device = self.device;
    result.rate = self.rate;
    result.period_frames = self.period_frames;
    result.periods = self.periods;
    result.buffer_frames = self.buffer_frames;
    result.render_frames = static_cast<std::uint32_t>(self.render_frames);
    result.channels = self.channels;
    result.format = self.format;
    result.reserved = self.reserved;
    result.realtime = self.realtime.load(std::memory_order_acquire);
    if (const auto delay = self.delay_frames.load(std::memory_order_relaxed); delay >= 0)
      result.delay_frames = static_cast<std::uint32_t>(delay);
  }
  result.realtime_workers = self.realtime_workers.load(std::memory_order_acquire);
  result.xruns = device.xruns.value_or(0);
  result.render_failures = device.render_failures;
  result.measured_callback_blocks = device.measured_callback_blocks;
  result.callback_over_period_blocks = device.callback_over_period_blocks;
  result.max_callback_duration_ns = device.max_callback_duration_ns;
  result.callback_timing_distribution = device.callback_timing_distribution;
  result.callback_load = device.callback_load;
  return result;
}

} // namespace oscillate::audio_io

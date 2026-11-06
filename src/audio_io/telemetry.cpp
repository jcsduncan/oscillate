#include <oscillate/audio_io/pipewire_output.hpp>

#include <oscillate/foundation/diagnostic.hpp>
#include <oscillate/foundation/trace.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace oscillate::audio_io {
namespace {
constexpr std::array<std::uint32_t, CallbackTimingDistribution::bin_count - 1>
    timing_bin_upper_percent{25, 50, 75, 90, 100, 125, 150};

std::size_t timing_bin(std::uint64_t elapsed_ns, std::uint32_t frames,
                       std::uint32_t rate) noexcept {
  for (std::size_t index = 0; index < timing_bin_upper_percent.size(); ++index) {
    const auto percent = timing_bin_upper_percent[index];
    // frames * 10^7 * 150 fits uint64 even for UINT32_MAX frames.
    const auto bound = static_cast<std::uint64_t>(frames) * 10'000'000ULL * percent / rate;
    if (elapsed_ns <= bound) {
      return index;
    }
  }
  return timing_bin_upper_percent.size();
}

std::optional<std::uint32_t> observed(std::uint32_t value) noexcept {
  if (value == 0) {
    return std::nullopt;
  }
  return value;
}

constexpr double nanoseconds_per_second = 1e9;
constexpr std::uint64_t nanoseconds_per_microsecond = 1000;

void publish(foundation::DiagnosticKind kind, foundation::ErrorCode code, std::uint32_t detail = 0,
             std::int64_t sample = DeviceTelemetry::unknown_sample) noexcept {
  // A full ring counts the record as dropped; the callback never waits.
  (void)foundation::realtime_diagnostics().try_publish(
      {.subject = {},
       .correlation = 0,
       .sample_position = sample,
       .detail = detail,
       .code = code,
       .category = foundation::DiagnosticCategory::device,
       .kind = kind});
}
} // namespace

std::optional<CallbackTimingWindow>
summarize_callback_window(const CallbackTimingDistribution& before,
                          const CallbackTimingDistribution& after,
                          std::uint64_t minimum_blocks) noexcept {
  if (before.epoch != after.epoch || before.rate == 0 || before.rate != after.rate ||
      before.frames == 0 || before.frames != after.frames || minimum_blocks == 0)
    return std::nullopt;
  CallbackTimingWindow result;
  for (std::size_t index = 0; index < before.bins.size(); ++index) {
    if (after.bins[index] < before.bins[index])
      return std::nullopt;
    const auto count = after.bins[index] - before.bins[index];
    if (count > std::numeric_limits<std::uint64_t>::max() - result.blocks)
      return std::nullopt;
    result.blocks += count;
    if (count != 0)
      result.slowest_bin_upper_percent =
          index < timing_bin_upper_percent.size() ? timing_bin_upper_percent[index] : 151;
  }
  if (result.blocks < minimum_blocks)
    return std::nullopt;
  const auto period_us = static_cast<std::uint64_t>(after.frames) * 1'000'000ULL / after.rate;
  const auto free_percent =
      result.slowest_bin_upper_percent < 100 ? 100 - result.slowest_bin_upper_percent : 0;
  result.observed_free_us_lower_bound = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      std::numeric_limits<std::uint32_t>::max(), period_us * free_percent / 100));
  return result;
}

void CallbackHeadroomTelemetry::reset() noexcept {
  sequence_.fetch_add(1, std::memory_order_seq_cst);
  epoch_.fetch_add(1, std::memory_order_relaxed);
  rate_.store(0, std::memory_order_relaxed);
  frames_.store(0, std::memory_order_relaxed);
  for (auto& bin : bins_) {
    bin.store(0, std::memory_order_relaxed);
  }
  sequence_.fetch_add(1, std::memory_order_seq_cst);
}

void CallbackHeadroomTelemetry::record(Sample sample) noexcept {
  if (sample.frames == 0 || sample.rate == 0) {
    return;
  }
  sequence_.fetch_add(1, std::memory_order_seq_cst);
  if (rate_.load(std::memory_order_relaxed) != sample.rate ||
      frames_.load(std::memory_order_relaxed) != sample.frames) {
    epoch_.fetch_add(1, std::memory_order_relaxed);
    rate_.store(sample.rate, std::memory_order_relaxed);
    frames_.store(sample.frames, std::memory_order_relaxed);
    for (auto& bin : bins_) {
      bin.store(0, std::memory_order_relaxed);
    }
  }
  bins_[timing_bin(sample.elapsed_ns, sample.frames, sample.rate)].fetch_add(
      1, std::memory_order_relaxed);
  sequence_.fetch_add(1, std::memory_order_seq_cst);
}

std::optional<CallbackTimingDistribution> CallbackHeadroomTelemetry::snapshot() const noexcept {
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    const auto before = sequence_.load(std::memory_order_seq_cst);
    if ((before & 1U) != 0) {
      continue;
    }
    CallbackTimingDistribution result;
    result.epoch = epoch_.load(std::memory_order_relaxed);
    result.rate = rate_.load(std::memory_order_relaxed);
    result.frames = frames_.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < result.bins.size(); ++index) {
      result.bins[index] = bins_[index].load(std::memory_order_relaxed);
    }
    if (before == sequence_.load(std::memory_order_seq_cst)) {
      if (result.rate == 0 || result.frames == 0) {
        return std::nullopt;
      }
      return result;
    }
  }
  return std::nullopt;
}

void SelectedTargetRegistry::select(std::optional<std::uint64_t> serial) noexcept {
  serial_ = serial;
  global_id_.reset();
  lost_ = false;
}

bool SelectedTargetRegistry::observe_node(RegistryNodeIdentity node) noexcept {
  if (!serial_ || node.serial != *serial_ || lost_) {
    return false;
  }
  global_id_ = node.global_id;
  return true;
}

bool SelectedTargetRegistry::remove_node(std::uint32_t global_id) noexcept {
  if (!global_id_ || *global_id_ != global_id || lost_) {
    return false;
  }
  global_id_.reset();
  lost_ = true;
  return true;
}

void DeviceTelemetry::start(std::uint32_t requested_rate,
                            std::optional<std::uint64_t> target_serial) noexcept {
  callback_headroom_.reset();
  requested_rate_.store(requested_rate, std::memory_order_relaxed);
  target_serial_.store(target_serial.value_or(0), std::memory_order_relaxed);
  stream_node_.store(0, std::memory_order_relaxed);
  client_rate_.store(0, std::memory_order_relaxed);
  graph_rate_.store(0, std::memory_order_relaxed);
  block_frames_.store(0, std::memory_order_relaxed);
  render_failures_.store(0, std::memory_order_relaxed);
  oversized_blocks_.store(0, std::memory_order_relaxed);
  invalid_buffers_.store(0, std::memory_order_relaxed);
  graph_recovery_events_.store(0, std::memory_order_relaxed);
  graph_recovery_observations_.store(0, std::memory_order_relaxed);
  graph_recovery_observation_gap_.store(false, std::memory_order_relaxed);
  measured_callback_blocks_.store(0, std::memory_order_relaxed);
  callback_over_period_blocks_.store(0, std::memory_order_relaxed);
  max_callback_duration_ns_.store(0, std::memory_order_relaxed);
  server_xruns_.store(0, std::memory_order_relaxed);
  server_xruns_observed_.store(false, std::memory_order_relaxed);
  load_average_bits_.store(0, std::memory_order_relaxed);
  load_peak_bits_.store(0, std::memory_order_relaxed);
  load_average_ = 0.0;
  load_peak_ = 0.0;
  graph_recovering_ = false;
  state_.store(OutputState::connecting, std::memory_order_release);
}
void DeviceTelemetry::state(OutputState value) noexcept {
  state_.store(value, std::memory_order_release);
}
void DeviceTelemetry::stream_node(std::uint32_t value) noexcept {
  stream_node_.store(static_cast<std::uint64_t>(value) + 1, std::memory_order_relaxed);
}
void DeviceTelemetry::client_rate(std::uint32_t value) noexcept {
  client_rate_.store(value, std::memory_order_relaxed);
}
void DeviceTelemetry::invalidate_format() noexcept {
  client_rate_.store(0, std::memory_order_relaxed);
}
void DeviceTelemetry::invalidate_connection() noexcept {
  invalidate_format();
  stream_node_.store(0, std::memory_order_relaxed);
  graph_rate_.store(0, std::memory_order_relaxed);
  block_frames_.store(0, std::memory_order_relaxed);
}
void DeviceTelemetry::graph_rate(std::uint32_t value) noexcept {
  graph_rate_.store(value, std::memory_order_relaxed);
}
void DeviceTelemetry::block_frames(std::uint32_t value) noexcept {
  block_frames_.store(value, std::memory_order_relaxed);
}
void DeviceTelemetry::render_failure(std::uint32_t frames, std::int64_t sample) noexcept {
  render_failures_.fetch_add(1, std::memory_order_relaxed);
  publish(foundation::DiagnosticKind::block_silenced, foundation::ErrorCode::internal, frames,
          sample);
}
void DeviceTelemetry::oversized_block(std::uint32_t frames) noexcept {
  oversized_blocks_.fetch_add(1, std::memory_order_relaxed);
  publish(foundation::DiagnosticKind::oversized_block, foundation::ErrorCode::unsupported, frames);
}
void DeviceTelemetry::invalid_buffer() noexcept {
  invalid_buffers_.fetch_add(1, std::memory_order_relaxed);
  publish(foundation::DiagnosticKind::invalid_buffer, foundation::ErrorCode::unavailable);
}
void DeviceTelemetry::graph_xrun_recovery(std::optional<bool> flagged) noexcept {
  if (!flagged) {
    graph_recovery_observation_gap_.store(true, std::memory_order_relaxed);
    return;
  }
  if (*flagged) {
    graph_recovery_events_.fetch_add(1, std::memory_order_relaxed);
    // One record per recovery episode, not per flagged cycle.
    if (!graph_recovering_)
      publish(foundation::DiagnosticKind::xrun, foundation::ErrorCode::unavailable);
  }
  graph_recovering_ = *flagged;
  graph_recovery_observations_.fetch_add(1, std::memory_order_release);
}
void DeviceTelemetry::reports_server_xruns() noexcept {
  server_xruns_observed_.store(true, std::memory_order_release);
}
void DeviceTelemetry::server_xrun() noexcept {
  server_xruns_.fetch_add(1, std::memory_order_relaxed);
  server_xruns_observed_.store(true, std::memory_order_release);
  publish(foundation::DiagnosticKind::xrun, foundation::ErrorCode::unavailable);
  foundation::trace::instant("audio.xrun");
}
void DeviceTelemetry::callback_duration(CallbackTimingSample sample) noexcept {
  if (sample.frames == 0 || sample.rate == 0) {
    return;
  }
  callback_headroom_.record(sample);
  const auto period_ns = static_cast<std::uint64_t>(sample.frames) * 1'000'000'000ULL / sample.rate;
  if (sample.elapsed_ns > period_ns) {
    callback_over_period_blocks_.fetch_add(1, std::memory_order_relaxed);
  }
  if (sample.elapsed_ns > max_callback_duration_ns_.load(std::memory_order_relaxed)) {
    // The process callback is the sole writer. Readers only load this value.
    max_callback_duration_ns_.store(sample.elapsed_ns, std::memory_order_relaxed);
  }
  const double block_seconds = static_cast<double>(sample.frames) / sample.rate;
  const double load =
      static_cast<double>(sample.elapsed_ns) / nanoseconds_per_second / block_seconds;
  const bool primed = measured_callback_blocks_.load(std::memory_order_relaxed) != 0;
  if (!primed) {
    load_average_ = load;
    load_peak_ = load;
  } else {
    load_average_ +=
        (1.0 - std::exp(-block_seconds / CallbackLoad::average_seconds)) * (load - load_average_);
    load_peak_ =
        std::max(load, load_peak_ * std::exp(-block_seconds / CallbackLoad::peak_release_seconds));
  }
  load_average_bits_.store(std::bit_cast<std::uint64_t>(load_average_), std::memory_order_relaxed);
  load_peak_bits_.store(std::bit_cast<std::uint64_t>(load_peak_), std::memory_order_relaxed);
  measured_callback_blocks_.fetch_add(1, std::memory_order_release);
  if (foundation::trace::enabled()) {
    foundation::trace::counter("audio.load", load);
    if (sample.elapsed_ns > period_ns)
      foundation::trace::instant("audio.over_budget",
                                 sample.elapsed_ns / nanoseconds_per_microsecond);
  }
}
OutputStatus DeviceTelemetry::snapshot() const noexcept {
  OutputStatus result;
  result.state = state_.load(std::memory_order_acquire);
  result.requested_client_rate = requested_rate_.load(std::memory_order_relaxed);
  const auto target = target_serial_.load(std::memory_order_relaxed);
  if (target != 0) {
    result.selected_target_serial = target;
  }
  const auto node = stream_node_.load(std::memory_order_relaxed);
  if (node != 0) {
    result.stream_node_id = static_cast<std::uint32_t>(node - 1);
  }
  result.negotiated_client_rate = observed(client_rate_.load(std::memory_order_relaxed));
  result.client_rate_mismatch = result.negotiated_client_rate &&
                                *result.negotiated_client_rate != result.requested_client_rate;
  result.observed_graph_rate = observed(graph_rate_.load(std::memory_order_relaxed));
  result.observed_block_frames = observed(block_frames_.load(std::memory_order_relaxed));
  result.render_failures = render_failures_.load(std::memory_order_relaxed);
  result.oversized_blocks = oversized_blocks_.load(std::memory_order_relaxed);
  result.invalid_buffers = invalid_buffers_.load(std::memory_order_relaxed);
  if (graph_recovery_observations_.load(std::memory_order_acquire) != 0 &&
      !graph_recovery_observation_gap_.load(std::memory_order_relaxed)) {
    result.graph_xrun_recovery_events = graph_recovery_events_.load(std::memory_order_relaxed);
  }
  const auto measured = measured_callback_blocks_.load(std::memory_order_acquire);
  if (measured != 0) {
    result.measured_callback_blocks = measured;
    result.callback_over_period_blocks =
        callback_over_period_blocks_.load(std::memory_order_relaxed);
    result.max_callback_duration_ns = max_callback_duration_ns_.load(std::memory_order_relaxed);
    result.callback_timing_distribution = callback_headroom_.snapshot();
    result.callback_load = CallbackLoad{
        .average = std::bit_cast<double>(load_average_bits_.load(std::memory_order_relaxed)),
        .peak = std::bit_cast<double>(load_peak_bits_.load(std::memory_order_relaxed))};
  }
  // The server's xrun notifications are this client's notifications: one
  // count, exposed under both names so neither field is left unknown.
  if (server_xruns_observed_.load(std::memory_order_acquire)) {
    result.xruns = server_xruns_.load(std::memory_order_relaxed);
    result.client_xrun_notifications = result.xruns;
  }
  return result;
}
} // namespace oscillate::audio_io

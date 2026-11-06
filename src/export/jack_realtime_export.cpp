#include <oscillate/export/jack_realtime_export.hpp>

#include <algorithm>
#include <chrono>
#include <limits>

namespace oscillate::exporting {
namespace {
using foundation::ErrorCode;
using foundation::Result;

std::optional<std::uint64_t> delta(std::optional<std::uint64_t> before,
                                   std::optional<std::uint64_t> after) noexcept {
  if (!before || !after || *after < *before)
    return std::nullopt;
  return *after - *before;
}
} // namespace

JackRealtimeWavExport::JackRealtimeWavExport(
    std::unique_ptr<RealtimeWavCapture> capture, audio_io::StereoRenderSource final_master,
    audio_io::TransportCursor& transport, HealthProbe health, HealthCounters baseline,
    std::uint64_t capture_start, std::uint64_t expected_frames, const audio_io::JackApi* api)
    : capture_(std::move(capture)), final_master_(final_master), health_(health),
      baseline_(baseline), capture_start_(capture_start), expected_frames_(expected_frames),
      output_(std::make_unique<audio_io::JackOutput>(
          audio_io::StereoRenderSource{this, &JackRealtimeWavExport::render,
                                       &JackRealtimeWavExport::freeze_cursor, final_master.workers},
          transport, audio_io::TransportPermission{this, &JackRealtimeWavExport::permitted}, api)) {
}

JackRealtimeWavExport::~JackRealtimeWavExport() { stop_and_join(); }

Result<std::unique_ptr<JackRealtimeWavExport>> JackRealtimeWavExport::begin_with_tail_plan(
    const std::filesystem::path& target, WavSpec spec, TailPreflight preflight,
    std::uint64_t first_sample, std::uint32_t output_latency_frames, std::size_t ring_frames,
    audio_io::StereoRenderSource final_master, audio_io::TransportCursor& transport,
    audio_io::JackStartConfig device, HealthProbe health, const audio_io::JackApi* api) {
  auto tail = plan_realtime_tail_frames(preflight.route, preflight.master, preflight.node_tails,
                                        preflight.max_tail_frames);
  if (!tail)
    return Result<std::unique_ptr<JackRealtimeWavExport>>::failure(tail.error());
  if (preflight.content_frames == 0 ||
      preflight.content_frames > std::numeric_limits<std::uint64_t>::max() - tail.value() ||
      spec.expected_frames != preflight.content_frames + tail.value())
    return Result<std::unique_ptr<JackRealtimeWavExport>>::failure(
        {ErrorCode::invalid_argument, "Real-time capture interval omits prepared effect tails"});
  return begin(target, spec, first_sample, output_latency_frames, ring_frames, final_master,
               transport, device, health, api);
}

Result<std::unique_ptr<JackRealtimeWavExport>>
JackRealtimeWavExport::begin(const std::filesystem::path& target, WavSpec spec,
                             std::uint64_t first_sample, std::uint32_t output_latency_frames,
                             std::size_t ring_frames, audio_io::StereoRenderSource final_master,
                             audio_io::TransportCursor& transport, audio_io::JackStartConfig device,
                             HealthProbe health, const audio_io::JackApi* api) {
  const auto baseline =
      health.snapshot != nullptr ? health.snapshot(health.context) : HealthCounters{};
  if (final_master.render == nullptr || spec.sample_rate != device.project_rate ||
      transport.completed_snapshot().next_sample != first_sample || !baseline.source_underruns ||
      first_sample >= std::numeric_limits<std::uint64_t>::max() - output_latency_frames ||
      spec.expected_frames >
          std::numeric_limits<std::uint64_t>::max() - first_sample - output_latency_frames) {
    return Result<std::unique_ptr<JackRealtimeWavExport>>::failure(
        {ErrorCode::invalid_argument,
         "Real-time export needs a positioned master and known health counters"});
  }
  const auto capture_start = first_sample + output_latency_frames;
  auto capture = RealtimeWavCapture::begin(target, spec, capture_start, ring_frames);
  if (!capture)
    return Result<std::unique_ptr<JackRealtimeWavExport>>::failure(capture.error());
  auto session = std::unique_ptr<JackRealtimeWavExport>(
      new JackRealtimeWavExport(std::move(capture.value()), final_master, transport, health,
                                baseline, capture_start, spec.expected_frames, api));
  session->drain_worker_ =
      std::jthread([self = session.get()](std::stop_token token) { self->drain_loop(token); });
  auto started = session->output_->start(device);
  if (!started)
    return Result<std::unique_ptr<JackRealtimeWavExport>>::failure(started.error());
  return Result<std::unique_ptr<JackRealtimeWavExport>>::success(std::move(session));
}

bool JackRealtimeWavExport::render(void* context, std::span<engine::StereoFrame> frames,
                                   engine::BlockContext block) noexcept {
  auto& self = *static_cast<JackRealtimeWavExport*>(context);
  self.pending_frames_ = frames;
  self.pending_block_ = block;
  self.pending_rendered_ = self.final_master_.render(self.final_master_.context, frames, block);
  // The device period may straddle the captured end. Render its normal block
  // for processor continuity, but deliver silence beyond the exact endpoint.
  if (self.pending_rendered_) {
    const auto end = self.capture_start_ + self.expected_frames_;
    const auto audible = block.project_sample_position < end
                             ? static_cast<std::size_t>(std::min<std::uint64_t>(
                                   frames.size(), end - block.project_sample_position))
                             : 0;
    std::fill(frames.begin() + static_cast<std::ptrdiff_t>(audible), frames.end(),
              engine::StereoFrame{});
  }
  return self.pending_rendered_;
}

bool JackRealtimeWavExport::freeze_cursor(void* context) noexcept {
  auto& self = *static_cast<JackRealtimeWavExport*>(context);
  const bool frozen = self.final_master_.freeze_cursor != nullptr &&
                      self.final_master_.freeze_cursor(self.final_master_.context);
  // render_transport_block invokes this once for every rendered part, including
  // a failed render. A held source has not delivered its final master frames.
  // Frames before the capture start are the master's output latency; a failed
  // render there still fails the capture.
  if (!frozen) {
    auto frames = self.pending_frames_;
    auto block = self.pending_block_;
    if (block.project_sample_position < self.capture_start_ && self.pending_rendered_) {
      const auto skipped = static_cast<std::size_t>(std::min<std::uint64_t>(
          frames.size(), self.capture_start_ - block.project_sample_position));
      frames = frames.subspan(skipped);
      block.project_sample_position += skipped;
    }
    if (!frames.empty() || !self.pending_rendered_)
      self.capture_->capture(frames, block, self.pending_rendered_);
  }
  self.pending_frames_ = {};
  return frozen;
}

bool JackRealtimeWavExport::permitted(const void* context) noexcept {
  const auto& self = *static_cast<const JackRealtimeWavExport*>(context);
  return !self.capture_failed() && !self.interval_captured();
}

bool JackRealtimeWavExport::capture_failed() const noexcept {
  return drain_failed_.load(std::memory_order_acquire) || capture_->failed();
}

void JackRealtimeWavExport::drain_loop(std::stop_token token) noexcept {
  while (!token.stop_requested()) {
    if (!capture_->drain()) {
      drain_failed_.store(true, std::memory_order_release);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!capture_->drain())
    drain_failed_.store(true, std::memory_order_release);
}

void JackRealtimeWavExport::stop_and_join() noexcept {
  if (output_)
    output_->stop();
  if (drain_worker_.joinable()) {
    drain_worker_.request_stop();
    drain_worker_.join();
  }
}

bool JackRealtimeWavExport::interval_captured() const noexcept {
  return capture_->frames_captured() == expected_frames_;
}

std::uint64_t JackRealtimeWavExport::frames_captured() const noexcept {
  return capture_->frames_captured();
}

audio_io::JackOutputStatus JackRealtimeWavExport::device_status() const noexcept {
  return output_->status();
}

Result<PublishOutcome> JackRealtimeWavExport::finish() {
  if (finished_)
    return Result<PublishOutcome>::failure({ErrorCode::conflict, "Export already finished"});
  finished_ = true;
  const auto before_stop = output_->status();
  stop_and_join();
  const auto after_stop = output_->status();
  const auto final_counters = health_.snapshot(health_.context);
  const auto source_delta = delta(baseline_.source_underruns, final_counters.source_underruns);
  const auto& timing = after_stop.callback_timing_distribution;
  // A new JackOutput has one reset epoch and one first-format epoch. A later
  // process format change advances it again, even if the format changes back.
  // Never infer zero overruns from an absent, empty or mixed-format histogram.
  constexpr std::uint64_t one_format_epoch = 2;
  bool observed_blocks = false;
  bool timing_overflow = false;
  std::uint64_t over_period_blocks = 0;
  if (timing) {
    for (std::size_t bin = 0; bin < timing->bins.size(); ++bin) {
      observed_blocks |= timing->bins[bin] != 0;
      if (bin > 4) {
        if (timing->bins[bin] > std::numeric_limits<std::uint64_t>::max() - over_period_blocks)
          timing_overflow = true;
        else
          over_period_blocks += timing->bins[bin];
      }
    }
  }
  if (drain_failed_.load(std::memory_order_acquire) || !source_delta ||
      before_stop.state != audio_io::OutputState::streaming || after_stop.rate_mismatch ||
      after_stop.invalid_buffers != 0 || after_stop.oversized_blocks != 0 ||
      after_stop.xrun_notifications < before_stop.xrun_notifications || !timing ||
      !observed_blocks || timing_overflow || timing->epoch != one_format_epoch ||
      timing->rate != after_stop.requested_project_rate || !after_stop.server_rate ||
      timing->rate != *after_stop.server_rate || !after_stop.server_buffer_frames ||
      timing->frames != *after_stop.server_buffer_frames) {
    return Result<PublishOutcome>::failure(
        {ErrorCode::unavailable, "Real-time export device or health evidence is incomplete"});
  }
  RealtimeWavCapture::DeviceEvidence evidence{.xruns_before = 0,
                                              .xruns_after = after_stop.xrun_notifications,
                                              .render_failures = after_stop.render_failures,
                                              .source_underruns = *source_delta,
                                              .callback_over_period_blocks = over_period_blocks};
  return capture_->finish(evidence);
}

} // namespace oscillate::exporting

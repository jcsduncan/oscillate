#include "realtime_export_run.hpp"

#include "oscillate/native_inserts/native_stage.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace oscillate::quick {
namespace {
using foundation::ErrorCode;
using foundation::Result;
constexpr std::size_t capture_ring_frames = 1U << 17U;

// Audible history after the input falls silent. A compressor or limiter has
// only gain state; the limiter's lookahead is part of the prepared output
// latency, which the capture skips separately.
Result<std::uint64_t> insert_tail_frames(const project::EffectInsert& insert,
                                         const project::Snapshot& snapshot) {
  if (!insert.enabled)
    return Result<std::uint64_t>::success(0);
  if (!native_inserts::is_native(insert.processor))
    return Result<std::uint64_t>::failure(
        {ErrorCode::unsupported,
         "JACK real-time export cannot bound a VST3 effect tail; use offline export"});
  if (std::holds_alternative<project::NativeConvolution>(insert.processor))
    return Result<std::uint64_t>::failure(
        {ErrorCode::unsupported, "JACK real-time export cannot bound a convolution tail"});
  native_inserts::StageContext context{snapshot.sample_rate};
  context.snapshot = &snapshot;
  auto prepared = native_inserts::prepare_native_stage(insert, context);
  if (!prepared)
    return Result<std::uint64_t>::failure(prepared.error());
  if (!prepared.value().export_tail_frames)
    return Result<std::uint64_t>::failure(*prepared.value().tail_error);
  return Result<std::uint64_t>::success(*prepared.value().export_tail_frames);
}
} // namespace

Result<std::uint64_t> RealtimeExportRun::bounded_tail_frames(const project::Snapshot& snapshot) {
  const auto limit = static_cast<std::uint64_t>(snapshot.sample_rate) * max_tail_seconds;
  std::uint64_t total = 0;
  const auto add = [&](const project::EffectInsert& insert) -> Result<void> {
    auto tail = insert_tail_frames(insert, snapshot);
    if (!tail)
      return Result<void>::failure(tail.error());
    if (tail.value() > limit - total)
      return Result<void>::failure(
          {ErrorCode::resource_exhausted,
           "JACK real-time export effect tails exceed the ten-second limit"});
    total += tail.value();
    return Result<void>::success();
  };
  for (const auto& track : snapshot.tracks) {
    // The saved diagnostic cannot certify the exact prepared source and its
    // cutoff. begin() checks that runtime and its route receipt instead.
    if (std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source))
      return Result<std::uint64_t>::failure(
          {ErrorCode::unsupported,
           "JACK real-time export cannot bound a VST3 instrument tail; use offline export"});
    if (track.kind == project::TrackKind::instrument && !track.midi_clips.empty())
      return Result<std::uint64_t>::failure(
          {ErrorCode::unsupported,
           "JACK real-time export cannot bound instrument note releases; use offline export"});
    for (const auto& insert : track.effects)
      if (auto added = add(insert); !added)
        return Result<std::uint64_t>::failure(added.error());
  }
  for (const auto& insert : snapshot.master_effects)
    if (auto added = add(insert); !added)
      return Result<std::uint64_t>::failure(added.error());
  return Result<std::uint64_t>::success(total);
}

RealtimeExportRun::RealtimeExportRun(std::unique_ptr<RoutedPlayback> playback) noexcept
    : playback_(std::move(playback)) {}

RealtimeExportRun::~RealtimeExportRun() {
  capture_.reset(); // JACK callback and drain worker join before graph/source retirement.
  if (playback_)
    (void)playback_->stop_and_retire();
}

Result<std::unique_ptr<RealtimeExportRun>>
RealtimeExportRun::begin(const std::filesystem::path& root, project::Snapshot snapshot,
                         application::ControlTransportGate::PlaybackToken token,
                         const std::filesystem::path& target, std::uint64_t end_sample,
                         std::uint32_t buffer_frames, Vst3LiveQualification qualification,
                         const audio_io::JackApi* api) {
  using Failure = Result<std::unique_ptr<RealtimeExportRun>>;
  if (end_sample == 0 || end_sample == std::numeric_limits<std::uint64_t>::max()) {
    token.finish_after_join();
    return Failure::failure(
        {ErrorCode::invalid_argument, "Real-time export needs a positive bounded bar interval"});
  }
  const auto vst3_sources = std::ranges::count_if(snapshot.tracks, [](const auto& track) {
    return std::holds_alternative<project::Vst3PluginInstance>(track.instrument_source);
  });
  if (vst3_sources > 2) {
    token.finish_after_join();
    return Failure::failure({ErrorCode::unsupported,
                             "JACK real-time export supports at most two VST3 instrument sources"});
  }
  const auto revision = snapshot.revision;
  auto prepared = RoutedPlayback::prepare_for_export(root, std::move(snapshot), std::move(token),
                                                     buffer_frames, std::move(qualification));
  if (!prepared)
    return Failure::failure(prepared.error());
  auto run = std::unique_ptr<RealtimeExportRun>(new RealtimeExportRun(std::move(prepared).value()));
  const auto& control = *run->playback_->control_;
  auto cut = run->playback_->source_->prepare_realtime_export_end(end_sample);
  if (!cut)
    return Failure::failure(cut.error());
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  if (run->playback_->instruments_) {
    for (const auto& track : run->playback_->model_->snapshot()->tracks) {
      const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source);
      if (!source)
        continue;
      if (!control.has_bound_instrument_track(track.id))
        return Failure::failure(
            {ErrorCode::unavailable, "JACK export VST3 source lacks its exact bound track"});
      auto instrument_cut =
          run->playback_->instruments_->prepare_export_end(track.id, source->id, end_sample);
      if (!instrument_cut)
        return Failure::failure(instrument_cut.error());
      if (instrument_cut.value() != end_sample ||
          run->playback_->instruments_->prepared_export_end(track.id, source->id) != end_sample)
        return Failure::failure(
            {ErrorCode::conflict, "JACK export VST3 source cutoff differs from the selected end"});
    }
  }
#endif
  auto receipt = control.prepared_route_tail_receipt(revision, end_sample);
  if (!receipt)
    return Failure::failure(receipt.error());
  std::vector<exporting::RealtimeNodeTail> node_tails;
  node_tails.reserve(receipt.value().nodes.size());
  for (const auto& node : receipt.value().nodes)
    node_tails.push_back({node.node, node.frames});
  const auto limit = static_cast<std::uint64_t>(run->playback_->sample_rate()) * max_tail_seconds;
  auto tail = exporting::plan_realtime_tail_frames(receipt.value().route, receipt.value().master,
                                                   node_tails, limit);
  if (!tail)
    return Failure::failure(tail.error());
  if (tail.value() > std::numeric_limits<std::uint64_t>::max() - end_sample)
    return Failure::failure(
        {ErrorCode::invalid_argument, "Real-time export interval and tail exceed frame range"});
  run->content_end_ = end_sample;
  run->tail_frames_ = tail.value();
  run->expected_frames_ = end_sample + tail.value();
  run->latency_frames_ = control.master_output_latency_frames();
  run->silence_.assign(control.max_frames(), engine::StereoFrame{});
  run->spans_.resize(control.topological_order().size());
  for (const auto& source : control.source_layout()) {
    const auto& tracks = run->playback_->model_->snapshot()->tracks;
    const auto track = std::ranges::find(tracks, source.track, &project::Track::id);
    if (track == tracks.end())
      return Failure::failure({ErrorCode::conflict, "Prepared source track is missing"});
    run->cuts_.push_back(
        {source.topological_index,
         std::holds_alternative<project::NativeInstrumentPatch>(track->instrument_source),
         std::vector<engine::StereoFrame>(control.max_frames())});
    run->cuts_.back().retain_instrument_release |=
        std::holds_alternative<project::Vst3PluginInstance>(track->instrument_source);
  }
  auto capture = exporting::JackRealtimeWavExport::begin_with_tail_plan(
      target, {run->playback_->sample_rate(), exporting::WavFormat::float32, run->expected_frames_},
      {receipt.value().route, receipt.value().master, node_tails, end_sample, limit}, 0,
      run->latency_frames_, capture_ring_frames,
      {run.get(), &RealtimeExportRun::render, &RealtimeExportRun::freeze_cursor,
       run->playback_->workers_.get()},
      run->playback_->cursor_, {run->playback_->sample_rate(), buffer_frames},
      {run.get(), &RealtimeExportRun::health}, api);
  if (!capture)
    return Failure::failure(capture.error());
  run->capture_ = std::move(capture).value();
  return Result<std::unique_ptr<RealtimeExportRun>>::success(std::move(run));
}

bool RealtimeExportRun::render(void* context, std::span<engine::StereoFrame> output,
                               engine::BlockContext block) noexcept {
  auto& run = *static_cast<RealtimeExportRun*>(context);
  auto& playback = *run.playback_;
  if (!playback.control_ || !playback.source_selection_)
    return false;
  auto lease = playback.source_selection_->enter_callback();
  struct FillContext {
    RealtimeExportRun& run;
    application::RoutedSourceSelection::CallbackLease& lease;
    application::RoutedAudioSource* used = nullptr;
  } fill_context{run, lease};
  const std::span<const std::span<const engine::StereoFrame>> unused{};
  const auto result = playback.control_->render_block(
      unused, output, block,
      {&fill_context, nullptr,
       [](void* context, std::uint64_t revision, engine::BlockContext fill_block,
          std::size_t frames,
          std::span<const std::span<const engine::StereoFrame>>& selected) noexcept {
         auto& state = *static_cast<FillContext*>(context);
         auto* source = static_cast<application::RoutedAudioSource*>(
             state.lease.source_for_graph_revision(revision));
         if (source == nullptr)
           return false;
         state.used = source;
         return source->fill_source_bank(fill_block, frames, selected,
                                         state.run.playback_->workers_.get()) &&
                state.run.cut_sources(fill_block, frames, selected);
       }},
      playback.workers_.get());
  run.callback_frozen_ = result != engine::RoutedRenderStatus::rendered;
  if (result == engine::RoutedRenderStatus::rendered && fill_context.used != nullptr)
    fill_context.used->note_rendered_frames(output.size());
  return result == engine::RoutedRenderStatus::rendered ||
         (result == engine::RoutedRenderStatus::invalid_block && fill_context.used != nullptr &&
          fill_context.used->streaming_hold());
}

bool RealtimeExportRun::freeze_cursor(void* context) noexcept {
  return static_cast<RealtimeExportRun*>(context)->callback_frozen_;
}

bool RealtimeExportRun::cut_sources(
    engine::BlockContext block, std::size_t frames,
    std::span<const std::span<const engine::StereoFrame>>& selected) noexcept {
  if (block.project_sample_position < content_end_ &&
      frames <= content_end_ - block.project_sample_position)
    return true;
  if (selected.size() > spans_.size() || frames > silence_.size())
    return false;
  const auto kept = block.project_sample_position < content_end_
                        ? static_cast<std::size_t>(content_end_ - block.project_sample_position)
                        : 0;
  std::ranges::copy(selected, spans_.begin());
  for (auto& cut : cuts_) {
    if (cut.retain_instrument_release || cut.index >= selected.size() ||
        selected[cut.index].empty())
      continue;
    const auto length = selected[cut.index].size();
    if (kept == 0) {
      spans_[cut.index] = std::span<const engine::StereoFrame>(silence_).first(length);
      continue;
    }
    auto frames_out = std::span(cut.frames).first(length);
    const auto audible = std::min(kept, length);
    std::copy_n(selected[cut.index].begin(), audible, frames_out.begin());
    std::fill(frames_out.begin() + static_cast<std::ptrdiff_t>(audible), frames_out.end(),
              engine::StereoFrame{});
    spans_[cut.index] = frames_out;
  }
  selected = std::span<const std::span<const engine::StereoFrame>>(spans_).first(selected.size());
  return true;
}

exporting::JackRealtimeWavExport::HealthCounters RealtimeExportRun::health(void* context) noexcept {
  auto& run = *static_cast<RealtimeExportRun*>(context);
  if (run.playback_->processor_fault() || run.playback_->source_failure())
    return {};
  const auto source = run.playback_->source_status();
  if (source.worker_failed)
    return {};
  return {source.missing_frames};
}

bool RealtimeExportRun::interval_captured() const noexcept {
  return capture_ && capture_->interval_captured();
}

std::uint64_t RealtimeExportRun::frames_captured() const noexcept {
  return capture_ ? capture_->frames_captured() : 0;
}

audio_io::JackOutputStatus RealtimeExportRun::device_status() const noexcept {
  return capture_ ? capture_->device_status() : audio_io::JackOutputStatus{};
}

Result<exporting::PublishOutcome> RealtimeExportRun::finish() {
  if (!capture_)
    return Result<exporting::PublishOutcome>::failure(
        {ErrorCode::conflict, "Real-time export is not active"});
  return capture_->finish();
}

Result<void> RealtimeExportRun::wait_until_ready(const std::atomic<bool>& cancelled,
                                                 std::stop_token stop) {
  const auto rate = static_cast<double>(playback_->sample_rate());
  const auto max_seconds = static_cast<double>(expected_frames_ + latency_frames_) / rate + 10.0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::duration<double>(max_seconds);
  while (!interval_captured() && !stop.stop_requested() &&
         !cancelled.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    const auto state = device_status().state;
    if (state == audio_io::OutputState::disconnected || state == audio_io::OutputState::error ||
        state == audio_io::OutputState::stopped || (capture_ && capture_->capture_failed()))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (stop.stop_requested() || cancelled.load(std::memory_order_acquire))
    return Result<void>::failure({ErrorCode::cancelled, "JACK real-time export cancelled"});
  if (!interval_captured())
    return Result<void>::failure(
        {ErrorCode::unavailable, "JACK real-time export ended before its selected tail end"});
  return Result<void>::success();
}

Result<exporting::PublishOutcome>
RealtimeExportRun::finish_if_current(std::uint64_t expected_revision,
                                     std::uint64_t current_revision, bool cancelled) {
  if (cancelled)
    return Result<exporting::PublishOutcome>::failure(
        {ErrorCode::cancelled, "JACK real-time export cancelled"});
  if (current_revision != expected_revision)
    return Result<exporting::PublishOutcome>::failure(
        {ErrorCode::conflict, "Project changed during JACK real-time export"});
  return finish();
}

} // namespace oscillate::quick

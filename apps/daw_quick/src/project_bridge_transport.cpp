#include "audio_import_controller.hpp"
#include "clip_warp_controller.hpp"
#include "loop_transport.hpp"
#include "native_synth_automation_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "oscillate/foundation/trace.hpp"
#include "oscillate/processing/warp_render_cache.hpp"
#include "oscillate/project/frozen_view.hpp"
#include "project_bridge.hpp"
#include "recording_controller.hpp"
#include "saved_processing.hpp"
#include "waveform_controller.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace oscillate::quick {
namespace {
// Saved warp, pitch or stretch on any audio clip: only routed playback
// renders it, so it must never fall back to the direct InstrumentPlayback path.
bool has_processed_audio(const project::Snapshot& snapshot) {
  return std::ranges::any_of(snapshot.tracks, [&snapshot](const project::Track& track) {
    return std::ranges::any_of(track.clips, [&snapshot](const project::Clip& clip) {
      const auto request = processing::processed_clip_request(snapshot, clip.id);
      return !request || request.value().has_value();
    });
  });
}

bool has_rate_converted_audio(const project::Snapshot& snapshot) {
  return std::ranges::any_of(snapshot.tracks, [&snapshot](const project::Track& track) {
    return std::ranges::any_of(track.clips, [&snapshot](const project::Clip& clip) {
      const auto source =
          std::ranges::find_if(snapshot.sources, [&clip](const project::SourceRecord& record) {
            return record.id == clip.source;
          });
      return source != snapshot.sources.end() && source->sample_rate != snapshot.sample_rate;
    });
  });
}

QString underrun_detail(const media::UnderrunEvent& event) {
  return QStringLiteral("; clip %1 missing source frames %2–%3 (generation %4)")
      .arg(QString::fromStdString(event.clip.to_string()))
      .arg(event.first_source_frame)
      .arg(event.first_source_frame + event.missing_frames)
      .arg(event.generation);
}

} // namespace

std::uint64_t ProjectBridge::bar_one_sample() const {
  if (!session_)
    return 0;
  const auto& snapshot = session_->snapshot();
  return transport_controller_.sample_at_bar(&snapshot, static_cast<double>(snapshot.lead_bars))
      .value_or(0);
}

std::uint64_t ProjectBridge::stop_sample() const {
  if (loop_transport_)
    if (const auto loop = loop_transport_->start_sample())
      return *loop;
  return bar_one_sample();
}

bool ProjectBridge::playback_busy() const {
  return audio_edit_completion_ || playback_completion_ || routed_completion_ || routed_playback_ ||
         routed_stop_completion_ || routed_transport_completion_ || managed_media_busy() ||
         (playback_ && (playback_->playing() || playback_->retiring()));
}

TransportController::PlaybackView ProjectBridge::playback_view(PlaybackViewOptions options) const {
  TransportController::PlaybackView view;
  view.project_open = session_ != nullptr;
  view.model_revision = session_ ? session_->snapshot().revision : 0;
  view.audible_revision = session_ ? session_->snapshot().audible_revision : 0;
  view.active = playback_active();
  view.routed_present = routed_playback_ != nullptr;
  view.routed_stopping = routed_stop_completion_ != nullptr;
  auto* const run = routed_run();
  view.routed_paused = run && run->paused();
  view.routed_output_streaming = options.include_output && run &&
                                 run->output_status().state == audio_io::OutputState::streaming;
  view.routed_preparing = routed_completion_ != nullptr;
  view.direct_preparing = playback_completion_ != nullptr;
  view.direct_playing = playback_ && playback_->playing();
  view.audio_edit_stopping = audio_edit_completion_ && audio_edit_completion_->stopping_only;
  if (options.include_graph && routed_playback_)
    view.graph = observed_routed_graph_;
  else if (options.include_direct_graph && view.direct_playing)
    view.graph = playback_->status();
  if (options.include_fault && run)
    view.plugin_fault = run->processor_fault();
  return view;
}

bool ProjectBridge::playback_paused() const {
  return transport_controller_.playback_paused(playback_view());
}

QVariantMap ProjectBridge::playback_state() const {
  return transport_controller_.playback_state(
      playback_view({.include_graph = true, .include_direct_graph = true, .include_fault = true}));
}

bool ProjectBridge::routed_graph_current() const {
  return transport_controller_.routed_graph_current(
      playback_view({.include_graph = true, .include_output = true}));
}

bool ProjectBridge::playback_revision_inactive() const {
  return transport_controller_.playback_revision_inactive(
      playback_view({.include_graph = true, .include_direct_graph = true}));
}

bool ProjectBridge::playback_pause_supported() const {
  return transport_controller_.playback_pause_supported(playback_view());
}

qulonglong ProjectBridge::playhead_sample() const {
  return static_cast<qulonglong>(playhead_sample_);
}

double ProjectBridge::playhead_bar() const {
  return transport_controller_.playhead_bar(session_ ? &session_->snapshot() : nullptr,
                                            playhead_sample_);
}

double ProjectBridge::clip_playhead_ticks(const QString& clip_id) const {
  constexpr double outside = -1.0;
  if (!session_ || !playback_active())
    return outside;
  const auto id = project::ClipId::parse(clip_id.toStdString());
  if (!id)
    return outside;
  const auto& snapshot = session_->snapshot();
  const project::MidiClip* clip = nullptr;
  for (const auto& track : snapshot.tracks) {
    const auto found = std::ranges::find(track.midi_clips, id.value(), &project::MidiClip::id);
    if (found != track.midi_clips.end()) {
      clip = &*found;
      break;
    }
  }
  if (clip == nullptr)
    return outside;
  const auto now = snapshot.tempo.sample_to_tick(
      timeline::SamplePosition{static_cast<std::int64_t>(playhead_sample_)}, snapshot.sample_rate);
  if (!now)
    return outside;
  std::int64_t begin = 0;
  std::int64_t end = 0;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip->placement)) {
    begin = musical->start.value;
    end = musical->end.value;
  } else if (const auto* elapsed = std::get_if<project::ElapsedInterval>(&clip->placement)) {
    const auto from = snapshot.tempo.sample_to_tick(elapsed->start, snapshot.sample_rate);
    const auto to = snapshot.tempo.sample_to_tick(elapsed->end, snapshot.sample_rate);
    if (!from || !to)
      return outside;
    begin = from.value().value;
    end = to.value().value;
  }
  const auto tick = now.value().value;
  if (tick < begin || tick >= end)
    return outside;
  auto inside = tick - begin;
  const auto pattern = clip->pattern_length.value;
  if (clip->loop && pattern > 0)
    inside %= pattern;
  return static_cast<double>(inside);
}

double ProjectBridge::playhead_seconds() const {
  return transport_controller_.playhead_seconds(session_ ? &session_->snapshot() : nullptr,
                                                playhead_sample_);
}

double ProjectBridge::elapsed_seconds_at_bar(double zero_based_bar) const {
  return transport_controller_.elapsed_seconds_at_bar(session_ ? &session_->snapshot() : nullptr,
                                                      zero_based_bar);
}

bool ProjectBridge::control_busy() const {
  return audio_edit_completion_ != nullptr || managed_media_busy();
}

void ProjectBridge::set_playback_status(QString value) {
  OSCILLATE_TRACE_SPAN("ui.playback_changed");
  playback_status_ = std::move(value);
  if (clip_warp_)
    clip_warp_->refresh_playback();
  emit playbackChanged();
}

void ProjectBridge::begin_routed_playback() {
  observed_routed_graph_.reset();
  routed_stop_reason_.clear();
  routed_transport_failure_.clear();
  source_underrun_detail_.clear();
  auto completion = std::make_shared<RoutedCompletion>();
  auto token = std::make_shared<std::optional<application::ControlTransportGate::PlaybackToken>>();
  const auto root = waveform_root_;
  auto snapshot = session_->snapshot();
  routed_limiter_active_ = std::ranges::any_of(snapshot.master_effects, [](const auto& slot) {
    const auto* patch = std::get_if<project::NativeEffectPatch>(&slot.processor);
    return slot.enabled && patch && patch->engine == "ceiling";
  });
  auto reservation = session_->reserve_playback_start();
  if (!reservation) {
    routed_limiter_active_ = false;
    set_playback_status(QStringLiteral("Routed playback cannot start: %1")
                            .arg(QString::fromStdString(reservation.error().message)));
    return;
  }
  token->emplace(std::move(reservation).value());
  const auto target_serial = audio_output_->target_serial();
  const auto requested_buffer_frames = audio_output_->buffer_frames();
  const auto first_sample = playhead_sample_;
  publish_click_schedule();
  try {
    routed_worker_ = std::jthread([root, snapshot = std::move(snapshot), token, completion,
                                   metronome = metronome_, factory = audio_output_->factory(),
                                   target_serial, requested_buffer_frames, first_sample,
                                   qualification = vst3_live_qualification_]() mutable {
      try {
        completion->result.emplace(RoutedPlayback::prepare_and_start(
            root, std::move(snapshot), std::move(**token), std::move(factory), target_serial,
            requested_buffer_frames, first_sample, std::move(qualification), std::move(metronome)));
        if (completion->result->has_value() &&
            completion->stop_requested.load(std::memory_order_acquire)) {
          completion->result->value()->stop_and_retire();
          completion->result.emplace(foundation::Result<std::unique_ptr<RoutedPlayback>>::failure(
              {foundation::ErrorCode::conflict, "Routed playback preparation stopped"}));
        }
      } catch (const std::exception& error) {
        (*token)->finish_after_join();
        completion->result.emplace(foundation::Result<std::unique_ptr<RoutedPlayback>>::failure(
            {foundation::ErrorCode::internal,
             std::string("Routed playback preparation failed: ") + error.what()}));
      } catch (...) {
        (*token)->finish_after_join();
        completion->result.emplace(foundation::Result<std::unique_ptr<RoutedPlayback>>::failure(
            {foundation::ErrorCode::internal, "Routed playback preparation failed"}));
      }
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    (*token)->finish_after_join();
    routed_limiter_active_ = false;
    set_playback_status(
        QStringLiteral("Cannot start routed playback worker: %1").arg(error.what()));
    return;
  }
  routed_completion_ = std::move(completion);
  set_playback_status(
      QStringLiteral("Preparing routed audio from model revision %1").arg(revision_));
  playback_timer_.start();
}

void ProjectBridge::begin_async_routed_stop(bool graceful) {
  if (!routed_playback_ || routed_stop_completion_) {
    return;
  }
  deferred_routed_transport_.reset();
  auto completion = std::make_shared<RoutedStopCompletion>();
  auto* run = routed_playback_.get();
  try {
    routed_stop_worker_ = std::jthread([completion, run, graceful] {
      if (graceful)
        run->begin_graceful_stop();
      completion->drain = run->stop_and_retire();
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    close_after_playback_ = false;
    set_playback_status(QStringLiteral("Cannot stop routed playback on a worker: %1. Retry Stop.")
                            .arg(error.what()));
    return;
  }
  routed_stop_completion_ = std::move(completion);
  set_playback_status(QStringLiteral("Stopping routed playback"));
  playback_timer_.start();
}

void ProjectBridge::begin_async_routed_transport(RoutedTransportOperation operation,
                                                 std::uint64_t sample) {
  if (!routed_playback_ || routed_stop_completion_ || routed_transport_completion_)
    return;
  if (operation != RoutedTransportOperation::resume &&
      routed_playback_->graph_handoff_in_progress()) {
    deferred_routed_transport_ = std::pair{operation, sample};
    set_playback_status(QStringLiteral("Completing the audio edit before changing transport"));
    playback_timer_.setInterval(5);
    playback_timer_.start();
    return;
  }
  deferred_routed_transport_.reset();
  auto completion = std::make_shared<RoutedTransportCompletion>();
  completion->operation = operation;
  completion->sample = sample;
  auto* run = routed_playback_.get();
  try {
    routed_transport_worker_ = std::jthread([completion, run, operation, sample] {
      try {
        foundation::Result<void> result = foundation::Result<void>::success();
        if (operation == RoutedTransportOperation::pause) {
          result = run->pause();
        } else if (operation == RoutedTransportOperation::resume) {
          result = run->resume();
        } else {
          const bool was_playing = !run->paused();
          if (was_playing)
            result = run->pause();
          if (result)
            result = run->seek_paused(sample);
          if (result && was_playing)
            result = run->resume();
        }
        completion->result.emplace(std::move(result));
      } catch (const std::exception& error) {
        completion->result.emplace(foundation::Result<void>::failure(
            {foundation::ErrorCode::internal,
             std::string("Routed transport change failed: ") + error.what()}));
      } catch (...) {
        completion->result.emplace(foundation::Result<void>::failure(
            {foundation::ErrorCode::internal, "Routed transport change failed"}));
      }
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    set_playback_status(QStringLiteral("Cannot change routed transport: %1").arg(error.what()));
    return;
  }
  routed_transport_completion_ = std::move(completion);
  set_playback_status(QStringLiteral("Changing routed transport"));
  playback_timer_.start();
}

void ProjectBridge::toggle_pause() {
  // Play takes the device from live monitoring: the monitor stops (or, still
  // preparing, stops when ready and then starts playback).
  if (monitor_ || monitor_completion_) {
    if (!stop_monitor_now()) {
      play_after_monitor_ = true;
      return;
    }
  }
  using Action = TransportController::PauseAction;
  const auto action = transport_controller_.pause_action(
      session_ ? &session_->snapshot() : nullptr,
      {.transport_pending = routed_transport_completion_ != nullptr,
       .routed_stopping = routed_stop_completion_ != nullptr,
       .deferred = deferred_routed_transport_.has_value(),
       .routed_preparing = routed_completion_ != nullptr,
       .routed_present = routed_playback_ != nullptr,
       .routed_paused = routed_run() && routed_run()->paused(),
       .direct_playing = playback_ && playback_->playing(),
       .playback_enabled = playback_enabled(),
       .direct_present = playback_ != nullptr,
       .device_available = audio_output_->selected_audio_device_available()});
  switch (action) {
  case Action::none:
    return;
  case Action::cancel_deferred:
    deferred_routed_transport_.reset();
    set_playback_status(QStringLiteral("Continuing playback"));
    return;
  case Action::toggle_playback:
    toggle_playback();
    return;
  case Action::pause_routed:
    begin_async_routed_transport(RoutedTransportOperation::pause);
    return;
  case Action::resume_routed:
    begin_async_routed_transport(RoutedTransportOperation::resume);
    return;
  case Action::direct_unavailable:
    set_playback_status(QStringLiteral("Pause is unavailable for this instrument playback path"));
    return;
  case Action::start_routed:
    begin_routed_playback();
    return;
  case Action::device_unavailable:
    set_playback_status(QStringLiteral("Selected audio output is unavailable. Refresh outputs "
                                       "or choose System default."));
    return;
  }
}

void ProjectBridge::stop_playback() {
  // Stop silences the browser's previews too (a sample loop playing in
  // time with the song stops with it), even when nothing else is playing.
  stop_browser_preview();
  // Stop ends a recording take and any count-in still to come.
  if (recording_)
    recording_->stopped();
  if (metronome_)
    metronome_->cancel_count_in();
  using Action = TransportController::StopAction;
  const auto action = transport_controller_.stop_action(
      {.transport_pending = routed_transport_completion_ != nullptr,
       .routed_preparing = routed_completion_ != nullptr,
       .routed_present = routed_playback_ != nullptr,
       .routed_paused = routed_run() && routed_run()->paused(),
       .direct_playing = playback_ && playback_->playing(),
       .audio_edit_pending = audio_edit_completion_ != nullptr});
  switch (action) {
  case Action::none:
    // Stop while stopped goes back too (to the loop start, or bar 1).
    if (session_ && !playback_busy()) {
      playhead_sample_ = stop_sample();
      emit playbackChanged();
    }
    return;
  case Action::request_transport_stop:
    routed_transport_completion_->stop_requested.store(true, std::memory_order_release);
    set_playback_status(QStringLiteral("Stopping after transport change"));
    return;
  case Action::request_preparation_stop:
    routed_completion_->stop_requested.store(true, std::memory_order_release);
    set_playback_status(QStringLiteral("Stopping after playback preparation"));
    return;
  case Action::stop_routed_gracefully:
  case Action::stop_routed_immediately:
    deferred_routed_transport_.reset();
    begin_async_routed_stop(action == Action::stop_routed_gracefully);
    return;
  case Action::stop_direct:
    playhead_sample_ = stop_sample();
    toggle_playback();
    return;
  }
}

void ProjectBridge::seek_to_bar(double zero_based_bar) {
  const auto sample = transport_controller_.sample_at_bar(
      session_ ? &session_->snapshot() : nullptr, zero_based_bar);
  if (sample)
    seek_to_sample(static_cast<qulonglong>(*sample));
}

void ProjectBridge::seek_to_sample(qulonglong sample) {
  using SeekAction = TransportController::SeekAction;
  const auto action = transport_controller_.seek_action(
      sample, {.project_open = session_ != nullptr,
               .transport_busy = routed_completion_ || routed_stop_completion_ ||
                                 routed_transport_completion_ || audio_edit_completion_ ||
                                 playback_completion_,
               .routed_playback = routed_playback_ != nullptr,
               .direct_playing = playback_ && playback_->playing()});
  switch (action) {
  case SeekAction::ignore:
    return;
  case SeekAction::wait_for_transport:
    set_playback_status(QStringLiteral("Wait for the current transport change before seeking"));
    return;
  case SeekAction::seek_routed:
    begin_async_routed_transport(RoutedTransportOperation::seek, sample);
    return;
  case SeekAction::direct_unavailable:
    set_playback_status(QStringLiteral("Seek is unavailable for this instrument playback path"));
    return;
  case SeekAction::park_playhead:
    playhead_sample_ = sample;
    set_playback_status(QStringLiteral("Playhead set to sample %1").arg(sample));
    return;
  }
}

void ProjectBridge::toggle_playback() {
  if (routed_transport_completion_) {
    routed_transport_completion_->stop_requested.store(true, std::memory_order_release);
    return;
  }
  if (routed_completion_) {
    routed_completion_->stop_requested.store(true, std::memory_order_release);
    set_playback_status(QStringLiteral("Stopping routed playback after preparation"));
    return;
  }
  if (routed_playback_) {
    deferred_routed_transport_.reset();
    if (auto* const run = routed_run())
      begin_async_routed_stop(!run->paused());
    return;
  }
  if (audio_edit_completion_) {
    audio_edit_completion_->stop_requested.store(true, std::memory_order_release);
    set_playback_status(QStringLiteral("Stopping after the audio edit handoff"));
    return;
  }
  if (!playback_enabled() || !playback_) {
    return;
  }
  if (!playback_->playing() && !audio_output_->selected_audio_device_available()) {
    set_playback_status(QStringLiteral("Selected audio output is unavailable. Refresh outputs or "
                                       "choose System default."));
    return;
  }
  if (!playback_->playing())
    source_underrun_detail_.clear();
  exports_->clear_status();
  const auto& snapshot = session_->snapshot();
  const bool bound_instrument = std::ranges::any_of(snapshot.tracks, [](const auto& track) {
    return track.kind == project::TrackKind::instrument &&
           std::holds_alternative<project::NativeInstrumentPatch>(track.instrument_source);
  });
  if (!playback_->playing() &&
      (project::requires_routed_rendering(snapshot) ||
       project::requires_automation_rendering(snapshot) || has_saved_vst3_insert(snapshot) ||
       has_saved_track_effect(snapshot) || has_rate_converted_audio(snapshot) ||
       has_processed_audio(snapshot) || project::has_frozen_track(snapshot) ||
       // The direct path has no master effects: any master chain plays routed.
       bound_instrument || !snapshot.master_effects.empty())) {
    begin_routed_playback();
    return;
  }
  if (playback_->playing()) {
    playback_->stop();
    set_playback_status({});
    if (playback_->retiring()) {
      playback_timer_.start();
    } else {
      playback_timer_.stop();
    }
    return;
  }
  audio_edit_failure_.clear();
  auto task = playback_->prepare_start();
  if (!task) {
    set_playback_status(QString::fromStdString(task.error().message));
    return;
  }
  auto completion = std::make_shared<PlaybackCompletion>();
  try {
    playback_worker_ = std::jthread([task = std::move(task).value(), completion]() mutable {
      try {
        completion->result.emplace(std::move(task).run());
      } catch (const std::exception& error) {
        completion->result.emplace(
            foundation::Result<application::PreparedInstrumentPlayback>::failure(
                {foundation::ErrorCode::internal,
                 std::string("Playback preparation failed: ") + error.what()}));
      } catch (...) {
        completion->result.emplace(
            foundation::Result<application::PreparedInstrumentPlayback>::failure(
                {foundation::ErrorCode::internal, "Playback preparation failed unexpectedly"}));
      }
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    set_playback_status(QStringLiteral("Cannot start playback preparation: %1").arg(error.what()));
    return;
  }
  playback_completion_ = std::move(completion);
  set_playback_status(QStringLiteral("Preparing audio sources"));
  playback_timer_.start();
}

void ProjectBridge::begin_live_audio_edit() {
  audio_edit_failure_.clear();
  auto task = playback_->prepare_live_edit();
  if (!task) {
    // Preserve the accepted model edit and expose the old audible revision.
    (void)playback_->submit_current();
    const auto graph = playback_->status();
    audio_edit_failure_ = QString::fromStdString(task.error().message);
    set_playback_status(QStringLiteral("Audio edit revision %1 not active; playing revision %2: "
                                       "%3. Stop and Play to retry, or Undo.")
                            .arg(revision_)
                            .arg(graph.active_graph_revision)
                            .arg(QString::fromStdString(task.error().message)));
    return;
  }
  auto completion = std::make_shared<AudioEditCompletion>();
  auto* playback = playback_.get();
  const auto hook = before_live_edit_handoff_;
  auto* session = session_.get();
  try {
    audio_edit_worker_ =
        audio_edit_runner_([task = std::make_shared<application::InstrumentPlaybackPreparation>(
                                std::move(task).value()),
                            completion, playback, hook, session] {
          try {
            auto prepared = std::move(*task).run();
            if (!prepared) {
              completion->result.emplace(foundation::Result<void>::failure(prepared.error()));
            } else if (completion->stop_requested.load(std::memory_order_acquire)) {
              playback->stop();
              completion->result.emplace(foundation::Result<void>::success());
            } else {
              if (hook) {
                hook(*session, prepared.value());
              }
              completion->result.emplace(
                  playback->restart_prepared_after_edit(std::move(prepared).value()));
            }
          } catch (const std::exception& error) {
            completion->result.emplace(foundation::Result<void>::failure(
                {foundation::ErrorCode::internal,
                 std::string("Audio edit handoff failed: ") + error.what()}));
          } catch (...) {
            completion->result.emplace(foundation::Result<void>::failure(
                {foundation::ErrorCode::internal, "Audio edit handoff failed unexpectedly"}));
          }
          if (completion->stop_requested.load(std::memory_order_acquire)) {
            playback->stop();
          }
          completion->done.store(true, std::memory_order_release);
        });
  } catch (const std::exception& error) {
    (void)playback_->submit_current();
    const auto graph = playback_->status();
    audio_edit_failure_ = QString::fromLatin1(error.what());
    set_playback_status(
        QStringLiteral("Audio edit revision %1 not active; playing revision %2: could not start "
                       "handoff: %3. Stop and Play to retry, or Undo.")
            .arg(revision_)
            .arg(graph.active_graph_revision)
            .arg(error.what()));
    return;
  }
  audio_edit_completion_ = std::move(completion);
  set_playback_status(
      QStringLiteral("Audio edit revision %1 pending; previous audio remains active")
          .arg(revision_));
  playback_timer_.start();
}

void ProjectBridge::submit_audio_clip_edit() {
  if (routed_playback_) {
    submit_playback_edit();
  } else if (playback_ && playback_->playing()) {
    if (has_processed_audio(session_->snapshot())) {
      set_playback_status(
          QStringLiteral("Edit revision %1 saved but not audible: this playback path cannot "
                         "render warp or pitch. Stop and Play to hear it.")
              .arg(session_->snapshot().revision));
      return;
    }
    begin_live_audio_edit();
  }
}

void ProjectBridge::begin_async_playback_stop() {
  auto completion = std::make_shared<AudioEditCompletion>();
  completion->stopping_only = true;
  auto* playback = playback_.get();
  try {
    audio_edit_worker_ = std::jthread([completion, playback] {
      playback->stop();
      completion->result.emplace(foundation::Result<void>::success());
      completion->done.store(true, std::memory_order_release);
    });
  } catch (const std::exception& error) {
    close_after_playback_ = false;
    stop_worker_start_failed_ = true;
    playback_timer_.stop();
    set_playback_status(
        QStringLiteral("Could not stop playback on a worker: %1. Select Stop to retry.")
            .arg(error.what()));
    return;
  }
  audio_edit_completion_ = std::move(completion);
  set_playback_status(QStringLiteral("Stopping playback"));
  playback_timer_.start();
}

void ProjectBridge::poll_live_audio_edit() {
  if (!audio_edit_completion_ || !audio_edit_completion_->done.load(std::memory_order_acquire)) {
    return;
  }
  if (audio_edit_worker_.joinable())
    audio_edit_worker_.join();
  auto completion = std::move(audio_edit_completion_);
  refresh();
  const bool stop_requested = completion->stop_requested.load(std::memory_order_acquire);
  if (stop_requested && playback_->playing()) {
    begin_async_playback_stop();
    return;
  }
  if (completion->stopping_only || stop_requested) {
    set_playback_status(
        QStringLiteral("Playback stopped; model revision %1 retained").arg(revision_));
  } else if (!completion->result || !*completion->result) {
    if (playback_->playing()) {
      (void)playback_->submit_current();
    }
    const auto graph = playback_->status();
    const auto reason = completion->result
                            ? QString::fromStdString(completion->result->error().message)
                            : QStringLiteral("unknown handoff failure");
    audio_edit_failure_ = reason;
    set_playback_status(
        playback_->playing()
            ? QStringLiteral("Audio edit revision %1 not active; playing revision %2: %3. "
                             "Stop and Play to retry, or Undo.")
                  .arg(revision_)
                  .arg(graph.active_graph_revision)
                  .arg(reason)
            : QStringLiteral("Playback stopped; audio edit revision %1 not active; last graph "
                             "revision %2: %3. Select Play to retry, or Undo.")
                  .arg(revision_)
                  .arg(graph.active_graph_revision)
                  .arg(reason));
  } else {
    audio_edit_failure_.clear();
    set_playback_status(
        QStringLiteral("Audio edit revision %1 accepted; re-priming from the completed sample")
            .arg(revision_));
  }
  emit playbackChanged();
  if (close_after_playback_ && !playback_->playing() && !playback_->retiring()) {
    playback_timer_.stop();
    emit readyToClose();
  }
}

void ProjectBridge::preview_track_level_pan(const QString& track_id, double level, double pan) {
  auto* const run = routed_run();
  if (!run)
    return;
  if (const auto track = project::TrackId::parse(track_id.toStdString()))
    (void)run->set_live_channel(track.value(), level, pan);
}

void ProjectBridge::preview_master_level(double level) {
  if (auto* const run = routed_run())
    (void)run->set_live_channel(std::nullopt, level, 0.0);
}

void ProjectBridge::preview_send_gain(const QString& send_id, double gain) {
  const auto send = project::SendId::parse(send_id.toStdString());
  auto* const run = routed_run();
  if (!run || !session_ || !send)
    return;
  for (const auto& track : session_->snapshot().tracks)
    if (std::ranges::any_of(track.sends,
                            [&](const auto& item) { return item.id == send.value(); })) {
      (void)run->set_live_send(track.id, send.value(), gain);
      return;
    }
}

void ProjectBridge::submit_playback_edit() {
  OSCILLATE_TRACE_SPAN("edit.submit");
  // A monitor run plays the project with its clips removed; it takes the
  // same edits, muted the same way.
  if (monitor_ && !routed_playback_) {
    auto snapshot = session_->snapshot();
    if (monitor_->status().active_graph_revision >= snapshot.audible_revision)
      return;
    for (auto& track : snapshot.tracks) {
      track.clips.clear();
      track.midi_clips.clear();
      track.frozen.reset();
    }
    (void)monitor_->submit_revision(std::move(snapshot));
    return;
  }
  if (routed_playback_) {
    // A stopping run takes no edits; the next Play prepares the saved one.
    auto* const run = routed_run();
    if (!run)
      return;
    const auto snapshot = session_->snapshot();
    foundation::trace::instant("edit.committed", snapshot.revision);
    const auto graph = run->status();
    // Names, colours and markers never need a new graph.
    if (graph.active_graph_revision >= snapshot.audible_revision)
      return;
    const auto submitted = run->submit_revision(snapshot);
    if (!submitted) {
      set_playback_status(
          QStringLiteral("Edit revision %1 saved but not active: %2. Stop and Play to retry.")
              .arg(snapshot.revision)
              .arg(QString::fromStdString(submitted.error().message)));
      return;
    }
    playback_timer_.setInterval(5);
    set_playback_status(
        QStringLiteral("Edit revision %1 not active; playing routed revision %2 "
                       "(limiter %3). Pending at an audio block boundary.")
            .arg(snapshot.revision)
            .arg(graph.active_graph_revision)
            .arg(routed_limiter_active_ ? QStringLiteral("active") : QStringLiteral("off")));
    return;
  }
  if (!playback_ || !playback_->playing() ||
      playback_->status().model_revision >= session_->snapshot().audible_revision) {
    return;
  }
  const auto submitted = playback_->submit_current();
  if (!submitted) {
    set_playback_status(QStringLiteral("Edit accepted but not active: %1. Stop and play to retry.")
                            .arg(QString::fromStdString(submitted.error().message)));
  }
}

void ProjectBridge::poll_routed_playback() {
  OSCILLATE_TRACE_SPAN("ui.poll_routed_playback");
  if (routed_transport_completion_) {
    if (!routed_transport_completion_->done.load(std::memory_order_acquire))
      return;
    routed_transport_worker_.join();
    auto completion = std::move(routed_transport_completion_);
    if (routed_playback_)
      playhead_sample_ = routed_playback_->completed_sample();
    if (close_after_playback_ || completion->stop_requested.load(std::memory_order_acquire)) {
      begin_async_routed_stop();
      return;
    }
    if (!completion->result || !*completion->result) {
      if (completion->result &&
          completion->result->error().code == foundation::ErrorCode::conflict && routed_playback_ &&
          routed_playback_->graph_handoff_in_progress()) {
        deferred_routed_transport_ = std::pair{completion->operation, completion->sample};
        set_playback_status(QStringLiteral("Completing the audio edit before changing transport"));
        playback_timer_.setInterval(5);
        return;
      }
      const auto detail = completion->result
                              ? QString::fromStdString(completion->result->error().message)
                              : QStringLiteral("transport worker returned no result");
      routed_transport_failure_ = detail;
      set_playback_status(
          QStringLiteral("Routed transport change failed: %1. Retry or Stop").arg(detail));
      return;
    }
    routed_transport_failure_.clear();
    set_playback_status(routed_playback_->paused()
                            ? QStringLiteral("Paused at sample %1").arg(playhead_sample_)
                            : QStringLiteral("Playing from sample %1").arg(playhead_sample_));
    return;
  }
  if (routed_completion_) {
    if (!routed_completion_->done.load(std::memory_order_acquire))
      return;
    routed_worker_.join();
    auto completion = std::move(routed_completion_);
    if (!completion->result || !*completion->result) {
      const auto reason = completion->result
                              ? QString::fromStdString(completion->result->error().message)
                              : QStringLiteral("preparation returned no result");
      set_playback_status(
          QStringLiteral("Routed model revision %1 not active: %2").arg(revision_).arg(reason));
      routed_limiter_active_ = false;
      if (close_after_playback_) {
        playback_timer_.stop();
        emit readyToClose();
      } else {
        playback_timer_.stop();
      }
      return;
    }
    const bool stop_requested = completion->stop_requested.load(std::memory_order_acquire);
    routed_playback_ = std::move(completion->result->value());
    emit playbackChanged();
    if (close_after_playback_ || stop_requested) {
      begin_async_routed_stop(stop_requested && !close_after_playback_);
      return;
    }
  }
  if (routed_stop_completion_) {
    if (!routed_stop_completion_->done.load(std::memory_order_acquire))
      return;
    routed_stop_worker_.join();
    const auto drain = routed_stop_completion_->drain;
    routed_stop_completion_.reset();
    routed_playback_.reset();
    vst3_editor_->refresh_live_status();
    playback_timer_.setInterval(50);
    // Stop goes back to the loop start (or bar 1); Pause resumes in place.
    playhead_sample_ = stop_sample();
    if (routed_stop_reason_.isEmpty()) {
      const auto label =
          drain == RoutedPlayback::StopDrainOutcome::completed
              ? QStringLiteral("Playback stopped; limiter lookahead rendered to output")
          : drain == RoutedPlayback::StopDrainOutcome::truncated
              ? QStringLiteral("Playback stopped; limiter lookahead incomplete (output or source "
                               "stalled or failed)")
          : drain == RoutedPlayback::StopDrainOutcome::instrument_release_failed
              ? QStringLiteral("Playback stopped; VST3 instrument note release failed")
              : QStringLiteral("Playback stopped");
      set_playback_status(label + QStringLiteral("; model revision %1 retained").arg(revision_));
    } else {
      set_playback_status(routed_stop_reason_ +
                          QStringLiteral("; model revision %1 retained").arg(revision_));
      routed_stop_reason_.clear();
    }
    routed_limiter_active_ = false;
    routed_transport_failure_.clear();
    if (close_after_playback_) {
      playback_timer_.stop();
      emit readyToClose();
    } else {
      playback_timer_.stop();
    }
    return;
  }
  if (!routed_playback_)
    return;
  playhead_sample_ = routed_playback_->completed_sample();
  if (const auto failure = routed_playback_->source_failure()) {
    routed_stop_reason_ =
        QStringLiteral("Routed source failed: %1").arg(QString::fromStdString(failure->message));
    begin_async_routed_stop();
    set_playback_status(routed_stop_reason_);
    return;
  }
  const auto processor_fault = routed_playback_->processor_fault();
  if (routed_playback_->paused()) {
    set_playback_status(routed_transport_failure_.isEmpty()
                            ? QStringLiteral("Paused at sample %1").arg(playhead_sample_)
                            : QStringLiteral("Routed transport change failed: %1. Retry or Stop")
                                  .arg(routed_transport_failure_));
    return;
  }
  const auto output = routed_playback_->output_status();
  const auto output_action = transport_controller_.routed_output_action(
      output, routed_playback_->sample_rate(), routed_playback_->prepared_block_limit());
  if (output_action == TransportController::OutputPollAction::recover_device) {
    routed_stop_reason_ = output.client_rate_mismatch
                              ? QStringLiteral("%1 rate %2 differs from project rate "
                                               "%3; routed playback stopped. Correct the server "
                                               "or project rate, then retry")
                                    .arg(audio_output_->jack() ? QStringLiteral("JACK server")
                                                               : QStringLiteral("PipeWire client"))
                                    .arg(output.negotiated_client_rate.value_or(0))
                                    .arg(output.requested_client_rate)
                          : audio_output_->jack()
                              ? QStringLiteral("JACK server disconnected; routed playback "
                                               "stopped. Restart JACK, reconnect output "
                                               "ports, then Play to retry")
                              : QStringLiteral("Audio device lost; routed playback stopped. "
                                               "Refresh outputs, then select Play to retry");
    begin_async_routed_stop();
    audio_output_->refresh_audio_devices();
    set_playback_status(routed_stop_reason_);
    return;
  }
  if (output_action == TransportController::OutputPollAction::stop_invalid_format) {
    routed_stop_reason_ = QStringLiteral("Routed output format differs from prepared project "
                                         "rate or block limit; playback stopped");
    begin_async_routed_stop();
    set_playback_status(routed_stop_reason_);
    return;
  }
  const auto graph = routed_playback_->status();
  observed_routed_graph_ = graph;
  vst3_editor_->refresh_live_status();
  if (deferred_routed_transport_) {
    if (routed_playback_->graph_handoff_in_progress()) {
      set_playback_status(QStringLiteral("Completing the audio edit before changing transport"));
      return;
    }
    const auto [operation, sample] = *deferred_routed_transport_;
    deferred_routed_transport_.reset();
    begin_async_routed_transport(operation, sample);
    return;
  }
  if (processor_fault) {
    set_playback_status(QStringLiteral("Routed plugin fault: %1")
                            .arg(QString::fromStdString(processor_fault->message)));
    return;
  }
  playback_timer_.setInterval(routed_playback_->edit_pending() ? 5 : 50);
  for (unsigned index = 0; index < 16; ++index) {
    const auto event = routed_playback_->pop_source_underrun();
    if (!event)
      break;
    source_underrun_detail_ = underrun_detail(*event);
  }
  const auto model_revision = session_->snapshot().revision;
  const auto audible_revision = session_->snapshot().audible_revision;
  const auto source_missing_frames = graph.active_graph_revision >= audible_revision
                                         ? routed_playback_->source_status().missing_frames
                                         : 0;
  set_playback_status(transport_controller_.routed_running_status(
      {.graph = graph,
       .output = output,
       .model_revision = model_revision,
       .audible_revision = audible_revision,
       .completed_sample = routed_playback_->completed_sample(),
       .source_missing_frames = source_missing_frames,
       .edit_pending = routed_playback_->edit_pending(),
       .limiter_active = routed_limiter_active_,
       .source_underrun_detail = source_underrun_detail_}));
}

void ProjectBridge::poll_playback() {
  OSCILLATE_TRACE_SPAN("ui.poll_playback");
  if (routed_completion_ || routed_playback_ || routed_stop_completion_ ||
      routed_transport_completion_) {
    poll_routed_playback();
    if (native_synth_controller_)
      native_synth_controller_->refresh();
    if (native_synth_automation_)
      native_synth_automation_->refresh_playback_status();
    if (native_effect_controller_)
      native_effect_controller_->refresh();
    if (native_effect_automation_)
      native_effect_automation_->refresh_playback_status();
    if (clip_warp_)
      clip_warp_->refresh_playback();
    if (loop_transport_)
      loop_transport_->poll();
    return;
  }
  if (audio_edit_completion_) {
    poll_live_audio_edit();
    if (stop_worker_start_failed_) {
      stop_worker_start_failed_ = false;
      return;
    }
    if (audio_edit_completion_) {
      return;
    }
    if (close_after_playback_) {
      if (playback_->poll_retirement()) {
        playback_timer_.stop();
        emit readyToClose();
      }
      return;
    }
  }
  if (playback_completion_) {
    if (!playback_completion_->done.load(std::memory_order_acquire)) {
      return;
    }
    if (playback_worker_.joinable()) {
      playback_worker_.join();
    }
    auto completion = std::move(playback_completion_);
    emit playbackChanged();
    if (close_after_playback_) {
      playback_timer_.stop();
      emit readyToClose();
      return;
    }
    auto started = foundation::Result<void>::failure(
        {foundation::ErrorCode::internal, "Playback preparation incomplete"});
    if (completion->result) {
      const auto target_serial = audio_output_->target_serial();
      started =
          completion->result->has_value()
              ? playback_->start_prepared(std::move(completion->result->value()), target_serial)
              : foundation::Result<void>::failure(completion->result->error());
    }
    if (!started) {
      if (!playback_->retiring()) {
        playback_timer_.stop();
      }
      set_playback_status(QString::fromStdString(started.error().message));
      return;
    }
    set_playback_status(QStringLiteral("Preparing audio output"));
  }
  if (!playback_->playing()) {
    if (playback_->poll_retirement()) {
      playback_timer_.stop();
      emit playbackChanged();
      if (close_after_playback_) {
        emit readyToClose();
      }
    }
    return;
  }
  auto ready = playback_->poll_start();
  if (!ready) {
    if (!playback_->retiring()) {
      playback_timer_.stop();
    }
    const auto graph = playback_->status();
    set_playback_status(QStringLiteral("Playback revision %1 not active; last graph revision %2: "
                                       "%3. Select Play to retry, or Undo.")
                            .arg(graph.model_revision)
                            .arg(graph.active_graph_revision)
                            .arg(QString::fromStdString(ready.error().message)));
    return;
  }
  if (!ready.value()) {
    set_playback_status(QStringLiteral("Priming audio clips"));
    return;
  }
  playback_->observe();
  playhead_sample_ = playback_->completed_sample();
  if (const auto failure = playback_->source_failure()) {
    playback_->stop();
    set_playback_status(
        QStringLiteral("Audio source failed: %1").arg(QString::fromStdString(failure->message)));
    return;
  }
  const auto output = playback_->output_status();
  if (transport_controller_.direct_output_action(output) ==
      TransportController::OutputPollAction::recover_device) {
    const auto reason =
        output.client_rate_mismatch
            ? QStringLiteral("%1 rate %2 differs from project rate %3; playback stopped. "
                             "Correct the server or project rate, then retry")
                  .arg(audio_output_->jack() ? QStringLiteral("JACK server")
                                             : QStringLiteral("PipeWire client"))
                  .arg(output.negotiated_client_rate.value_or(0))
                  .arg(output.requested_client_rate)
        : audio_output_->jack()
            ? QStringLiteral("JACK server disconnected; playback stopped. Restart JACK, "
                             "reconnect output ports, then Play to retry.")
            : QStringLiteral("Audio device lost; playback stopped. Refresh "
                             "outputs, choose an output, then select Play to retry.");
    playback_->stop();
    audio_output_->refresh_audio_devices();
    set_playback_status(reason);
    return;
  }
  const auto graph = playback_->status();
  for (unsigned index = 0; index < 16; ++index) {
    const auto event = playback_->pop_source_underrun();
    if (!event)
      break;
    source_underrun_detail_ = underrun_detail(*event);
  }
  const bool graph_failed =
      graph.application == application::PlaybackApplication::failed && graph.failure.has_value();
  const auto source_underruns = graph_failed ? 0 : playback_->source_underruns();
  const auto status = transport_controller_.direct_running_status(
      {.graph = graph,
       .output = output,
       .completed_sample = playback_->completed_sample(),
       .source_underruns = source_underruns,
       .audio_edit_failure = audio_edit_failure_,
       .source_underrun_detail = source_underrun_detail_});
  if (status)
    set_playback_status(*status);
}

bool ProjectBridge::request_close() {
  autosave_timer_.stop();
  background_save_timer_.stop();
  if (background_save_job_.running()) {
    close_after_background_save_ = true;
    background_save_poll_timer_.start();
    set_status(QStringLiteral("Finishing project save before closing"));
    return false;
  }
  if (autosave_job_.running()) {
    close_after_autosave_ = true;
    autosave_poll_timer_.start();
    set_status(QStringLiteral("Finishing autosave before closing"));
    return false;
  }
  if (!plugin_state_saved([this] {
        if (request_close())
          emit readyToClose();
      }))
    return false;
  vst3_editor_->discard_vst3_insert_editor();
  if (managed_media_busy()) {
    managed_media_->cancel_for_close();
    return false;
  }
  if (vst3_browser_->close_when_done())
    return false;
  waveform_->cancel_work();
  if (audio_edit_completion_) {
    close_after_playback_ = true;
    audio_edit_completion_->stop_requested.store(true, std::memory_order_release);
    set_playback_status(QStringLiteral("Finishing and stopping audio edit before closing"));
    playback_timer_.start();
    return false;
  }
  if (import_busy()) {
    imports_->audio().cancel_for_close();
    return false;
  }
  if (exports_->close_when_done())
    return false;
  if (routed_completion_ || routed_playback_ || routed_stop_completion_ ||
      routed_transport_completion_) {
    close_after_playback_ = true;
    if (routed_completion_)
      routed_completion_->stop_requested.store(true, std::memory_order_release);
    if (routed_transport_completion_)
      routed_transport_completion_->stop_requested.store(true, std::memory_order_release);
    if (routed_playback_ && !routed_stop_completion_ && !routed_transport_completion_)
      begin_async_routed_stop();
    playback_timer_.start();
    return false;
  }
  if (!playback_busy()) {
    return save_current_project_if_dirty();
  }
  close_after_playback_ = true;
  if (playback_ && playback_->playing()) {
    playback_->stop();
  }
  set_playback_status(QStringLiteral("Stopping playback before closing"));
  playback_timer_.start();
  return false;
}

} // namespace oscillate::quick

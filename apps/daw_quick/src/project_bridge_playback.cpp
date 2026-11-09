#include "project_bridge.hpp"

#include "clip_warp_controller.hpp"
#include "loop_transport.hpp"
#include "native_synth_automation_controller.hpp"
#include "native_synth_device_controller.hpp"
#include "oscillate/foundation/trace.hpp"
#include "waveform_controller.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

// Playback while it runs: the UI-thread polls, edits submitted to the
// running graph, level and send previews, and closing after playback.
namespace oscillate::quick {
namespace {

QString underrun_detail(const media::UnderrunEvent& event) {
  return QStringLiteral("; clip %1 missing source frames %2–%3 (generation %4)")
      .arg(QString::fromStdString(event.clip.to_string()))
      .arg(event.first_source_frame)
      .arg(event.first_source_frame + event.missing_frames)
      .arg(event.generation);
}

} // namespace

foundation::Result<void> ProjectBridge::hand_off_live_audio_edit(AudioEditCompletion& completion) {
  if (!completion.result)
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::internal, "Audio edit preparation returned no result"});
  if (!*completion.result)
    return foundation::Result<void>::failure(completion.result->error());
  try {
    if (before_live_edit_handoff_)
      before_live_edit_handoff_(*session_, completion.result->value());
    return playback_->restart_prepared_after_edit(std::move(*completion.result).value());
  } catch (const std::exception& error) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::internal,
         std::string("Audio edit handoff failed: ") + error.what()});
  } catch (...) {
    return foundation::Result<void>::failure(
        {foundation::ErrorCode::internal, "Audio edit handoff failed unexpectedly"});
  }
}

void ProjectBridge::poll_live_audio_edit() {
  if (!audio_edit_completion_ || !audio_edit_completion_->done.load(std::memory_order_acquire)) {
    return;
  }
  if (audio_edit_worker_.joinable())
    audio_edit_worker_.join();
  const auto completion = std::move(audio_edit_completion_);
  if (completion->stop_requested) {
    // Stop (or close) won over the prepared edit: it is never installed.
    playback_->stop();
    refresh();
    set_playback_status(
        QStringLiteral("Playback stopped; model revision %1 retained").arg(revision_));
    emit playbackChanged();
    return;
  }
  const auto handed_off = hand_off_live_audio_edit(*completion);
  refresh();
  if (!handed_off) {
    if (playback_->playing()) {
      (void)playback_->submit_current();
    }
    const auto graph = playback_->status();
    const auto reason = QString::fromStdString(handed_off.error().message);
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
  // A live edit retires the clip workers it replaced: join them as they
  // finish, so the next edit is not held behind them.
  (void)playback_->poll_retirement();
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
    audio_edit_completion_->stop_requested = true;
    set_playback_status(QStringLiteral("Stopping once the audio edit is prepared, then closing"));
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
